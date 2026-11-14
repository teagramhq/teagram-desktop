/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ui_stall_watchdog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#if defined(Q_OS_MAC)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#endif // Q_OS_MAC
#if defined(Q_OS_MAC) && defined(__arm64__)
#include <ptrauth.h>
#endif // Q_OS_MAC && __arm64__
#endif // Q_OS_LINUX || Q_OS_MAC

namespace Core {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kCountThreshold = 50ms;
constexpr auto kPingInterval = 50ms;
constexpr auto kLogThreshold = 100ms;
constexpr auto kLogRateLimit = 10s;
constexpr auto kShortPingInterval = 10ms;
constexpr auto kSchedulerTracePingInterval = 2ms;
constexpr auto kShortLogThreshold = 40ms;
constexpr auto kSchedulerSampleInterval = 1ms;
constexpr auto kMaxSchedulerSampleGap = 5ms;
constexpr auto kMaxSchedulerSamples = 8192U;
constexpr auto kMaxSchedulerHistorySamples = 65'536U;
constexpr auto kMaxFrames = 32U;
constexpr auto kMaxFrameCharacters = 160;
constexpr auto kMaxPingAcknowledgements = 32'768U;

std::atomic<quint64> TotalStallCount = 0;
std::mutex StallEvidenceMutex;
std::vector<UiStallWatchdog::StallEvidence> StallEvidenceHistory;
std::deque<UiStallWatchdog::PingAcknowledgementEvidence>
	PingAcknowledgementHistory;
std::mutex SchedulerSampleHistoryMutex;
std::deque<UiStallWatchdog::SchedulerSampleEvidence>
	SchedulerSampleHistory;

#if defined(Q_OS_MAC)
[[nodiscard]] std::uint64_t SchedulerSampleIntervalMachTicks() {
	static const auto result = [] {
		auto timebase = mach_timebase_info_data_t();
		const auto status = mach_timebase_info(&timebase);
		if ((status != KERN_SUCCESS) || !timebase.numer || !timebase.denom) {
			return std::uint64_t(0);
		}
		const auto intervalNanoseconds
			= std::chrono::duration_cast<std::chrono::nanoseconds>(
				  kSchedulerSampleInterval)
				  .count();
		const auto numerator = std::uint64_t(timebase.numer);
		const auto nanoseconds
			= std::uint64_t(intervalNanoseconds) * timebase.denom;
		return (nanoseconds + numerator - 1) / numerator;
	}();
	return result;
}
#endif // Q_OS_MAC

#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)

struct StackBounds final {
	std::uintptr_t low = 0;
	std::uintptr_t high = 0;
};

struct RegisterState final {
	std::uintptr_t instruction = 0;
	std::uintptr_t frame = 0;
};

std::array<void *, kMaxFrames> CapturedFrames = {};
std::atomic_uint CapturedFrameCount = 0;
std::atomic_uint RequestedCapture = 0;
std::atomic_uint CompletedCapture = 0;
std::atomic_uint CaptureSequence = 0;
pthread_t MainThread = {};
#if defined(Q_OS_MAC)
thread_t MainMachThread = MACH_PORT_NULL;
#endif // Q_OS_MAC
StackBounds MainStack;
struct sigaction PreviousSignalAction = {};
std::mutex SignalInstallMutex;
bool SignalInstalled = false;

static_assert(std::atomic_uint::is_always_lock_free);

[[nodiscard]] char MainThreadSchedulerState() noexcept {
#if defined(Q_OS_MAC)
	if (MainMachThread == MACH_PORT_NULL) {
		return '?';
	}
	auto info = thread_basic_info_data_t();
	auto count = mach_msg_type_number_t(THREAD_BASIC_INFO_COUNT);
	if (thread_info(MainMachThread, THREAD_BASIC_INFO,
					reinterpret_cast<thread_info_t>(&info), &count)
		!= KERN_SUCCESS) {
		return '?';
	}
	switch (info.run_state) {
	case TH_STATE_RUNNING:
		return 'R';
	case TH_STATE_WAITING:
		return 'W';
	case TH_STATE_UNINTERRUPTIBLE:
		return 'U';
	case TH_STATE_STOPPED:
		return 'S';
	case TH_STATE_HALTED:
		return 'H';
	default:
		return '?';
	}
#else  // Q_OS_MAC
	return '?';
#endif // else for Q_OS_MAC
}

[[nodiscard]] StackBounds CurrentStackBounds(pthread_t thread) {
#ifdef Q_OS_MAC
	const auto high
		= reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
	const auto size = pthread_get_stacksize_np(thread);
	return high > size ? StackBounds{high - size, high} : StackBounds();
#else  // Q_OS_MAC
	auto attributes = pthread_attr_t();
	if (pthread_getattr_np(thread, &attributes) != 0) {
		return {};
	}
	void *address = nullptr;
	auto size = std::size_t();
	const auto result = pthread_attr_getstack(&attributes, &address, &size);
	const auto destroyResult = pthread_attr_destroy(&attributes);
	return result == 0 && destroyResult == 0 && address && size
			   ? StackBounds{reinterpret_cast<std::uintptr_t>(address),
							 reinterpret_cast<std::uintptr_t>(address) + size}
			   : StackBounds();
#endif // else for Q_OS_MAC
}

[[nodiscard]] RegisterState RegistersFromContext(void *rawContext) noexcept {
	const auto context = static_cast<ucontext_t *>(rawContext);
#if defined(Q_OS_MAC) && defined(__x86_64__)
	return {static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__rip),
			static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__rbp)};
