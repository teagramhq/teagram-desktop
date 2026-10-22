/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_authorizations.h"
#include "base/weak_ptr.h"
#include "settings/sections/settings_active_sessions_callbacks.h"
#include "settings/sections/settings_active_sessions_state.h"

#include <rpl/event_stream.h>

#include <memory>
#include <utility>
#include <vector>

namespace {

using Api::details::AuthorizationsReloadState;
using Api::details::RunAuthorizationsReload;
using Api::details::ScheduleAuthorizationsReloadTimeout;
using Settings::details::ActiveSessionsState;
using Settings::details::BindReloadFailures;
using Settings::details::GuardTerminateAllCallbacks;
using Settings::details::HandleReloadFailure;

struct FakeReloadTransport final {
	struct Request {
		mtpRequestId id = 0;
		Fn<void(const int &)> done;
		Fn<void(const MTP::Error &)> fail;
	};

	mtpRequestId send(Fn<void(const int &)> done,
					  Fn<void(const MTP::Error &)> fail) {
		const auto id = mtpRequestId(requests.size() + 1);
		requests.push_back({id, std::move(done), std::move(fail)});
		return id;
	}

	void cancel(mtpRequestId id) { cancelled.push_back(id); }

	std::vector<Request> requests;
	std::vector<mtpRequestId> cancelled;
};

struct FakeReloadClock final {
	template <typename Callback>
	void schedule(crl::time delay, Callback &&callback) {
		ScheduleAuthorizationsReloadTimeout(*this, action, delay,
											std::forward<Callback>(callback));
	}

	void callOnce(crl::time delay) {
		delayMs = delay;
		timeout = std::move(action);
	}

	void cancel() {
		action = nullptr;
		timeout = nullptr;
	}

	void advance() {
		auto callback = std::exchange(timeout, nullptr);
		if (callback) {
			callback();
		}
	}

