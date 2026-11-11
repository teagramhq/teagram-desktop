/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ui_stall_watchdog.h"
#include "core/ui_stall_log_writer.h"

#include "tests/unit/unit_test.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#if defined(Q_OS_MAC)
#include <mach/mach.h>
#endif // Q_OS_MAC
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
#include <sys/resource.h>
#endif // Q_OS_LINUX || Q_OS_MAC
#if defined(Q_OS_LINUX)
#include <time.h>
#endif // Q_OS_LINUX

[[nodiscard]] std::optional<qint64>
UiStallWatchdogProcessCpuTimeNanosecondsForTest() {
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
	auto usage = rusage();
	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		return std::nullopt;
	}
	const auto userNanoseconds
		= (qint64(usage.ru_utime.tv_sec) * 1'000'000
		   + usage.ru_utime.tv_usec)
		  * 1'000;
	const auto systemNanoseconds
		= (qint64(usage.ru_stime.tv_sec) * 1'000'000
		   + usage.ru_stime.tv_usec)
		  * 1'000;
	return userNanoseconds + systemNanoseconds;
#else
	return std::nullopt;
#endif
}

[[nodiscard]] std::optional<qint64>
UiStallWatchdogThreadCpuTimeNanosecondsForTest() {
#if defined(Q_OS_MAC)
	const auto thread = mach_thread_self();
	if (thread == MACH_PORT_NULL) {
		return std::nullopt;
	}
	auto info = thread_basic_info_data_t();
	auto count = mach_msg_type_number_t(THREAD_BASIC_INFO_COUNT);
	const auto infoResult
		= thread_info(thread, THREAD_BASIC_INFO,
					  reinterpret_cast<thread_info_t>(&info), &count);
	const auto releaseResult = mach_port_deallocate(mach_task_self(), thread);
	if (infoResult != KERN_SUCCESS || releaseResult != KERN_SUCCESS) {
		return std::nullopt;
	}
	const auto userNanoseconds
		= (qint64(info.user_time.seconds) * 1'000'000
		   + info.user_time.microseconds)
		  * 1'000;
	const auto systemNanoseconds
		= (qint64(info.system_time.seconds) * 1'000'000
		   + info.system_time.microseconds)
		  * 1'000;
	return userNanoseconds + systemNanoseconds;
#elif defined(Q_OS_LINUX)
	auto usage = timespec();
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &usage) != 0) {
		return std::nullopt;
	}
	return qint64(usage.tv_sec) * 1'000'000'000 + usage.tv_nsec;
#else
	return std::nullopt;
#endif
}

