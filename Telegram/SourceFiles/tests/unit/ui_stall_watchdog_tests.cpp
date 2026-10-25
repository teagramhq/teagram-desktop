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
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
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
	auto completed = std::atomic_bool(false);
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto loop = QEventLoop();
		auto watchdog
			= Core::UiStallWatchdog([&](const QString &line, bool update) {
				  const auto lock = std::lock_guard(mutex);
				  if (update) {
					  if (!lines.empty()) {
						  lines.back() = line;
					  }
					  completed.store(true, std::memory_order_release);
				  } else {
					  lines.push_back(line);
					  logged.store(true, std::memory_order_release);
				  }
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
		while (!completed.load(std::memory_order_acquire)
			   && std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		QCoreApplication::processEvents();
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before + 1);
	CHECK_EQ(int(lines.size()), 1);
	if (!lines.empty()) {
		CHECK(lines.front().startsWith(u"UI stall "_q));
		CHECK(lines.front().contains(u"UiStallWatchdogTestBlock"_q));
		const auto duration = lines.front()
								  .mid(u"UI stall "_q.size())
								  .section(u' ', 0, 0)
								  .toInt();
		CHECK(duration >= 300);
	}
}

TEST_CASE(UiStallLogWriterPreservesFollowingEntries) {
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
	CHECK(log.flush());
	log.close();

	auto output = QFile(path);
	CHECK(output.open(QIODevice::ReadWrite));
	if (!output.isOpen()) {
		return;
	}
	CHECK(writer.writeCompletion(output,
								 u"UI stall 325 ms (started test): stack"_q));
	output.close();

	auto result = QFile(path);
	CHECK(result.open(QIODevice::ReadOnly));
	if (!result.isOpen()) {
		return;
	}
	const auto content = QString::fromUtf8(result.readAll());
	const auto lines = content.split(u'\n', Qt::SkipEmptyParts);
	CHECK_EQ(int(lines.size()), 3);
	CHECK_EQ(content.count(u"UI stall "_q), 1);
	if (lines.size() == 3) {
		CHECK(lines.at(0).startsWith(
			u"[prefix] UI stall 325 ms (started test): stack"_q));
		CHECK_EQ(lines.at(1), u"[following] first log entry"_q);
		CHECK_EQ(lines.at(2), u"[following] second log entry"_q);
	}
}

TEST_CASE(UiStallWatchdogIgnoresIdleTimeForShortStall) {
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto loop = QEventLoop();
		auto watchdog = Core::UiStallWatchdog([](const QString &, bool) {});
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
			[&](const QString &line, bool) { lines.push_back(line); });
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