#elif defined(Q_OS_MAC) && defined(__arm64__)
	return {static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__pc),
			static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__fp)};
#elif defined(Q_OS_LINUX) && defined(__x86_64__)
	return {static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]),
			static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RBP])};
#elif defined(Q_OS_LINUX) && defined(__i386__)
	return {static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_EIP]),
			static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_EBP])};
#elif defined(Q_OS_LINUX) && defined(__aarch64__)
	return {static_cast<std::uintptr_t>(context->uc_mcontext.pc),
			static_cast<std::uintptr_t>(context->uc_mcontext.regs[29])};
#else
	return {};
#endif
}

// The signal context holds the interrupted UI thread's registers.
// Walking frame pointers here avoids suspending or allocating on that thread.
// Symbol lookup happens later on the watchdog thread.
// Stack bounds keep the walk inside the main thread's stack.
[[nodiscard]] unsigned CaptureFrames(void *rawContext) noexcept {
	const auto registers = RegistersFromContext(rawContext);
	auto count = 0U;
	if (registers.instruction) {
		CapturedFrames[count++]
			= reinterpret_cast<void *>(registers.instruction);
	}
	auto frame = registers.frame;
	while (count < kMaxFrames
		   && (MainStack.high > MainStack.low + 2 * sizeof(std::uintptr_t))
		   && frame >= MainStack.low
		   && frame <= MainStack.high - 2 * sizeof(std::uintptr_t)
		   && (frame % alignof(std::uintptr_t)) == 0) {
		const auto record = reinterpret_cast<const std::uintptr_t *>(frame);
		const auto previous = record[0];
		auto address = record[1];
		if (!address) {
			break;
		}
#if defined(Q_OS_MAC) && defined(__arm64__)
		address = reinterpret_cast<std::uintptr_t>(ptrauth_strip(
			reinterpret_cast<void *>(address), ptrauth_key_return_address));
#endif // Q_OS_MAC && __arm64__
		CapturedFrames[count++] = reinterpret_cast<void *>(address);
		if (previous <= frame || previous >= MainStack.high) {
			break;
		}
		frame = previous;
	}
	return count;
}

void CaptureSignal(int, siginfo_t *, void *context) {
	const auto request = RequestedCapture.load(std::memory_order_acquire);
	if (!request || !pthread_equal(pthread_self(), MainThread)
		|| (CompletedCapture.load(std::memory_order_relaxed) == request)) {
		return;
	}
	const auto count = CaptureFrames(context);
	CapturedFrameCount.store(count, std::memory_order_relaxed);
	CompletedCapture.store(request, std::memory_order_release);
}

[[nodiscard]] bool InstallSignalHandler() {
	const auto lock = std::lock_guard(SignalInstallMutex);
	if (SignalInstalled) {
		return false;
	}
	MainThread = pthread_self();
#if defined(Q_OS_MAC)
	MainMachThread = mach_thread_self();
#endif // Q_OS_MAC
	MainStack = CurrentStackBounds(MainThread);
	struct sigaction action = {};
	action.sa_sigaction = CaptureSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	if (sigaction(SIGUSR2, &action, &PreviousSignalAction) != 0) {
#if defined(Q_OS_MAC)
		if (MainMachThread != MACH_PORT_NULL) {
			mach_port_deallocate(mach_task_self(), MainMachThread);
			MainMachThread = MACH_PORT_NULL;
		}
#endif // Q_OS_MAC
		return false;
	}
	SignalInstalled = true;
	return true;
}