// Spins without calling into libstdc++ or the vDSO: those are built without
// frame pointers, so a sample taken inside them would skip this frame.
__attribute__((noinline)) void
UiStallWatchdogTestBlock(std::chrono::milliseconds duration,
						 std::atomic_bool *waitUntil = nullptr) {
	auto released = std::atomic_bool(false);
	auto releaser = std::thread([&] {
		std::this_thread::sleep_for(duration);
		if (waitUntil) {
			const auto deadline
				= std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!waitUntil->load(std::memory_order_acquire)
				   && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		released.store(true, std::memory_order_release);
	});
	while (!released.load(std::memory_order_acquire)) {
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}
	releaser.join();
}

class ReadLimitedFile final : public QFile {
  public:
	using QFile::QFile;

	void LimitReadTo(qint64 size) {
		_readLimit = size;
		_readCount = 0;
		_readPastLimit = false;
		unsetError();
	}
	[[nodiscard]] qint64 ReadCount() const { return _readCount; }
	[[nodiscard]] bool ReadPastLimit() const { return _readPastLimit; }

  protected:
	qint64 readData(char *data, qint64 maxSize) override {
		if (_readLimit < 0) {
			return QFile::readData(data, maxSize);
		}
		const auto remaining = _readLimit - _readCount;
		if (remaining <= 0) {
			_readPastLimit = true;
			return 0;
		}
		const auto size = maxSize < remaining ? maxSize : remaining;
		const auto result = QFile::readData(data, size);
		if (result > 0) {
			_readCount += result;
		}
		return result;
	}

  private:
	qint64 _readLimit = -1;
	qint64 _readCount = 0;
	bool _readPastLimit = false;
};

class UiStallWatchdogTestBlockFilter final : public QObject {
  public:
	UiStallWatchdogTestBlockFilter(std::chrono::milliseconds duration,
								   QEventLoop *loop,
								   std::atomic_bool *logged = nullptr,
								   std::atomic_bool *blockActive = nullptr)
		: _durations({duration}), _loop(loop), _logged(logged),
		  _blockActive(blockActive) {}
	UiStallWatchdogTestBlockFilter(
		std::vector<std::chrono::milliseconds> durations, QEventLoop *loop,
		std::atomic_bool *logged = nullptr,
		std::atomic_bool *blockActive = nullptr)
		: _durations(std::move(durations)), _loop(loop), _logged(logged),
		  _blockActive(blockActive) {}

	[[nodiscard]] bool Blocked() const { return _blocked; }
	[[nodiscard]] int BlocksCompleted() const { return _nextBlock; }
	[[nodiscard]] bool LoggedWhileBlocked() const {
		return _loggedWhileBlocked;
	}

  protected:
	bool eventFilter(QObject *, QEvent *event) override {
		// Block every other ping, starting with the second ping to an idle
		// loop.
		if (event->type() >= QEvent::User) {
			++_pingCount;
		}
		if (event->type() >= QEvent::User && _nextBlock < int(_durations.size())
			&& _pingCount == 2 + 2 * _nextBlock) {
			_blocked = true;
			if (_logged) {
				_logged->store(false, std::memory_order_release);
			}
			if (_blockActive) {
				_blockActive->store(true, std::memory_order_release);
			}
			UiStallWatchdogTestBlock(_durations[_nextBlock], _logged);
			if (_blockActive) {
				_blockActive->store(false, std::memory_order_release);
			}
			_loggedWhileBlocked
				= _logged && _logged->load(std::memory_order_acquire);
			if (++_nextBlock == int(_durations.size())) {
				_loop->quit();
			}
		}
		return false;
	}

  private:
	const std::vector<std::chrono::milliseconds> _durations;
	QEventLoop *_loop = nullptr;
	std::atomic_bool *_logged = nullptr;
	std::atomic_bool *_blockActive = nullptr;
	int _pingCount = 0;
	int _nextBlock = 0;
	bool _blocked = false;
	bool _loggedWhileBlocked = false;
};

TEST_CASE(UiStallWatchdogLogsMainThreadStack) {
	auto lines = std::vector<QString>();
	auto mutex = std::mutex();
	auto logged = std::atomic_bool(false);
	auto blockActive = std::atomic_bool(false);
	auto completed = std::atomic_bool(false);
	auto acknowledgementsDuringLogging = std::atomic_int(0);
	auto initialSchedulerSampleCount = std::atomic_int(-1);
	auto finalReportedSchedulerTrace = QString();
	const auto schedulerTraceFromLine = [](const QString &line) {
		const auto prefix = u"scheduler_trace="_q;
		const auto start = line.indexOf(prefix);
		if (start < 0) {
			return QString();
		}
		const auto traceStart = start + prefix.size();
		const auto traceEnd = line.indexOf(u" stack="_q, traceStart);
		return (traceEnd >= traceStart)
				   ? line.mid(traceStart, traceEnd - traceStart)
				   : QString();
	};
	const auto beforeEvidence
		= Core::UiStallWatchdog::RecentStallEvidenceForTesting();
	const auto beforeAcknowledgements
		= Core::UiStallWatchdog::RecentPingAcknowledgementsForTesting();
	const auto lastEvidenceStartedAt
		= beforeEvidence.empty() ? QString() : beforeEvidence.back().startedAt;
	const auto lastAcknowledgedAt
		= beforeAcknowledgements.empty()
			  ? QString()
			  : beforeAcknowledgements.back().acknowledgedAt;
	const auto lastAcknowledgedAtNanoseconds
		= beforeAcknowledgements.empty()
			  ? qint64(0)
			  : beforeAcknowledgements.back().acknowledgedAtNanoseconds;
	const auto windowProcessCpuBefore
		= UiStallWatchdogProcessCpuTimeNanosecondsForTest();
	const auto windowThreadCpuBefore
		= UiStallWatchdogThreadCpuTimeNanosecondsForTest();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog(
			[&](const QString &line, bool update) {
				const auto lock = std::lock_guard(mutex);
				if (update) {
					if (!lines.empty()) {
						lines.back() = line;
					}
					if (!blockActive.load(std::memory_order_acquire)) {
						const auto before
							= Core::UiStallWatchdog::
								  RecentPingAcknowledgementsForTesting()
									  .size();
						std::this_thread::sleep_for(
							std::chrono::milliseconds(200));
						const auto after
							= Core::UiStallWatchdog::
								  RecentPingAcknowledgementsForTesting()
									  .size();
						acknowledgementsDuringLogging.store(
							int(after - before), std::memory_order_release);
					}
					const auto duration = line.mid(u"UI stall "_q.size())
											  .section(u' ', 0, 0)
											  .toInt();
					completed.store(duration >= 300, std::memory_order_release);
				} else {
					lines.push_back(line);
					initialSchedulerSampleCount.store(
						schedulerTraceFromLine(line).size(),
						std::memory_order_release);
				}
				if (blockActive.load(std::memory_order_acquire)) {
					logged.store(true, std::memory_order_release);
				}
			},
			{.captureSchedulerTrace = true});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(300), &loop, &logged, &blockActive);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
		CHECK(blockFilter.LoggedWhileBlocked());
		const auto deadline
			= std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!completed.load(std::memory_order_acquire)
			   && std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			std::this_thread::yield();
		}
		QCoreApplication::processEvents();
		CHECK_EQ(watchdog.StallCountForTesting(), 1);
	}
	const auto windowProcessCpuAfter
		= UiStallWatchdogProcessCpuTimeNanosecondsForTest();
	const auto windowThreadCpuAfter
		= UiStallWatchdogThreadCpuTimeNanosecondsForTest();
	auto windowOtherThreadCpuNanoseconds = qint64(-1);
	if (windowProcessCpuBefore && windowProcessCpuAfter && windowThreadCpuBefore
		&& windowThreadCpuAfter) {
		const auto processWork = *windowProcessCpuAfter - *windowProcessCpuBefore;
		const auto threadWork = *windowThreadCpuAfter - *windowThreadCpuBefore;
		if (processWork >= threadWork) {
			windowOtherThreadCpuNanoseconds = processWork - threadWork;
		}
	}
	CHECK(acknowledgementsDuringLogging.load(std::memory_order_acquire) >= 1);
	CHECK_EQ(int(lines.size()), 1);
	if (!lines.empty()) {
		CHECK(lines.front().startsWith(u"UI stall "_q));
		CHECK(lines.front().contains(u"UiStallWatchdogTestBlock"_q));
		CHECK(lines.front().contains(u"scheduler_trace="_q));
		finalReportedSchedulerTrace = schedulerTraceFromLine(lines.front());
		CHECK(finalReportedSchedulerTrace.size()
			  > initialSchedulerSampleCount.load(std::memory_order_acquire));
		const auto duration = lines.front()
								  .mid(u"UI stall "_q.size())
								  .section(u' ', 0, 0)
								  .toInt();
		CHECK(duration >= 300);
	}
	const auto evidence
		= Core::UiStallWatchdog::RecentStallEvidenceForTesting();
	const auto acknowledgements
		= Core::UiStallWatchdog::RecentPingAcknowledgementsForTesting();
	CHECK(!evidence.empty());
	CHECK(!acknowledgements.empty());
	auto shortSchedulerCapturePingObserved = false;
	auto pingIntervalNanoseconds = qint64(-1);
	auto previousAcknowledgedAtNanoseconds
		= lastAcknowledgedAtNanoseconds;
	auto hasPreviousAcknowledgement
		= (lastAcknowledgedAtNanoseconds > 0);
	auto acknowledgementGapsNanoseconds = std::vector<qint64>();
	for (const auto &acknowledgement : acknowledgements) {
		const auto acknowledgedAtNanoseconds
			= acknowledgement.acknowledgedAtNanoseconds;
		if (acknowledgedAtNanoseconds <= lastAcknowledgedAtNanoseconds) {
			continue;
		}
		pingIntervalNanoseconds = acknowledgement.pingIntervalNanoseconds;
		if (hasPreviousAcknowledgement
			&& (acknowledgedAtNanoseconds > previousAcknowledgedAtNanoseconds)) {
			const auto gapNanoseconds
				= acknowledgedAtNanoseconds - previousAcknowledgedAtNanoseconds;
			acknowledgementGapsNanoseconds.push_back(gapNanoseconds);
			if (gapNanoseconds <= 5'000'000) {
				shortSchedulerCapturePingObserved = true;
			}
		}
		previousAcknowledgedAtNanoseconds = acknowledgedAtNanoseconds;
		hasPreviousAcknowledgement = true;
	}
	if (!shortSchedulerCapturePingObserved) {
		std::fprintf(stderr,
			"Scheduler ping cadence failure: ping_interval_ns=%lld "
			"window_other_thread_cpu_ns=%lld acknowledgement_gaps_ns=[",
			static_cast<long long>(pingIntervalNanoseconds),
			static_cast<long long>(windowOtherThreadCpuNanoseconds));
		for (auto i = std::size_t(0); i != acknowledgementGapsNanoseconds.size();
			 ++i) {
			std::fprintf(stderr, "%s%lld", i ? "," : "",
						 static_cast<long long>(
							 acknowledgementGapsNanoseconds[i]));
		}
		std::fprintf(stderr, "]\n");
	}
	CHECK(shortSchedulerCapturePingObserved);
	if (!acknowledgements.empty()) {
		CHECK(acknowledgements.back().acknowledgedAt != lastAcknowledgedAt);
		CHECK(acknowledgements.back().pingIntervalNanoseconds > 0);
		CHECK(acknowledgements.back().acknowledgedAtNanoseconds
			  > lastAcknowledgedAtNanoseconds);
		CHECK(acknowledgements.back().responseNanoseconds >= 0);
	}
	if (!evidence.empty()) {
		CHECK(evidence.back().startedAt != lastEvidenceStartedAt);
		CHECK(evidence.back().stack.contains(u"UiStallWatchdogTestBlock"_q));
		CHECK(!evidence.back().schedulerTrace.isEmpty());
		CHECK(evidence.back().schedulerTrace.size()
			  == qsizetype(
				  evidence.back().schedulerSampleOffsetsNanoseconds.size()));
		CHECK(evidence.back().schedulerTrace.size()
			  >= evidence.back().durationMilliseconds / 5);
		const auto schedulerTraceProvesRunnable
			= Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
				evidence.back().schedulerTrace,
				evidence.back().schedulerSampleOffsetsNanoseconds,
				evidence.back().durationNanoseconds);
		CHECK_EQ(finalReportedSchedulerTrace, evidence.back().schedulerTrace);
