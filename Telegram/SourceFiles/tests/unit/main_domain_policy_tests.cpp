/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "main/main_domain_policy.h"

TEST_CASE(SignupInviteReuseRequiresDefaultServerAccountSlot) {
	using Main::details::CanReuseForSignupInvite;

	CHECK(CanReuseForSignupInvite(false, true, false, false));
	CHECK(!CanReuseForSignupInvite(false, true, true, false));
	CHECK(!CanReuseForSignupInvite(false, true, false, true));
	CHECK(!CanReuseForSignupInvite(true, true, false, false));
	CHECK(!CanReuseForSignupInvite(false, false, false, false));
}