[[nodiscard]] bool RestoreSignalHandler() {
	const auto lock = std::lock_guard(SignalInstallMutex);
	if (!SignalInstalled) {
		return true;
	}
	RequestedCapture.store(0, std::memory_order_relaxed);
	if (sigaction(SIGUSR2, &PreviousSignalAction, nullptr) != 0) {
		return false;
	}
	SignalInstalled = false;
#if defined(Q_OS_MAC)
	if (MainMachThread != MACH_PORT_NULL) {
		const auto result
			= mach_port_deallocate(mach_task_self(), MainMachThread);
		MainMachThread = MACH_PORT_NULL;
		if (result != KERN_SUCCESS) {
			return false;
		}
	}
#endif // Q_OS_MAC
	return true;
}

[[nodiscard]] std::vector<void *> CaptureStack() {
	if (!SignalInstalled) {
		return {};
	}
	auto request = CaptureSequence.fetch_add(1, std::memory_order_relaxed) + 1;
	if (!request) {
		request = CaptureSequence.fetch_add(1, std::memory_order_relaxed) + 1;
	}
	RequestedCapture.store(request, std::memory_order_release);
	if (pthread_kill(MainThread, SIGUSR2) != 0) {
		RequestedCapture.store(0, std::memory_order_release);
		return {};
	}
	for (auto i = 0; i != 10; ++i) {
		if (CompletedCapture.load(std::memory_order_acquire) == request) {
			const auto count
				= std::min(CapturedFrameCount.load(std::memory_order_relaxed),
						   static_cast<unsigned>(kMaxFrames));
			auto result = std::vector<void *>(count);
			std::copy_n(CapturedFrames.begin(), count, result.begin());
			RequestedCapture.store(0, std::memory_order_release);
			return result;
		}
		std::this_thread::sleep_for(1ms);
	}
	RequestedCapture.store(0, std::memory_order_release);
	return {};
}

[[nodiscard]] QString DescribeFrame(void *frame, bool returnAddress) {
	const auto address = reinterpret_cast<std::uintptr_t>(frame);
	const auto lookup = returnAddress && address
							? reinterpret_cast<void *>(address - 1)
							: frame;
	auto info = Dl_info();
	if (dladdr(lookup, &info) && info.dli_sname) {
		auto status = 0;
		const auto demangled
			= abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
		const auto name = (status == 0 && demangled)
							  ? QString::fromLocal8Bit(demangled)
							  : QString::fromLocal8Bit(info.dli_sname);
		std::free(demangled);
		if (!info.dli_saddr) {
			return name;
		}
		const auto offset
			= address - reinterpret_cast<std::uintptr_t>(info.dli_saddr);
		return u"%1+0x%2"_q.arg(name).arg(qulonglong(offset), 0, 16);
	}
	return u"0x%1"_q.arg(qulonglong(address), 0, 16);
}

[[nodiscard]] QString FormatRawStack(const std::vector<void *> &frames) {
	auto result = QStringList();
	for (const auto frame : frames) {
		result.push_back(u"0x%1"_q.arg(
			qulonglong(reinterpret_cast<std::uintptr_t>(frame)), 0, 16));
	}
	return result.isEmpty() ? u"stack unavailable"_q : result.join(u" <- "_q);
}

[[nodiscard]] QString FormatStack(const std::vector<void *> &frames) {
	auto result = QStringList();
	for (auto i = 0; i != int(frames.size()); ++i) {
		auto frame = DescribeFrame(frames[i], i != 0);
		frame.truncate(kMaxFrameCharacters);
		result.push_back(std::move(frame));
	}
	return result.isEmpty() ? u"stack unavailable"_q : result.join(u" <- "_q);
}

[[nodiscard]] QString FormatStallLine(qint64 durationMilliseconds,
									  const QString &startedAt,
									  const QString &schedulerTrace,
									  const QString &stack,
									  bool captureSchedulerTrace) {
	if (captureSchedulerTrace) {
		return u"UI stall %1 ms (started %2): "
			   "scheduler_trace=%3 stack=%4"_q.arg(durationMilliseconds)
				   .arg(startedAt)
				   .arg(schedulerTrace)
				   .arg(stack);
	}
	return u"UI stall %1 ms (started %2): %3"_q.arg(durationMilliseconds)
		.arg(startedAt)
		.arg(stack);
}