#if defined(Q_OS_MAC)
		CHECK(evidence.back().schedulerTrace.contains(u'R'));
		CHECK(schedulerTraceProvesRunnable);
#else
		CHECK(!schedulerTraceProvesRunnable);
#endif
	}
}

TEST_CASE(UiStallWatchdogSchedulerTraceRequiresContinuousCoverage) {
	constexpr auto sampleIntervalNanoseconds = qint64(1'000'000);
	constexpr auto durationNanoseconds = qint64(95'000'000);
	constexpr auto jitterNanoseconds = qint64(2'000'000);
	auto offsets = std::vector<qint64>();
	for (auto offset = qint64(0); offset <= 94'000'000;
		 offset += sampleIntervalNanoseconds) {
		offsets.push_back(offset);
	}
	const auto trace = QString(int(offsets.size()), QChar(u'R'));
	CHECK(Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, offsets, durationNanoseconds));

	auto paddedTrace = u"W"_q + trace + u"W"_q;
	auto paddedOffsets = std::vector<qint64>{-1'000'000};
	paddedOffsets.insert(paddedOffsets.end(), offsets.begin(), offsets.end());
	paddedOffsets.push_back(95'000'000);
	CHECK(Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		paddedTrace, paddedOffsets, 94'000'000));

	auto blockedTrace = trace;
	blockedTrace[45] = u'W';
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		blockedTrace, offsets, durationNanoseconds));

	auto smallJitterOffsets = offsets;
	for (auto i = std::size_t(10); i != smallJitterOffsets.size(); ++i) {
		smallJitterOffsets[i] += jitterNanoseconds;
	}
	CHECK(Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, smallJitterOffsets, durationNanoseconds));

	auto jitteredOffsets = offsets;
	for (auto i = std::size_t(0); i != jitteredOffsets.size(); ++i) {
		const auto phase = i % 32;
		const auto jitterSteps = phase <= 16 ? phase : 32 - phase;
		const auto jitter = qint64(jitterSteps) * (jitterNanoseconds / 8);
		jitteredOffsets[i] += jitter;
	}
	CHECK(Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, jitteredOffsets, durationNanoseconds));

	auto skippedSlotOffsets = offsets;
	for (auto i = std::size_t(10); i != skippedSlotOffsets.size(); ++i) {
		skippedSlotOffsets[i] += 5 * sampleIntervalNanoseconds;
	}
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, skippedSlotOffsets, durationNanoseconds));

	auto blockedInJitteredGapOffsets = jitteredOffsets;
	for (auto i = std::size_t(10); i != blockedInJitteredGapOffsets.size();
		 ++i) {
		blockedInJitteredGapOffsets[i] += 5'000'000;
	}
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, blockedInJitteredGapOffsets, durationNanoseconds));

	auto lateStartOffsets = offsets;
	for (auto &offset : lateStartOffsets) {
		offset += 7'000'000;
	}
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, lateStartOffsets, durationNanoseconds));

	auto lateEndOffsets = offsets;
	lateEndOffsets.back() -= 6'000'000;
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		trace, lateEndOffsets, durationNanoseconds));
	CHECK(!Core::UiStallWatchdog::SchedulerTraceProvesRunnableForDuration(
		QString(4, QChar(u'R')),
		{0, sampleIntervalNanoseconds, 2 * sampleIntervalNanoseconds,
		 3 * sampleIntervalNanoseconds},
		92'000'000));
}

