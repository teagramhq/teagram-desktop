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

} // namespace

TEST_CASE(EmojiShutdownDrainsQueuedAndRunningWork) {
	auto tasks = BackgroundTasks();
	tasks.restart();
	auto pending = tasks.start();
	CHECK(pending.has_value());
	auto done = std::promise<void>();
	auto drained = done.get_future();
	tasks.stop([&] { done.set_value(); });
	const auto closedWithPending = !tasks.start().has_value();
	const auto waitedForPending
		= drained.wait_for(0ms) == std::future_status::timeout;
	pending.reset();
	const auto drainedPending
		= drained.wait_for(2s) == std::future_status::ready;
	CHECK(closedWithPending);
	CHECK(drainedPending);
	CHECK(waitedForPending);
	CHECK(tasks.stoppedAndDrained());

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
	auto stopped = std::promise<void>();
	auto shutdown = stopped.get_future();
	tasks.stop([&] { stopped.set_value(); });
	const auto closedWithWorker = !tasks.start().has_value();
	const auto waitedForWorker
		= shutdown.wait_for(0ms) == std::future_status::timeout;
	release.set_value();
	worker.join();
	const auto drainedWorker
		= shutdown.wait_for(2s) == std::future_status::ready;
	CHECK(closedWithWorker);
	CHECK(drainedWorker);
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
	auto stopped = std::promise<void>();
	auto shutdown = stopped.get_future();
	tasks.stop([&] { stopped.set_value(); });
	const auto closed = !tasks.start().has_value();
	const auto waited = shutdown.wait_for(0ms) == std::future_status::timeout;
	release.set_value();
	worker.join();
	const auto drained = shutdown.wait_for(2s) == std::future_status::ready;
	CHECK(destructorStarted);
	CHECK(drained);
	CHECK(closed);
	CHECK(waited);
}
