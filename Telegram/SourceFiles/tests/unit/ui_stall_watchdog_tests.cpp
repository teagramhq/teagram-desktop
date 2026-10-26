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
#include <utility>
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
			UiStallWatchdogTestBlock(_durations[_nextBlock]);
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
				  }
				  if (blockActive.load(std::memory_order_acquire)) {
					  logged.store(true, std::memory_order_release);
				  }
			  });
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
	const auto before = Core::UiStallWatchdog::StallCount();
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
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
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
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before + 2);
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
