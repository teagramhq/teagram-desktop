/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_instance.h"

#include "tests/unit/unit_test.h"

TEST_CASE(UnsupportedCallStartLeavesExistingCallAlone) {
	auto liveStockCall = 1;
	auto unavailableShown = false;
	if (Calls::details::AllowCallStart(false, [&] {
			unavailableShown = true;
		})) {
		liveStockCall = 0;
	}
	CHECK_EQ(liveStockCall, 1);
	CHECK(unavailableShown);
}

TEST_CASE(SupportedCallStartDoesNotShowUnavailableToast) {
	auto actionStarted = false;
	auto unavailableShown = false;
	if (Calls::details::AllowCallStart(true, [&] {
			unavailableShown = true;
		})) {
		actionStarted = true;
	}
	CHECK(actionStarted);
	CHECK(!unavailableShown);
}