	crl::time delayMs = 0;
	Fn<void()> action;
	Fn<void()> timeout;
};

TEST_CASE(CachedSessionsStayVisibleDuringRefresh) {
	auto state = ActiveSessionsState();
	state.initialize(true, true);
	CHECK(state.rowsVisible());

	const auto replay = state.receiveList(true, true);
	CHECK(!replay.clearRows);
	CHECK(state.rowsVisible());

	state.beginRefresh();
	CHECK(state.rowsVisible());
	CHECK(!state.loading());
}

TEST_CASE(CachedSessionsStayVisibleAndMarkedStaleAfterReloadTimeout) {
	auto sessions = ActiveSessionsState();
	sessions.initialize(true, true);
	const auto replay = sessions.receiveList(true, true);
	CHECK(!replay.clearRows);
	CHECK(sessions.rowsVisible());

	auto reload = AuthorizationsReloadState();
	auto transport = FakeReloadTransport();
	auto clock = FakeReloadClock();
	auto retryClock = FakeReloadClock();
	auto retryDelayMs = 0;
	auto lifetime = rpl::lifetime();
	const auto start = [&] {
		RunAuthorizationsReload(
			reload, AuthorizationsReloadState::kTimeoutMs,
			[&](auto done, auto fail) {
				return transport.send(std::move(done), std::move(fail));
			},
			[&](crl::time delayMs, auto callback) {
				clock.schedule(delayMs, std::move(callback));
			},
			[&] { clock.cancel(); },
			[&](mtpRequestId id) { transport.cancel(id); },
			[&](const int &rows) {
				sessions.reloadSucceeded();
				const auto transition = sessions.receiveList(rows != 0, true);
				CHECK_EQ(transition.clearRows, rows == 0);
				CHECK(transition.schedulePoll);
			});
	};
	BindReloadFailures(
		reload.failures(),
		[&, start](const MTP::Error &error) {
			HandleReloadFailure(sessions, error, [&](int delayMs) {
				retryDelayMs = delayMs;
				retryClock.schedule(delayMs, [&, start] {
					if (!sessions.shouldUseCachedList(true, true)) {
						sessions.beginRefresh();
						start();
					}
				});
			});
		},
		lifetime);

	sessions.beginRefresh();
	start();
	CHECK_EQ(int(transport.requests.size()), 1);
	CHECK_EQ(int(clock.delayMs), AuthorizationsReloadState::kTimeoutMs);
	CHECK(sessions.rowsVisible());

	clock.advance();
	CHECK_EQ(int(transport.cancelled.size()), 1);
	CHECK_EQ(transport.cancelled.front(), transport.requests.front().id);
	CHECK(!reload.pending());
	CHECK_EQ(retryDelayMs, ActiveSessionsState::kFailedReloadRetryDelayMs);
	CHECK(sessions.hasRows());
	CHECK(sessions.rowsVisible());
	CHECK(sessions.failed());
	CHECK_EQ(sessions.failureType(), u"AUTHORIZATIONS_RELOAD_TIMEOUT"_q);
	CHECK_EQ(reload.lastFailureType(), u"AUTHORIZATIONS_RELOAD_TIMEOUT"_q);
	CHECK(!sessions.shouldUseCachedList(true, true));
	const auto replayAgain = sessions.receiveList(true, true);
	CHECK(!replayAgain.clearRows);
	CHECK(replayAgain.schedulePoll);
	CHECK(sessions.failed());
	auto reopened = ActiveSessionsState();
	reopened.initialize(true, true, reload.lastFailureType());
	CHECK(reopened.failed());
	CHECK(reopened.contentVisible());

	CHECK_EQ(int(retryClock.delayMs),
			 ActiveSessionsState::kFailedReloadRetryDelayMs);
	retryClock.advance();
	CHECK_EQ(int(transport.requests.size()), 2);
	CHECK(reload.pending());
	transport.requests.front().done(1);
	CHECK(reload.pending());
	CHECK(sessions.failed());
	transport.requests.back().done(0);
	CHECK(!reload.pending());
	CHECK(!sessions.failed());
	CHECK(sessions.failureType().isEmpty());
	CHECK(reload.lastFailureType().isEmpty());
	CHECK(sessions.contentVisible());
}

TEST_CASE(ColdReloadTimeoutShowsFailureUntilEmptyResponse) {
	auto sessions = ActiveSessionsState();
	sessions.initialize(false, false);
	CHECK(sessions.loading());
	CHECK(!sessions.contentVisible());

	auto reload = AuthorizationsReloadState();
	auto transport = FakeReloadTransport();
	auto clock = FakeReloadClock();
	auto retryClock = FakeReloadClock();
	auto retryDelayMs = 0;
	auto lifetime = rpl::lifetime();
	const auto start = [&] {
		RunAuthorizationsReload(
			reload, AuthorizationsReloadState::kTimeoutMs,
			[&](auto done, auto fail) {
				return transport.send(std::move(done), std::move(fail));
			},
			[&](crl::time delayMs, auto callback) {
				clock.schedule(delayMs, std::move(callback));
			},
			[&] { clock.cancel(); },
			[&](mtpRequestId id) { transport.cancel(id); },
			[&](const int &rows) {
				sessions.reloadSucceeded();
				const auto transition = sessions.receiveList(rows != 0, true);
				CHECK_EQ(transition.clearRows, rows == 0);
				CHECK(transition.schedulePoll);
			});
	};
	BindReloadFailures(
		reload.failures(),
		[&, start](const MTP::Error &error) {
			HandleReloadFailure(sessions, error, [&](int delayMs) {
				retryDelayMs = delayMs;
				retryClock.schedule(delayMs, [&, start] {
					if (!sessions.shouldUseCachedList(true, true)) {
						sessions.beginRefresh();
						start();
					}
				});
			});
		},
		lifetime);

	start();
	clock.advance();
	CHECK(!sessions.loading());
	CHECK(sessions.failed());
	CHECK(!sessions.contentVisible());
	CHECK(!reload.lastFailureType().isEmpty());
	CHECK_EQ(retryDelayMs, ActiveSessionsState::kFailedReloadRetryDelayMs);
	CHECK(!sessions.shouldUseCachedList(true, true));

	retryClock.advance();
	CHECK(!sessions.loading());
	CHECK(sessions.failed());
	CHECK_EQ(int(transport.requests.size()), 2);
	transport.requests.back().done(0);
	CHECK(!sessions.loading());
	CHECK(!sessions.failed());
	CHECK(reload.lastFailureType().isEmpty());
	CHECK(sessions.contentVisible());
	CHECK(!sessions.hasRows());
}

TEST_CASE(TerminateAllInvalidationClearsCachedSessions) {
	auto state = ActiveSessionsState();
	state.initialize(true, true);
	const auto replay = state.receiveList(true, true);
	CHECK(!replay.clearRows);
	state.beginTerminateAll();
	CHECK(!state.rowsVisible());

	const auto invalidation = state.receiveList(false, false);
	CHECK(invalidation.clearRows);
	CHECK(invalidation.schedulePoll);
	CHECK(!state.hasRows());
	CHECK(state.loading());
	CHECK(!state.contentVisible());

	const auto failed = state.refreshFailed(u"AUTHORIZATIONS_RELOAD_TIMEOUT"_q);
	CHECK(failed.schedulePoll);
	CHECK(state.failed());
	CHECK(!state.contentVisible());

	state.reloadSucceeded();
	const auto emptyResponse = state.receiveList(false, true);
	CHECK(emptyResponse.clearRows);
	CHECK(emptyResponse.schedulePoll);
	CHECK(!state.hasRows());
	CHECK(!state.loading());
	CHECK(!state.rowsVisible());
	CHECK(!state.failed());
	CHECK(state.contentVisible());
}

TEST_CASE(CompletedInitialEmptyListStopsLoading) {
	auto state = ActiveSessionsState();
	state.initialize(false, false);
	CHECK(state.loading());

	state.reloadSucceeded();
	const auto emptyResponse = state.receiveList(false, true);
	CHECK(emptyResponse.clearRows);
	CHECK(emptyResponse.schedulePoll);
	CHECK(!state.loading());
	CHECK(!state.failed());
	CHECK(state.contentVisible());
}

TEST_CASE(TerminateAllCallbacksIgnoreResultsAfterContentDestruction) {
	struct CallbackOwner final : base::has_weak_ptr {};
	auto owner = std::make_unique<CallbackOwner>();
	const auto weak = base::make_weak(owner.get());
	auto callbackCount = 0;
	auto [done, fail] = GuardTerminateAllCallbacks(
		weak,
		[&](bool result) {
			if (!result) {
				++callbackCount;
			}
		},
		[&](int) { ++callbackCount; });

	owner.reset();
	done(false);
	fail(1);
	CHECK_EQ(callbackCount, 0);
}

} // namespace
