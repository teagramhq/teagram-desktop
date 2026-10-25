/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ui_stall_watchdog.h"

#include "tests/unit/unit_test.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

// Spins without calling into libstdc++ or the vDSO: those are built without
// frame pointers, so a sample taken inside them would skip this frame.
__attribute__((noinline)) void
UiStallWatchdogTestBlock(std::chrono::milliseconds duration) {
	auto released = std::atomic_bool(false);
	auto releaser = std::thread([&] {
		std::this_thread::sleep_for(duration);
		released.store(true, std::memory_order_release);
	});
	while (!released.load(std::memory_order_acquire)) {
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}
	releaser.join();
}

class UiStallWatchdogTestBlockFilter final : public QObject {
  public:
	UiStallWatchdogTestBlockFilter(std::chrono::milliseconds duration,
								   QEventLoop *loop,
								   std::atomic_bool *logged = nullptr)
		: _duration(duration), _loop(loop), _logged(logged) {}

	[[nodiscard]] bool Blocked() const { return _blocked; }
	[[nodiscard]] bool LoggedWhileBlocked() const {
		return _loggedWhileBlocked;
	}

  protected:
	bool eventFilter(QObject *, QEvent *event) override {
		// Block while handling the second ping, posted to an idle loop.
		if (event->type() >= QEvent::User && ++_pingCount == 2) {
			_blocked = true;
			UiStallWatchdogTestBlock(_duration);
			_loggedWhileBlocked
				= _logged && _logged->load(std::memory_order_acquire);
			_loop->quit();
		}
		return false;
	}

  private:
	const std::chrono::milliseconds _duration;
	QEventLoop *_loop = nullptr;
	std::atomic_bool *_logged = nullptr;
	int _pingCount = 0;
	bool _blocked = false;
	bool _loggedWhileBlocked = false;
};

TEST_CASE(UiStallWatchdogLogsMainThreadStack) {
	auto lines = std::vector<QString>();
	auto mutex = std::mutex();
	auto logged = std::atomic_bool(false);
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([&](const QString &line) {
			const auto lock = std::lock_guard(mutex);
			lines.push_back(line);
			logged.store(true, std::memory_order_release);
		});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(300), &loop, &logged);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
		CHECK(blockFilter.LoggedWhileBlocked());
		const auto deadline
			= std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			{
				const auto lock = std::lock_guard(mutex);
				if (lines.size() >= 2) {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		QCoreApplication::processEvents();
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before + 1);
	CHECK_EQ(int(lines.size()), 2);
	if (lines.size() >= 2) {
		CHECK(lines.front().startsWith(u"UI stall "_q));
		CHECK(lines.front().contains(u"UiStallWatchdogTestBlock"_q));
		const auto detectedDuration = lines.front()
										  .mid(u"UI stall "_q.size())
										  .section(u' ', 0, 0)
										  .toInt();
		CHECK(detectedDuration >= 100);
		const auto prefix = u"UI watchdog acknowledgement after "_q;
		CHECK(lines.back().startsWith(prefix));
		const auto duration
			= lines.back().mid(prefix.size()).section(u' ', 0, 0).toInt();
		CHECK(duration >= 300);
	}
}

TEST_CASE(UiStallWatchdogIgnoresIdleTimeForShortStall) {
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([](const QString &) {});
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(20), &loop);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before);
}

TEST_CASE(UiStallWatchdogCountsButDoesNotLogMidThresholdStall) {
	auto lines = std::vector<QString>();
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog(
			[&](const QString &line) { lines.push_back(line); });
		auto blockFilter = UiStallWatchdogTestBlockFilter(
			std::chrono::milliseconds(70), &loop);
		watchdog.installEventFilter(&blockFilter);
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();
		CHECK(blockFilter.Blocked());
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before + 1);
	CHECK(lines.empty());
}