#endif // Q_OS_LINUX || Q_OS_MAC

class PingEvent final : public QEvent {
  public:
	static auto Type() {
		static const auto Result = QEvent::Type(QEvent::registerEventType());
		return Result;
	}

	explicit PingEvent(std::uint64_t sequence, Clock::time_point postedAt)
		: QEvent(Type()), sequence(sequence), postedAt(postedAt) {}

	const std::uint64_t sequence;
	const Clock::time_point postedAt;
};

} // namespace

UiStallWatchdog::UiStallWatchdog(
	std::function<void(const QString &, bool update)> log)
	: UiStallWatchdog(std::move(log), Settings()) {}

UiStallWatchdog::UiStallWatchdog(
	std::function<void(const QString &, bool update)> log, Settings settings)
	: QObject(nullptr), _log(std::move(log)),
	  _pingInterval(settings.captureSchedulerTrace ? kSchedulerTracePingInterval
					: settings.captureShortStalls  ? kShortPingInterval
												   : kPingInterval),
	  _countThreshold(kCountThreshold),
	  _logThreshold(settings.captureShortStalls ? kShortLogThreshold
												: kLogThreshold),
	  _logRateLimit(settings.captureShortStalls ? 0ms : kLogRateLimit),
	  _captureSchedulerTrace(settings.captureSchedulerTrace) {
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	_canCaptureStack = InstallSignalHandler();
	try {
		_reportThread = std::thread([this] { reportLoop(); });
		_thread = std::thread([this] { run(); });
	} catch (const std::system_error &error) {
		{
			const auto lock = std::lock_guard(_reportMutex);
			_reportStopping = true;
		}
		_reportCondition.notify_all();
		if (_reportThread.joinable()) {
			_reportThread.join();
		}
		if (_log) {
			_log(u"UI stall watchdog could not start: %1"_q.arg(
					 QString::fromLocal8Bit(error.what())),
				 false);
		}
		return;
	}
	auto lock = std::unique_lock(_mutex);
	_condition.wait(lock, [this] { return _started; });
#endif
}

UiStallWatchdog::~UiStallWatchdog() {
	{
		const auto lock = std::lock_guard(_mutex);
		_stopping = true;
	}
	_condition.notify_all();
	if (_thread.joinable()) {
		_thread.join();
	}
	{
		const auto lock = std::lock_guard(_reportMutex);
		_reportStopping = true;
	}
	_reportCondition.notify_all();
	if (_reportThread.joinable()) {
		_reportThread.join();
	}
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	if (_canCaptureStack && !RestoreSignalHandler() && _log) {
		_log(u"UI stall watchdog could not restore its signal handler"_q,
			 false);
	}
#endif
	if (QCoreApplication::instance()) {
		QCoreApplication::removePostedEvents(this, PingEvent::Type());
	}
}

quint64 UiStallWatchdog::StallCount() {
	return TotalStallCount.load(std::memory_order_relaxed);
}

quint64 UiStallWatchdog::StallCountForTesting() const {
	return _stallCount.load(std::memory_order_relaxed);
}

void UiStallWatchdog::StopAfterNextAcknowledgementForTesting() {
	const auto lock = std::lock_guard(_mutex);
	_stopAfterNextAcknowledgementForTesting = true;
}

std::vector<UiStallWatchdog::StallEvidence>
UiStallWatchdog::RecentStallEvidenceForTesting() {
	const auto lock = std::lock_guard(StallEvidenceMutex);
	return StallEvidenceHistory;
}

std::vector<UiStallWatchdog::PingAcknowledgementEvidence>
UiStallWatchdog::RecentPingAcknowledgementsForTesting() {
	const auto lock = std::lock_guard(StallEvidenceMutex);
	return {PingAcknowledgementHistory.begin(),
			PingAcknowledgementHistory.end()};
}

std::vector<UiStallWatchdog::SchedulerSampleEvidence>
UiStallWatchdog::RecentSchedulerSamplesForTesting() {
	const auto lock = std::lock_guard(SchedulerSampleHistoryMutex);
	return {SchedulerSampleHistory.begin(), SchedulerSampleHistory.end()};
}

bool UiStallWatchdog::ShouldStopBeforeTraceAttachment(
	bool stopping, std::uint64_t acknowledgedSequence,
	std::uint64_t currentSequence) {
	return stopping && (acknowledgedSequence < currentSequence);
}

