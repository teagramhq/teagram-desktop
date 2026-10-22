/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "settings/sections/settings_active_sessions_state.h"

namespace {

using Settings::details::ActiveSessionsState;

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

} // namespace
