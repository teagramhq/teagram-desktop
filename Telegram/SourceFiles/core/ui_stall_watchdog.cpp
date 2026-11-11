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
constexpr auto kShortLogThreshold = 40ms;
constexpr auto kSchedulerSampleInterval = 5ms;
constexpr auto kMaxSchedulerSamples = 1024U;
constexpr auto kMaxFrames = 32U;
constexpr auto kMaxFrameCharacters = 160;
constexpr auto kMaxPingAcknowledgements = 16'384U;

std::atomic<quint64> TotalStallCount = 0;
std::mutex StallEvidenceMutex;
std::vector<UiStallWatchdog::StallEvidence> StallEvidenceHistory;
std::vector<UiStallWatchdog::PingAcknowledgementEvidence>
	PingAcknowledgementHistory;

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
	  _pingInterval(settings.captureShortStalls ? kShortPingInterval
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

std::vector<UiStallWatchdog::StallEvidence>
UiStallWatchdog::RecentStallEvidenceForTesting() {
	const auto lock = std::lock_guard(StallEvidenceMutex);
	return StallEvidenceHistory;
}

std::vector<UiStallWatchdog::PingAcknowledgementEvidence>
UiStallWatchdog::RecentPingAcknowledgementsForTesting() {
	const auto lock = std::lock_guard(StallEvidenceMutex);
	return PingAcknowledgementHistory;
}

bool UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
	const QString &trace, const std::vector<qint64> &sampleOffsetsNanoseconds,
	qint64 durationNanoseconds) {
	const auto sampleIntervalNanoseconds
		= std::chrono::duration_cast<std::chrono::nanoseconds>(
			  kSchedulerSampleInterval)
			  .count();
	const auto sampleCoverageToleranceNanoseconds
		= sampleIntervalNanoseconds * 3 / 2;
	const auto sampleEdgeToleranceNanoseconds = sampleIntervalNanoseconds;
	if ((durationNanoseconds < 0)
		|| (trace.size() != qsizetype(sampleOffsetsNanoseconds.size()))
		|| trace.isEmpty() || (trace.count(u'R') != trace.size())) {
		return false;
	}
	auto previousOffset = qint64(0);
	auto hasPreviousOffset = false;
	auto firstCoveredOffset = qint64(0);
	auto lastCoveredOffset = qint64(0);
	auto hasCoveredOffset = false;
	for (const auto offset : sampleOffsetsNanoseconds) {
		if (hasPreviousOffset
			&& ((offset < previousOffset)
				|| ((offset >= 0) && (previousOffset <= durationNanoseconds)
					&& (offset - previousOffset
						> sampleCoverageToleranceNanoseconds)))) {
			return false;
		}
		if ((offset >= 0) && (offset <= durationNanoseconds)) {
			if (!hasCoveredOffset) {
				firstCoveredOffset = offset;
				hasCoveredOffset = true;
			}
			lastCoveredOffset = offset;
		}
		previousOffset = offset;
		hasPreviousOffset = true;
	}
	return hasCoveredOffset
		   && (firstCoveredOffset <= sampleEdgeToleranceNanoseconds)
		   && (durationNanoseconds - lastCoveredOffset
			   <= sampleEdgeToleranceNanoseconds);
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
	auto acknowledged = false;
	{
		const auto lock = std::lock_guard(_mutex);
		if (ping->sequence > _acknowledgedSequence) {
			_acknowledgedSequence = ping->sequence;
			_acknowledgedAt = acknowledgedAt;
			if (_captureSchedulerTrace) {
				_schedulerSamplingStopSequence.store(ping->sequence,
													 std::memory_order_release);
			}
			acknowledged = true;
		}
	}
	if (acknowledged && _captureSchedulerTrace) {
		const auto duration = acknowledgedAt - ping->postedAt;
		const auto evidence = PingAcknowledgementEvidence{
			.acknowledgedAt
			= QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
			.responseNanoseconds
			= std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
				  .count(),
		};
		const auto lock = std::lock_guard(StallEvidenceMutex);
		PingAcknowledgementHistory.push_back(evidence);
		if (PingAcknowledgementHistory.size() > kMaxPingAcknowledgements) {
			PingAcknowledgementHistory.erase(
				PingAcknowledgementHistory.begin());
		}
	}
	_condition.notify_one();
	return true;
}