TEST_CASE(UiStallWatchdogCapturesShortStalls) {
	auto lines = std::vector<QString>();
	auto mutex = std::mutex();
	auto blockActive = std::atomic_bool(false);
	auto logged = std::atomic_bool(false);
	auto completed = std::atomic_bool(false);
	auto loop = QEventLoop();
	auto watchdog = Core::UiStallWatchdog(
		[&](const QString &line, bool update) {
			const auto lock = std::lock_guard(mutex);
			if (update) {
				if (!lines.empty()) {
					lines.back() = line;
				}
				const auto duration = line.mid(u"UI stall "_q.size())
										  .section(u' ', 0, 0)
										  .toInt();
				completed.store(duration >= 50, std::memory_order_release);
			} else {
				lines.push_back(line);
			}
			if (blockActive.load(std::memory_order_acquire)) {
				logged.store(true, std::memory_order_release);
			}
		},
		{.captureShortStalls = true});
	auto blockFilter = UiStallWatchdogTestBlockFilter(
		std::chrono::milliseconds(80), &loop, &logged, &blockActive);
	watchdog.installEventFilter(&blockFilter);
	QTimer::singleShot(2000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK(blockFilter.Blocked());
	CHECK(blockFilter.LoggedWhileBlocked());
	CHECK_EQ(watchdog.StallCountForTesting(), 1);
	const auto deadline
		= std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!completed.load(std::memory_order_acquire)
		   && std::chrono::steady_clock::now() < deadline) {
		QCoreApplication::processEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	QCoreApplication::processEvents();
	const auto lock = std::lock_guard(mutex);
	CHECK_EQ(int(lines.size()), 1);
	if (!lines.empty()) {
		CHECK(lines.front().startsWith(u"UI stall "_q));
		CHECK(lines.front().contains(u"UiStallWatchdogTestBlock"_q));
		const auto duration = lines.front()
								  .mid(u"UI stall "_q.size())
								  .section(u' ', 0, 0)
								  .toInt();
		CHECK(duration >= 50);
	}
}

TEST_CASE(UiStallWatchdogSymbolizesStackWhileMainThreadBlocked) {
	auto blockActive = std::atomic_bool(false);
	auto symbolizedWhileBlocked = std::atomic_bool(false);
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([&](const QString &line, bool) {
			if (blockActive.load(std::memory_order_acquire)
				&& line.contains(u"UiStallWatchdogTestBlock"_q)) {
				symbolizedWhileBlocked.store(true, std::memory_order_release);
			}
		});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(2000), &loop, nullptr, &blockActive);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(3000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
		CHECK(symbolizedWhileBlocked.load(std::memory_order_acquire));
	}
}

