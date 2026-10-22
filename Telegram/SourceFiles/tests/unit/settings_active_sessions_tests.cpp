/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "settings/sections/settings_active_sessions_state.h"

namespace {

TEST_CASE(ActiveSessionsShowCachedRowsWhileRefreshing) {
	CHECK(!Settings::details::ShouldShowSessionsLoading(false, true));
}

TEST_CASE(ActiveSessionsWaitForTheFirstResponse) {
	CHECK(Settings::details::ShouldShowSessionsLoading(false, false));
}

TEST_CASE(ActiveSessionsFinishLoadingAfterAnEmptyResponse) {
	const auto transition
		= Settings::details::HandleEmptySessionsResponse(true, true);
	CHECK(transition.updateLoading);
	CHECK(!transition.loading);
	CHECK(transition.schedulePoll);
}

TEST_CASE(ActiveSessionsStayLoadingUntilAnEmptyResponseArrives) {
	const auto transition
		= Settings::details::HandleEmptySessionsResponse(false, true);
	CHECK(!transition.updateLoading);
	CHECK(transition.loading);
	CHECK(!transition.schedulePoll);
}

} // namespace
