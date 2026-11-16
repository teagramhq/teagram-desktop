/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "ui/emoji_background_tasks.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Ui::Emoji::internal::BackgroundTasks;

[[nodiscard]] bool WaitUntilClosed(BackgroundTasks &tasks) {
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (std::chrono::steady_clock::now() < deadline) {
		if (!tasks.start()) {
			return true;
		}
		std::this_thread::yield();
	}
	return false;
}

} // namespace

TEST_CASE(EmojiShutdownWaitsForQueuedAndRunningCacheWork) {

	auto tasks = BackgroundTasks();
	tasks.restart();
	auto pending = tasks.start();
	CHECK(pending.has_value());

	auto entered = std::promise<void>();
	auto finished = std::async(std::launch::async, [&] {
		entered.set_value();
		tasks.stopAndWait();
	});
	entered.get_future().wait();
	const auto closedWithPending = WaitUntilClosed(tasks);
	const auto waitedForPending
		= finished.wait_for(0ms) == std::future_status::timeout;
	pending.reset();
	finished.get();

	CHECK(closedWithPending);
	CHECK(waitedForPending);
	CHECK(!tasks.start().has_value());

	tasks.restart();
	auto running = tasks.start();
	CHECK(running.has_value());
	auto started = std::promise<void>();
	auto release = std::promise<void>();
	auto released = release.get_future();
	auto worker = std::thread(
		[token = std::move(running), &started, &released]() mutable {
			started.set_value();
			released.wait();
			token.reset();
		});
	started.get_future().wait();
	auto shutdown
		= std::async(std::launch::async, [&] { tasks.stopAndWait(); });
	const auto closedWithWorker = WaitUntilClosed(tasks);
	const auto waitedForWorker
		= shutdown.wait_for(0ms) == std::future_status::timeout;
	release.set_value();
	worker.join();
	shutdown.get();
	CHECK(closedWithWorker);
	CHECK(waitedForWorker);
}

TEST_CASE(EmojiShutdownWaitsForTaskCaptureDestruction) {
	auto tasks = BackgroundTasks();
	tasks.restart();
	auto pending = tasks.start();
	CHECK(pending.has_value());

	auto destroying = std::promise<void>();
	auto release = std::promise<void>();
	struct WaitOnDestruction {
		WaitOnDestruction(std::promise<void> &destroying,
						  std::shared_future<void> release)
			: destroying(destroying), release(std::move(release)) {}
		~WaitOnDestruction() {
			destroying.set_value();
			release.wait();
		}

		std::promise<void> &destroying;
		std::shared_future<void> release;
	};
	auto captured = std::make_unique<WaitOnDestruction>(
		destroying, release.get_future().share());
	auto work = [captured = std::move(captured)] { (void)captured; };
	auto task = Ui::Emoji::internal::TrackedTask<decltype(work)>{
		std::move(*pending), std::move(work)};
	auto worker = std::thread([task = std::move(task)]() mutable { task(); });
	const auto destructorStarted
		= destroying.get_future().wait_for(2s) == std::future_status::ready;
	auto shutdown
		= std::async(std::launch::async, [&] { tasks.stopAndWait(); });
	const auto closed = WaitUntilClosed(tasks);
	const auto waited = shutdown.wait_for(0ms) == std::future_status::timeout;
	release.set_value();
	worker.join();
	shutdown.get();
	CHECK(destructorStarted);
	CHECK(closed);
	CHECK(waited);
}
