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
#include <system_error>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
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
constexpr auto kMaxFrames = 15U;

std::atomic<quint64> TotalStallCount = 0;

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
StackBounds MainStack;
struct sigaction PreviousSignalAction = {};
std::mutex SignalInstallMutex;
bool SignalInstalled = false;

static_assert(std::atomic_uint::is_always_lock_free);

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
	MainStack = CurrentStackBounds(MainThread);
	struct sigaction action = {};
	action.sa_sigaction = CaptureSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	if (sigaction(SIGUSR2, &action, &PreviousSignalAction) != 0) {
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

[[nodiscard]] QString FormatStack(const std::vector<void *> &frames) {
	auto result = QStringList();
	for (auto i = 0; i != int(frames.size()); ++i) {
		result.push_back(DescribeFrame(frames[i], i != 0));
	}
	return result.isEmpty() ? u"stack unavailable"_q : result.join(u" <- "_q);
}

#endif // Q_OS_LINUX || Q_OS_MAC

class PingEvent final : public QEvent {
  public:
	static auto Type() {
		static const auto Result = QEvent::Type(QEvent::registerEventType());
		return Result;
	}

	explicit PingEvent(std::uint64_t sequence)
		: QEvent(Type()), sequence(sequence) {}

	const std::uint64_t sequence;
};

} // namespace

UiStallWatchdog::UiStallWatchdog(std::function<void(const QString &)> log)
	: QObject(nullptr), _log(std::move(log)) {
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	_canCaptureStack = InstallSignalHandler();
	try {
		_thread = std::thread([this] { run(); });
	} catch (const std::system_error &error) {
		if (_log) {
			_log(u"UI stall watchdog could not start: %1"_q.arg(
				QString::fromLocal8Bit(error.what())));
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
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	if (_canCaptureStack && !RestoreSignalHandler() && _log) {
		_log(u"UI stall watchdog could not restore its signal handler"_q);
	}
#endif
	if (QCoreApplication::instance()) {
		QCoreApplication::removePostedEvents(this, PingEvent::Type());
	}
}

quint64 UiStallWatchdog::StallCount() {
	return TotalStallCount.load(std::memory_order_relaxed);
}

bool UiStallWatchdog::event(QEvent *event) {
	if (event->type() != PingEvent::Type()) {
		return QObject::event(event);
	}
	const auto ping = static_cast<PingEvent *>(event);
	{
		const auto lock = std::lock_guard(_mutex);
		if (ping->sequence > _acknowledgedSequence) {
			_acknowledgedSequence = ping->sequence;
			_acknowledgedAt = Clock::now();
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
	while (true) {
		const auto current = ++sequence;
		const auto startTime = QDateTime::currentDateTime();
		const auto postedAt = Clock::now();
		QCoreApplication::postEvent(this, new PingEvent(current));
		{
			const auto lock = std::lock_guard(_mutex);
			_started = true;
		}
		_condition.notify_all();

		// Persist the detection record before waiting; an ack adds the
		// duration.
		auto writeLog = false;
		auto detectedAt = Clock::time_point();
		auto frames = std::vector<void *>();
		auto stack = QString();
		const auto lineForDuration = [&](Clock::duration duration) {
			const auto milliseconds
				= std::chrono::duration_cast<std::chrono::milliseconds>(
					duration);
			return u"UI stall %1 ms (started %2): %3"_q
				.arg(milliseconds.count())
				.arg(startTime.toString(Qt::ISODateWithMs))
				.arg(stack);
		};
		auto lock = std::unique_lock(_mutex);
		const auto acknowledged
			= [&] { return _stopping || (_acknowledgedSequence >= current); };
		if (_condition.wait_until(lock, postedAt + kCountThreshold,
								  acknowledged)) {
			if (_stopping) {
				return;
			}
			const auto duration = _acknowledgedAt - postedAt;
			if (duration >= kCountThreshold) {
				TotalStallCount.fetch_add(1, std::memory_order_relaxed);
			}
			if (duration >= kLogThreshold) {
				detectedAt = _acknowledgedAt;
			}
		} else {
			TotalStallCount.fetch_add(1, std::memory_order_relaxed);
			if (!_condition.wait_until(lock, postedAt + kLogThreshold,
									   acknowledged)) {
				detectedAt = Clock::now();
			} else if (_stopping) {
				return;
			} else if (_acknowledgedAt - postedAt >= kLogThreshold) {
				detectedAt = _acknowledgedAt;
			}
		}
		if (detectedAt != Clock::time_point()) {
			writeLog = _log
					   && (!hasLogged
						   || (Clock::now() - lastLogged) >= kLogRateLimit);
			if (writeLog) {
				lock.unlock();
				if (_canCaptureStack) {
					frames = CaptureStack();
				}
				stack = FormatStack(frames);
				lock.lock();
				lock.unlock();
				_log(lineForDuration(detectedAt - postedAt));
				lastLogged = Clock::now();
				hasLogged = true;
				lock.lock();
			}
		}
		_condition.wait(lock, acknowledged);
		if (_stopping) {
			return;
		}
		if (writeLog) {
			const auto completedAt = _acknowledgedAt;
			const auto completedMilliseconds
				= std::chrono::duration_cast<std::chrono::milliseconds>(
					completedAt - postedAt);
			const auto completion
				= u"UI watchdog acknowledgement after %1 ms (started %2)"_q
					  .arg(completedMilliseconds.count())
					  .arg(startTime.toString(Qt::ISODateWithMs));
			lock.unlock();
			_log(completion);
			lastLogged = Clock::now();
			lock.lock();
		}
		_condition.wait_until(lock, _acknowledgedAt + kPingInterval,
							  [this] { return _stopping; });
		if (_stopping) {
			return;
		}
	}
#endif // Q_OS_LINUX || Q_OS_MAC
}

} // namespace Core
