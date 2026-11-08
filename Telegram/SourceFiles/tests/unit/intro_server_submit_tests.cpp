/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_server.h"

namespace {

TEST_CASE(BrowserOnlyRefusalKeepsSavedServerStepInPlace) {
	auto message = QString();
	auto wentToUsername = false;
	Intro::details::SubmitSavedServer(
		false, false, true, [&](QString error) { message = error; },
		[&] { wentToUsername = true; });

	CHECK_EQ(message, u"This server works only in the browser."_q);
	CHECK(!wentToUsername);
}

} // namespace
