/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "base/weak_ptr.h"
#include "settings/sections/settings_active_sessions_callbacks.h"
#include "settings/sections/settings_active_sessions_state.h"

#include <memory>

namespace {

using Settings::details::ActiveSessionsState;
using Settings::details::GuardTerminateAllCallbacks;

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

TEST_CASE(CachedSessionsStayVisibleAfterRefreshFailure) {
	auto state = ActiveSessionsState();
	state.initialize(true, true);

	const auto replay = state.receiveList(true, true);
	CHECK(!replay.clearRows);
	state.beginRefresh();
	CHECK(state.hasRows());
	CHECK(state.rowsVisible());

	const auto failed = state.refreshFailed();
	CHECK(failed.schedulePoll);
	CHECK(state.hasRows());
	CHECK(state.rowsVisible());
	CHECK(!state.loading());
}

TEST_CASE(InitialRefreshFailureStopsLoadingAndRetries) {
	auto state = ActiveSessionsState();
	state.initialize(false, false);
	const auto replay = state.receiveList(false, false);
	CHECK(replay.clearRows);
	CHECK(state.loading());
	state.beginRefresh();
	CHECK(state.loading());

	const auto failed = state.refreshFailed();
	CHECK(failed.schedulePoll);
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