TEST_CASE(UiStallLogWriterUpdatesWithinLargeLogTail) {
	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	if (!directory.isValid()) {
		return;
	}

	const auto path = QDir(directory.path()).filePath(u"log.txt"_q);
	auto log = QFile(path);
	CHECK(log.open(QIODevice::WriteOnly | QIODevice::Append));
	if (!log.isOpen()) {
		return;
	}

	auto writer = Core::UiStallLogWriter();
	CHECK(writer.writeDetection(log, u"[prefix] "_q,
								u"UI stall 100 ms (started test): stack"_q));
	const auto slotSize = log.size();
	auto detection = QFile(path);
	CHECK(detection.open(QIODevice::ReadOnly));
	if (!detection.isOpen()) {
		return;
	}
	const auto detected = QString::fromUtf8(detection.readAll());
	CHECK(detected.startsWith(
		u"[prefix] UI stall 100 ms (started test): stack"_q));
	detection.close();

	const auto following = QByteArray(
		"[following] first log entry\n[following] second log entry\n");
	CHECK_EQ(log.write(following), qint64(following.size()));
	auto largeTail = QByteArray(4 * 1024 * 1024, 'x');
	largeTail += '\n';
	CHECK_EQ(log.write(largeTail), qint64(largeTail.size()));
	CHECK(log.flush());
	log.close();

	auto output = ReadLimitedFile(path);
	CHECK(output.open(QIODevice::ReadWrite));
	if (!output.isOpen()) {
		return;
	}
	output.LimitReadTo(slotSize);
	auto replacement = u"UI stall 900 ms (started longer stall): "_q;
	const auto longFrame = QString(160, QChar(u'x'));
	for (auto i = 0; i != 31; ++i) {
		replacement += longFrame + u" <- "_q;
	}
	replacement += u"CheckCachePath"_q;
	CHECK(writer.writeUpdate(output, replacement));
	CHECK_EQ(output.ReadCount(), slotSize);
	CHECK(!output.ReadPastLimit());
	output.close();

	auto result = QFile(path);
	CHECK(result.open(QIODevice::ReadOnly));
	if (!result.isOpen()) {
		return;
	}
	const auto content = result.readAll();
	CHECK(content.startsWith(
		"[prefix] UI stall 900 ms (started longer stall): x"));
	CHECK(content.contains("CheckCachePath"));
	CHECK_EQ(content.count("UI stall "), 1);
	CHECK_EQ(content.mid(slotSize), following + largeTail);
}