bool UiStallWatchdog::SchedulerTraceCoversDuration(
	const QString &trace, const std::vector<qint64> &sampleOffsetsNanoseconds,
	qint64 durationNanoseconds) {
	const auto maxSchedulerSampleGapNanoseconds
		= std::chrono::duration_cast<std::chrono::nanoseconds>(
			  kMaxSchedulerSampleGap)
			  .count();
	if ((durationNanoseconds < 0)
		|| (trace.size() != qsizetype(sampleOffsetsNanoseconds.size()))
		|| trace.isEmpty()) {
		return false;
	}
	auto previousOffset = qint64(0);
	auto hasPreviousOffset = false;
	auto previousCoveredOffset = qint64(0);
	auto firstCoveredOffset = qint64(0);
	auto lastCoveredOffset = qint64(0);
	auto hasCoveredOffset = false;
	for (auto i = std::size_t(0); i != sampleOffsetsNanoseconds.size(); ++i) {
		const auto offset = sampleOffsetsNanoseconds[i];
		if (hasPreviousOffset && (offset < previousOffset)) {
			return false;
		}
		if ((offset >= 0) && (offset <= durationNanoseconds)) {
			if (hasCoveredOffset
				&& (offset - previousCoveredOffset
					> maxSchedulerSampleGapNanoseconds)) {
				return false;
			}
			if (!hasCoveredOffset) {
				firstCoveredOffset = offset;
				hasCoveredOffset = true;
			}
			previousCoveredOffset = offset;
			lastCoveredOffset = offset;
		}
		previousOffset = offset;
		hasPreviousOffset = true;
	}
	return hasCoveredOffset
		   && (firstCoveredOffset <= maxSchedulerSampleGapNanoseconds)
		   && (durationNanoseconds - lastCoveredOffset
			   <= maxSchedulerSampleGapNanoseconds);
}

bool UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
	const QString &trace, const std::vector<qint64> &sampleOffsetsNanoseconds,
	qint64 durationNanoseconds) {
	if (!SchedulerTraceCoversDuration(trace, sampleOffsetsNanoseconds,
									  durationNanoseconds)) {
		return false;
	}
	for (auto i = std::size_t(0); i != sampleOffsetsNanoseconds.size(); ++i) {
		const auto offset = sampleOffsetsNanoseconds[i];
		if ((offset >= 0) && (offset <= durationNanoseconds)
			&& (trace[qsizetype(i)] != u'R')) {
			return false;
		}
	}
	return true;
}

void UiStallWatchdog::enqueueReport(std::function<void()> report) {
	{
		const auto lock = std::lock_guard(_reportMutex);
		if (_reportStopping) {
			return;
		}
		_reports.push_back(std::move(report));
	}
	_reportCondition.notify_one();
}

void UiStallWatchdog::reportLoop() {
	while (true) {
		auto report = std::function<void()>();
		{
			auto lock = std::unique_lock(_reportMutex);
			_reportCondition.wait(
				lock, [this] { return _reportStopping || !_reports.empty(); });
			if (_reports.empty()) {
				return;
			}
			report = std::move(_reports.front());
			_reports.pop_front();
		}
		report();
	}
}

bool UiStallWatchdog::event(QEvent *event) {
	if (event->type() != PingEvent::Type()) {
		return QObject::event(event);
	}
	const auto ping = static_cast<PingEvent *>(event);
	const auto acknowledgedAt = Clock::now();
	{
		const auto lock = std::lock_guard(_mutex);
		if (ping->sequence > _acknowledgedSequence) {
			if (_captureSchedulerTrace) {
				const auto duration = acknowledgedAt - ping->postedAt;
				const auto evidence = PingAcknowledgementEvidence{
					.acknowledgedAt
					= QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
					.acknowledgedAtNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  acknowledgedAt.time_since_epoch())
						  .count(),
					.responseNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  duration)
						  .count(),
					.pingIntervalNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  _pingInterval)
						  .count(),
					.sequence = ping->sequence,
					.postedAtSteadyNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  ping->postedAt.time_since_epoch())
						  .count(),
				};
				const auto evidenceLock = std::lock_guard(StallEvidenceMutex);
				PingAcknowledgementHistory.push_back(evidence);
				if (PingAcknowledgementHistory.size()
					> kMaxPingAcknowledgements) {
					PingAcknowledgementHistory.pop_front();
				}
			}
			_acknowledgedSequence = ping->sequence;
			_acknowledgedAt = acknowledgedAt;
			if (_stopAfterNextAcknowledgementForTesting) {
				_stopAfterNextAcknowledgementForTesting = false;
				_stopping = true;
			}
		}
	}
	_condition.notify_one();
	return true;
}

