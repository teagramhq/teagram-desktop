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
	auto showedError = false;
	auto wentToUsername = false;
	const auto showError = [&] { showedError = true; };
	const auto advance = [&] { wentToUsername = true; };
	SubmitSavedServer(false, false, true, showError, advance);

	CHECK(showedError);
	CHECK(!wentToUsername);
}

} // namespace
