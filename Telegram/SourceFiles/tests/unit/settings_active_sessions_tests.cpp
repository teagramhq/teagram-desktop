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

namespace {

using Api::details::AuthorizationsReloadState;
using Settings::details::ActiveSessionsState;
using Settings::details::BindReloadFailures;
using Settings::details::GuardTerminateAllCallbacks;
using Settings::details::HandleReloadFailure;

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

TEST_CASE(CachedSessionsStayVisibleWhenRefreshTimesOut) {
	auto state = ActiveSessionsState();
	state.initialize(true, true);
	auto reload = AuthorizationsReloadState();
	const auto generation = reload.begin();
	reload.setRequestId(generation, 1);
	auto retryDelayMs = 0;
	auto lifetime = rpl::lifetime();
	BindReloadFailures(
		reload.failures(),
		[&] {
			HandleReloadFailure(state,
								[&](int delayMs) { retryDelayMs = delayMs; });
		},
		lifetime);

	const auto replay = state.receiveList(true, true);
	CHECK(!replay.clearRows);
	state.beginRefresh();
	CHECK(state.hasRows());
	CHECK(state.rowsVisible());

	CHECK_EQ(reload.timeout(generation), 1);
	CHECK(!reload.pending());
	CHECK_EQ(retryDelayMs, ActiveSessionsState::kFailedReloadRetryDelayMs);
	CHECK(state.hasRows());
	CHECK(state.rowsVisible());
	CHECK(!state.loading());

	const auto retryGeneration = reload.begin();
	reload.setRequestId(retryGeneration, 2);
	CHECK(!reload.complete(generation));
	CHECK(reload.pending());
}

TEST_CASE(InitialRefreshTimeoutStopsLoadingAndRetries) {
	auto state = ActiveSessionsState();
	state.initialize(false, false);
	auto reload = AuthorizationsReloadState();
	const auto generation = reload.begin();
	reload.setRequestId(generation, 1);
	auto retryDelayMs = 0;
	auto lifetime = rpl::lifetime();
	BindReloadFailures(
		reload.failures(),
		[&] {
			HandleReloadFailure(state,
								[&](int delayMs) { retryDelayMs = delayMs; });
		},
		lifetime);
	const auto replay = state.receiveList(false, false);
	CHECK(replay.clearRows);
	CHECK(state.loading());
	state.beginRefresh();
	CHECK(state.loading());

	CHECK_EQ(reload.timeout(generation), 1);
	CHECK(!reload.pending());
	CHECK_EQ(retryDelayMs, ActiveSessionsState::kFailedReloadRetryDelayMs);
	CHECK(!state.loading());
	CHECK(!state.rowsVisible());

	state.beginRefresh();
	CHECK(state.loading());
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

	const auto emptyResponse = state.receiveList(false, true);
	CHECK(emptyResponse.clearRows);
	CHECK(emptyResponse.schedulePoll);
	CHECK(!state.hasRows());
	CHECK(!state.loading());
	CHECK(!state.rowsVisible());
}

TEST_CASE(CompletedInitialEmptyListStopsLoading) {
	auto state = ActiveSessionsState();
	state.initialize(false, false);
	CHECK(state.loading());

	const auto emptyResponse = state.receiveList(false, true);
	CHECK(emptyResponse.clearRows);
	CHECK(emptyResponse.schedulePoll);
	CHECK(!state.loading());
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