TEST_CASE(UiStallWatchdogReplacesShortLogWithLongerStall) {
	auto lines = std::vector<QString>();
	auto mutex = std::mutex();
	auto loggedWhileBlocked = std::atomic_bool(false);
	auto blockActive = std::atomic_bool(false);
	auto completedLongStall = std::atomic_bool(false);
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([&](const QString &line,
												  bool update) {
			const auto lock = std::lock_guard(mutex);
			if (update) {
				if (!lines.empty()) {
					lines.back() = line;
				}
				const auto duration = line.mid(u"UI stall "_q.size())
										  .section(u' ', 0, 0)
										  .toInt();
				if (duration >= 300) {
					completedLongStall.store(true, std::memory_order_release);
				}
			} else {
				lines.push_back(line);
			}
			if (blockActive.load(std::memory_order_acquire)) {
				loggedWhileBlocked.store(true, std::memory_order_release);
			}
		});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::vector<std::chrono::milliseconds>{
				std::chrono::milliseconds(150),
				std::chrono::milliseconds(350),
			},
			&loop, &loggedWhileBlocked, &blockActive);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(8000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK_EQ(blockFilter.BlocksCompleted(), 2);
		CHECK(blockFilter.LoggedWhileBlocked());
		const auto deadline
			= std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!completedLongStall.load(std::memory_order_acquire)
			   && std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK_EQ(watchdog.StallCountForTesting(), 2);
	}
	CHECK_EQ(int(lines.size()), 1);
	if (!lines.empty()) {
		const auto duration = lines.front()
								  .mid(u"UI stall "_q.size())
								  .section(u' ', 0, 0)
								  .toInt();
		CHECK(duration >= 300);
	}
}

TEST_CASE(UiStallWatchdogIgnoresIdleTimeForShortStall) {
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([](const QString &, bool) {});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(20), &loop);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
		CHECK_EQ(watchdog.StallCountForTesting(), 0);
	}
}

TEST_CASE(UiStallWatchdogCountsButDoesNotLogMidThresholdStall) {
	auto lines = std::vector<QString>();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog(
			[&](const QString &line, bool) { lines.push_back(line); });
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(70), &loop);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
		CHECK_EQ(watchdog.StallCountForTesting(), 1);
	}
	CHECK(lines.empty());
}
