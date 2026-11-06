/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace Core {

class UiStallWatchdog final : public QObject {
  public:
	struct Settings {
		bool captureShortStalls = false;
	};

	explicit UiStallWatchdog(
		std::function<void(const QString &, bool update)> log);
	UiStallWatchdog(std::function<void(const QString &, bool update)> log,
					Settings settings);
	~UiStallWatchdog() override;

	UiStallWatchdog(const UiStallWatchdog &) = delete;
	UiStallWatchdog &operator=(const UiStallWatchdog &) = delete;

	[[nodiscard]] static quint64 StallCount();

  protected:
	bool event(QEvent *event) override;

  private:
	void run();

	std::function<void(const QString &, bool update)> _log;
	std::chrono::milliseconds _pingInterval;
	std::chrono::milliseconds _countThreshold;
	std::chrono::milliseconds _logThreshold;
	std::chrono::milliseconds _logRateLimit;
	std::thread _thread;
	std::mutex _mutex;
	std::condition_variable _condition;
	std::uint64_t _acknowledgedSequence = 0;
	std::chrono::steady_clock::time_point _acknowledgedAt;
	bool _started = false;
	bool _stopping = false;
	bool _canCaptureStack = false;
};

} // namespace Core
