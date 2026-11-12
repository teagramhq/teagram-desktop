/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Core {

class UiStallWatchdog final : public QObject {
  public:
	struct Settings {
		bool captureShortStalls = false;
		bool captureSchedulerTrace = false;
	};

	struct StallEvidence {
		QString startedAt;
		qint64 postedAtSteadyNanoseconds = 0;
		qint64 durationMilliseconds = 0;
		qint64 durationNanoseconds = 0;
		QString schedulerTrace;
		std::vector<qint64> schedulerSampleOffsetsNanoseconds;
		QString stack;
	};

	struct PingAcknowledgementEvidence {
		QString acknowledgedAt;
		qint64 acknowledgedAtNanoseconds = 0;
		qint64 responseNanoseconds = 0;
		qint64 pingIntervalNanoseconds = 0;
	};

	explicit UiStallWatchdog(
		std::function<void(const QString &, bool update)> log);
	UiStallWatchdog(std::function<void(const QString &, bool update)> log,
					Settings settings);
	~UiStallWatchdog() override;

	UiStallWatchdog(const UiStallWatchdog &) = delete;
	UiStallWatchdog &operator=(const UiStallWatchdog &) = delete;

	[[nodiscard]] static quint64 StallCount();
	[[nodiscard]] quint64 StallCountForTesting() const;
	[[nodiscard]] static std::vector<StallEvidence>
	RecentStallEvidenceForTesting();
	[[nodiscard]] static std::vector<PingAcknowledgementEvidence>
	RecentPingAcknowledgementsForTesting();
	[[nodiscard]] static bool SchedulerTraceCoversDuration(
		const QString &trace,
		const std::vector<qint64> &sampleOffsetsNanoseconds,
		qint64 durationNanoseconds);
	[[nodiscard]] static bool SchedulerTraceProvesRunnableForDuration(
		const QString &trace,
		const std::vector<qint64> &sampleOffsetsNanoseconds,
		qint64 durationNanoseconds);

  protected:
	bool event(QEvent *event) override;

  private:
	void reportLoop();
	void enqueueReport(std::function<void()> report);
	void run();

	std::function<void(const QString &, bool update)> _log;
	std::deque<std::function<void()>> _reports;
	std::chrono::milliseconds _pingInterval;
	std::chrono::milliseconds _countThreshold;
	std::chrono::milliseconds _logThreshold;
	std::chrono::milliseconds _logRateLimit;
	bool _captureSchedulerTrace = false;
	std::thread _thread;
	std::thread _reportThread;
	std::mutex _mutex;
	std::condition_variable _condition;
	std::mutex _reportMutex;
	std::condition_variable _reportCondition;
	std::uint64_t _acknowledgedSequence = 0;
	std::atomic<std::uint64_t> _schedulerSamplingStopSequence = 0;
	std::chrono::steady_clock::time_point _acknowledgedAt;
	bool _started = false;
	bool _stopping = false;
	bool _reportStopping = false;
	bool _canCaptureStack = false;
	std::atomic<quint64> _stallCount = 0;
};

} // namespace Core