void UiStallWatchdog::run() {
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	struct SchedulerSample final {
		Clock::time_point sampledAt;
		char state = '?';
	};
	auto schedulerSamplesMutex = std::mutex();
	auto schedulerSamples = std::deque<SchedulerSample>();
	auto schedulerSampler = std::jthread();
	if (_captureSchedulerTrace) {
		try {
			schedulerSampler = std::jthread(
				[this, &schedulerSamplesMutex, &schedulerSamples](
					std::stop_token stop) {
#if defined(Q_OS_MAC)
					const auto qosResult = pthread_set_qos_class_self_np(
						QOS_CLASS_USER_INTERACTIVE, 0);
					if (qosResult != 0) {
						enqueueReport([this, qosResult] {
							if (_log) {
								_log(
									u"UI stall watchdog scheduler sampler could not set QoS: %1"_q
										.arg(qosResult),
								false);
							}
						});
						return;
					}
					auto sampleIntervalMachTicks
						= SchedulerSampleIntervalMachTicks();
					auto nextSampleAtMach = sampleIntervalMachTicks
						? mach_absolute_time() + sampleIntervalMachTicks
						: std::uint64_t(0);
#endif // Q_OS_MAC
					auto nextSampleAt = Clock::now();
					while (!stop.stop_requested()) {
						auto sampleAt = nextSampleAt;
						const auto now = Clock::now();
						if (sampleAt < now) {
							sampleAt = now;
						}
#if defined(Q_OS_MAC)
						if (sampleIntervalMachTicks) {
							if (mach_wait_until(nextSampleAtMach)
								!= KERN_SUCCESS) {
								std::this_thread::sleep_until(sampleAt);
							}
						} else {
							std::this_thread::sleep_until(sampleAt);
						}
#else
						std::this_thread::sleep_until(sampleAt);
#endif // Q_OS_MAC
						if (stop.stop_requested()) {
							break;
						}
						const auto state = MainThreadSchedulerState();
						const auto sampledAt = Clock::now();
						{
							const auto lock
								= std::lock_guard(schedulerSamplesMutex);
							schedulerSamples.push_back({ sampledAt, state });
							if (schedulerSamples.size()
								> kMaxSchedulerSamples) {
								schedulerSamples.pop_front();
							}
						}
						{
							const auto lock
								= std::lock_guard(SchedulerSampleHistoryMutex);
							SchedulerSampleHistory.push_back({
								.sampledAtSteadyNanoseconds
								= std::chrono::duration_cast<
									  std::chrono::nanoseconds>(
									  sampledAt.time_since_epoch())
								  .count(),
								.state = state,
							});
							if (SchedulerSampleHistory.size()
								> kMaxSchedulerHistorySamples) {
								SchedulerSampleHistory.pop_front();
							}
						}
						nextSampleAt = sampledAt + kSchedulerSampleInterval;
#if defined(Q_OS_MAC)
						if (sampleIntervalMachTicks) {
							nextSampleAtMach = mach_absolute_time()
										  + sampleIntervalMachTicks;
						}
#endif // Q_OS_MAC
					}
				});
		} catch (const std::system_error &error) {
			const auto message = QString::fromLocal8Bit(error.what());
			enqueueReport([this, message] {
				if (_log) {
					_log(
						u"UI stall watchdog scheduler sampler could not start: %1"_q
							.arg(message),
						false);
				}
			});
		}
	}
	const auto schedulerSampleSnapshot
		= [&](Clock::time_point start, Clock::time_point end) {
			auto trace = std::string();
			auto offsets = std::vector<qint64>();
			const auto lock = std::lock_guard(schedulerSamplesMutex);
			const auto first = std::lower_bound(
				schedulerSamples.begin(), schedulerSamples.end(), start,
				[](const SchedulerSample &sample, Clock::time_point at) {
					return sample.sampledAt < at;
				});
			for (auto i = first;
				 i != schedulerSamples.end() && (i->sampledAt <= end); ++i) {
				trace.push_back(i->state);
				offsets.push_back(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						 i->sampledAt - start)
						.count());
			}
			return std::pair(std::move(trace), std::move(offsets));
		};
	auto sequence = std::uint64_t(0);
	auto hasLogged = false;
	auto lastLogged = Clock::time_point();
	auto lastLoggedDuration = Clock::duration::zero();
	while (true) {
		const auto current = ++sequence;
		const auto startTime = QDateTime::currentDateTime();
		const auto postedAt = Clock::now();
		const auto schedulerTraceTextSnapshot = [&](Clock::time_point end) {
			const auto snapshot = schedulerSampleSnapshot(postedAt, end);
			const auto &trace = snapshot.first;
			return trace.empty()
					   ? u"none"_q
					   : QString::fromLatin1(trace.data(),
										 qsizetype(trace.size()));
		};
		QCoreApplication::postEvent(this, new PingEvent(current, postedAt));
		{
			const auto lock = std::lock_guard(_mutex);
			_started = true;
		}
		_condition.notify_all();

		auto writeLog = false;
		auto replaceLog = false;
		auto detectedAt = Clock::time_point();
		auto frames = std::vector<void *>();
		auto rawStack = QString();
		auto formattedStack = std::shared_ptr<QString>();
		auto startedAt = QString();
		auto schedulerTraceText = QString();
		auto detectedDurationMilliseconds = qint64(0);
		auto lock = std::unique_lock(_mutex);
		const auto acknowledged
			= [&] { return _stopping || (_acknowledgedSequence >= current); };
		const auto waitUntil = [&](Clock::time_point deadline) {
			return _condition.wait_until(lock, deadline, acknowledged);
		};
		if (_condition.wait_until(lock, postedAt + _countThreshold,
								  acknowledged)) {
			if (ShouldStopBeforeTraceAttachment(
					_stopping, _acknowledgedSequence, current)) {
				return;
			}
			const auto duration = _acknowledgedAt - postedAt;
			if (duration >= _countThreshold) {
				_stallCount.fetch_add(1, std::memory_order_relaxed);
				TotalStallCount.fetch_add(1, std::memory_order_relaxed);
			}
			if (duration >= _logThreshold) {
				detectedAt = _acknowledgedAt;
			}
		} else {
			_stallCount.fetch_add(1, std::memory_order_relaxed);
			TotalStallCount.fetch_add(1, std::memory_order_relaxed);
			if (!waitUntil(postedAt + _logThreshold)) {
				detectedAt = Clock::now();
			} else if (ShouldStopBeforeTraceAttachment(
						   _stopping, _acknowledgedSequence, current)) {
				return;
			} else if (_acknowledgedAt - postedAt >= _logThreshold) {
				detectedAt = _acknowledgedAt;
			}
		}
		if (detectedAt != Clock::time_point() && _log) {
			const auto rateLimitedUntil = lastLogged + _logRateLimit;
			const auto rateLimited
				= hasLogged && Clock::now() < rateLimitedUntil;
			const auto observedDuration = detectedAt - postedAt;
			if (!rateLimited) {
				writeLog = true;
			} else if (observedDuration > lastLoggedDuration) {
				writeLog = true;
				replaceLog = true;
			} else {
				const auto longerThanLastAt
					= postedAt + lastLoggedDuration + Clock::duration(1);
				const auto retryAt
					= std::min(rateLimitedUntil, longerThanLastAt);
				if (!_condition.wait_until(lock, retryAt, acknowledged)) {
					detectedAt = Clock::now();
					writeLog = true;
					replaceLog = detectedAt < rateLimitedUntil;
				} else if (_acknowledgedSequence >= current) {
					if (_acknowledgedAt >= rateLimitedUntil) {
						detectedAt = _acknowledgedAt;
						writeLog = true;
					} else if (_acknowledgedAt - postedAt
							   > lastLoggedDuration) {
						detectedAt = _acknowledgedAt;
						writeLog = true;
						replaceLog = true;
					}
				}
			}
			if (writeLog) {
				lock.unlock();
				if (_canCaptureStack) {
					frames = CaptureStack();
				}
				rawStack = FormatRawStack(frames);
				formattedStack = std::make_shared<QString>();
				startedAt = startTime.toString(Qt::ISODateWithMs);
				schedulerTraceText = schedulerTraceTextSnapshot(detectedAt);
				detectedDurationMilliseconds
					= std::chrono::duration_cast<std::chrono::milliseconds>(
						  detectedAt - postedAt)
						  .count();
				const auto reportFrames
					= std::make_shared<std::vector<void *>>(std::move(frames));
				const auto reportedStack = formattedStack;
				enqueueReport([this, startedAt, schedulerTraceText,
							   detectedDurationMilliseconds, rawStack,
							   replaceLog, reportFrames, reportedStack] {
					_log(FormatStallLine(detectedDurationMilliseconds,
										 startedAt, schedulerTraceText,
										 rawStack, _captureSchedulerTrace),
						 replaceLog);
					if (!reportFrames->empty()) {
						*reportedStack = FormatStack(*reportFrames);
						_log(FormatStallLine(detectedDurationMilliseconds,
											 startedAt, schedulerTraceText,
											 *reportedStack,
											 _captureSchedulerTrace),
							 true);
					}
				});
				lastLogged = Clock::now();
				lastLoggedDuration = detectedAt - postedAt;
				hasLogged = true;
				lock.lock();
			}
		}
		while (!_stopping && (_acknowledgedSequence < current)) {
			_condition.wait(lock, acknowledged);
		}
		if (_acknowledgedSequence < current) {
			return;
		}
		auto schedulerSampleOffsetsNanoseconds = std::vector<qint64>();
		if (_captureSchedulerTrace) {
			const auto [trace, offsets]
				= schedulerSampleSnapshot(postedAt, _acknowledgedAt);
			schedulerTraceText
				= trace.empty()
					  ? u"none"_q
					  : QString::fromLatin1(trace.data(),
										 qsizetype(trace.size()));
			schedulerSampleOffsetsNanoseconds = offsets;
			const auto evidenceLock = std::lock_guard(StallEvidenceMutex);
			for (auto i = PingAcknowledgementHistory.rbegin();
				 i != PingAcknowledgementHistory.rend(); ++i) {
				if ((i->sequence == current)
					&& (i->postedAtSteadyNanoseconds
						== std::chrono::duration_cast<std::chrono::nanoseconds>(
							   postedAt.time_since_epoch())
							   .count())) {
					i->schedulerTrace = schedulerTraceText;
					i->schedulerSampleOffsetsNanoseconds
						= schedulerSampleOffsetsNanoseconds;
					i->schedulerTraceComplete = true;
					break;
				}
			}
		}
		if (_stopping) {
			return;
		}
		if (writeLog) {
			const auto completedAt = _acknowledgedAt;
			schedulerTraceText = schedulerTraceTextSnapshot(completedAt);
			lock.unlock();
			const auto completedDurationMilliseconds
				= std::chrono::duration_cast<std::chrono::milliseconds>(
					  completedAt - postedAt)
					  .count();
			const auto completedDuration = completedAt - postedAt;
			if (_captureSchedulerTrace) {
				const auto evidence = StallEvidence{
					.startedAt = startedAt,
					.postedAtSteadyNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  postedAt.time_since_epoch())
						  .count(),
					.durationMilliseconds = completedDurationMilliseconds,
					.durationNanoseconds
					= std::chrono::duration_cast<std::chrono::nanoseconds>(
						  completedDuration)
						  .count(),
					.schedulerTrace = schedulerTraceText,
					.schedulerSampleOffsetsNanoseconds
					= schedulerSampleOffsetsNanoseconds,
					.stack = rawStack,
				};
				const auto evidenceLock = std::lock_guard(StallEvidenceMutex);
				StallEvidenceHistory.push_back(evidence);
				if (StallEvidenceHistory.size() > 64) {
					StallEvidenceHistory.erase(StallEvidenceHistory.begin());
				}
			}
			enqueueReport([this, startedAt, schedulerTraceText,
						   completedDurationMilliseconds, rawStack,
						   reportedStack = formattedStack] {
				const auto stack
					= reportedStack->isEmpty() ? rawStack : *reportedStack;
				if (_captureSchedulerTrace && !reportedStack->isEmpty()) {
					auto evidenceLock = std::lock_guard(StallEvidenceMutex);
					for (auto i = StallEvidenceHistory.rbegin();
						 i != StallEvidenceHistory.rend(); ++i) {
						if (i->startedAt == startedAt) {
							i->stack = stack;
							break;
						}
					}
				}
				_log(FormatStallLine(completedDurationMilliseconds, startedAt,
									 schedulerTraceText, stack,
									 _captureSchedulerTrace),
					 true);
			});
			lastLogged = Clock::now();
			lastLoggedDuration = completedDuration;
			hasLogged = true;
			lock.lock();
		}
		_condition.wait_until(lock, _acknowledgedAt + _pingInterval,
							  [this] { return _stopping; });
		if (_stopping) {
			return;
		}
	}
#endif // Q_OS_LINUX || Q_OS_MAC
}

} // namespace Core