void UiStallWatchdog::run() {
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	auto sequence = std::uint64_t(0);
	auto hasLogged = false;
	auto lastLogged = Clock::time_point();
	auto lastLoggedDuration = Clock::duration::zero();
	while (true) {
		const auto current = ++sequence;
		const auto startTime = QDateTime::currentDateTime();
		const auto postedAt = Clock::now();
		auto schedulerTrace = std::string();
		auto schedulerSampleOffsetsNanoseconds = std::vector<qint64>();
		auto schedulerTraceMutex = std::mutex();
		auto schedulerSampler = std::jthread();
		const auto schedulerTraceTextSnapshot = [&] {
			const auto traceLock = std::lock_guard(schedulerTraceMutex);
			return schedulerTrace.empty()
					   ? u"none"_q
					   : QString::fromLatin1(schedulerTrace.data(),
											 qsizetype(schedulerTrace.size()));
		};
		QCoreApplication::postEvent(this, new PingEvent(current, postedAt));
		if (_captureSchedulerTrace) {
			const auto initialState = MainThreadSchedulerState();
			const auto sampledAt = Clock::now();
			schedulerTrace.push_back(initialState);
			schedulerSampleOffsetsNanoseconds.push_back(
				std::chrono::duration_cast<std::chrono::nanoseconds>(sampledAt
																	 - postedAt)
					.count());
			try {
				schedulerSampler = std::jthread(
					[this, current, postedAt, &schedulerTrace,
					 &schedulerSampleOffsetsNanoseconds,
					 &schedulerTraceMutex](std::stop_token stop) {
						auto nextSampleAt = postedAt + kSchedulerSampleInterval;
						auto sampleCount = std::size_t(1);
						while (!stop.stop_requested()
							   && (_schedulerSamplingStopSequence.load(
									   std::memory_order_acquire)
								   < current)
							   && (sampleCount < kMaxSchedulerSamples)) {
							auto sampleAt = nextSampleAt;
							const auto now = Clock::now();
							if (sampleAt + kSchedulerSampleInterval < now) {
								sampleAt = now + kSchedulerSampleInterval;
							}
							std::this_thread::sleep_until(sampleAt);
							if (stop.stop_requested()
								|| (_schedulerSamplingStopSequence.load(
										std::memory_order_acquire)
									>= current)) {
								break;
							}
							const auto state = MainThreadSchedulerState();
							const auto sampledAt = Clock::now();
							if (_schedulerSamplingStopSequence.load(
									std::memory_order_acquire)
								>= current) {
								break;
							}
							{
								const auto traceLock
									= std::lock_guard(schedulerTraceMutex);
								schedulerTrace.push_back(state);
								schedulerSampleOffsetsNanoseconds.push_back(
									std::chrono::duration_cast<
										std::chrono::nanoseconds>(sampledAt
																  - postedAt)
										.count());
							}
							++sampleCount;
							nextSampleAt = sampleAt + kSchedulerSampleInterval;
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
			if (_stopping) {
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
			} else if (_stopping) {
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
				} else if (_stopping) {
					return;
				} else if (_acknowledgedAt >= rateLimitedUntil) {
					detectedAt = _acknowledgedAt;
					writeLog = true;
				} else if (_acknowledgedAt - postedAt > lastLoggedDuration) {
					detectedAt = _acknowledgedAt;
					writeLog = true;
					replaceLog = true;
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
				schedulerTraceText = schedulerTraceTextSnapshot();
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
		if (_stopping) {
			return;
		}
		if (_acknowledgedSequence < current) {
			return;
		}
		if (schedulerSampler.joinable()) {
			schedulerSampler.request_stop();
			schedulerSampler.join();
		}
		if (writeLog) {
			const auto completedAt = _acknowledgedAt;
			schedulerTraceText = schedulerTraceTextSnapshot();
			lock.unlock();
			const auto completedDurationMilliseconds
				= std::chrono::duration_cast<std::chrono::milliseconds>(
					  completedAt - postedAt)
					  .count();
			const auto completedDuration = completedAt - postedAt;
			if (_captureSchedulerTrace) {
				const auto evidence = StallEvidence{
					.startedAt = startedAt,
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
