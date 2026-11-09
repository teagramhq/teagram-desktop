/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_server.h"

namespace {

using Intro::details::CanCommitServerSelection;
using Intro::details::ServerStepIsReadOnly;
using Intro::details::ServerStepState;
using Intro::details::SubmitSavedServer;

TEST_CASE(BrowserOnlyRefusalReopensSavedServerSelection) {
	auto refused = ServerStepState();
	refused.hasCustomServer = true;
	refused.browserOnlyServerRefused = true;
	CHECK(!ServerStepIsReadOnly(refused));

	auto ordinary = refused;
	ordinary.browserOnlyServerRefused = false;
	CHECK(ServerStepIsReadOnly(ordinary));

	auto authorized = refused;
	authorized.hasSession = true;
	CHECK(ServerStepIsReadOnly(authorized));

	auto blocked = refused;
	blocked.blocked = true;
	CHECK(ServerStepIsReadOnly(blocked));
}

TEST_CASE(BrowserOnlyRefusalAllowsReplacingPreAuthServer) {
	CHECK(CanCommitServerSelection(ServerStepState()));

	auto refused = ServerStepState();
	refused.hasCustomServer = true;
	refused.browserOnlyServerRefused = true;
	CHECK(CanCommitServerSelection(refused));

	auto ordinary = refused;
	ordinary.browserOnlyServerRefused = false;
	CHECK(!CanCommitServerSelection(ordinary));

	auto blocked = refused;
	blocked.blocked = true;
	CHECK(!CanCommitServerSelection(blocked));

	auto authorized = refused;
	authorized.hasSession = true;
	CHECK(!CanCommitServerSelection(authorized));
}

TEST_CASE(SubmitSavedServerShowsErrorOnRefusal) {
	auto showedError = false;
	auto wentToUsername = false;
	const auto showError = [&] { showedError = true; };
	const auto advance = [&] { wentToUsername = true; };
	SubmitSavedServer(false, false, true, showError, advance);

	CHECK(showedError);
	CHECK(!wentToUsername);
}

} // namespace
