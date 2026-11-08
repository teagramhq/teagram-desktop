/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_server.h"

namespace {

using Intro::details::SubmitSavedServer;

TEST_CASE(BrowserOnlyRefusalKeepsSavedServerStepInPlace) {
	auto message = QString();
	auto wentToUsername = false;
	const auto error = u"This server works only in the browser."_q;
	const auto showError = [&](QString value) { message = value; };
	const auto advance = [&] { wentToUsername = true; };
	SubmitSavedServer(false, false, true, error, showError, advance);

	CHECK_EQ(message, u"This server works only in the browser."_q);
	CHECK(!wentToUsername);
}

} // namespace
