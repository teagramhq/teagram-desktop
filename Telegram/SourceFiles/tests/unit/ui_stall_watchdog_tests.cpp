/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ui_stall_watchdog.h"

#include "tests/unit/unit_test.h"

#include <QtCore/QCoreApplication>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

// Spins without calling into libstdc++ or the vDSO: those are built without
// frame pointers, so a sample taken inside them would skip this frame.
__attribute__((noinline)) void UiStallWatchdogTestBlock() {
	auto released = std::atomic_bool(false);
	auto releaser = std::thread([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		released.store(true, std::memory_order_release);
	});
	while (!released.load(std::memory_order_acquire)) {
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}
	releaser.join();
}

TEST_CASE(UiStallWatchdogLogsMainThreadStack) {
	auto lines = std::vector<QString>();
	auto mutex = std::mutex();
	const auto before = Core::UiStallWatchdog::StallCount();
	{
		auto watchdog = Core::UiStallWatchdog([&](const QString &line) {
			const auto lock = std::lock_guard(mutex);
			lines.push_back(line);
		});
		QCoreApplication::processEvents();
		UiStallWatchdogTestBlock();
		const auto deadline
			= std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline) {
			QCoreApplication::processEvents();
			{
				const auto lock = std::lock_guard(mutex);
				if (!lines.empty()) {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		QCoreApplication::processEvents();
	}
	CHECK_EQ(Core::UiStallWatchdog::StallCount(), before + 1);
	CHECK_EQ(int(lines.size()), 1);
	if (!lines.empty()) {
		CHECK(lines.front().startsWith(u"UI stall "_q));
		CHECK(lines.front().contains(u"UiStallWatchdogTestBlock"_q));
	}
}
