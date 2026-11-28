/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "main/main_domain_policy.h"

#include <vector>

TEST_CASE(SignupInviteSlotSelectionSkipsSessionedAccounts) {
	using Main::details::SelectSignupInviteSlot;
	using Main::details::SignupInviteSlot;

	const auto slots = std::vector<SignupInviteSlot>{
		{.hasSession = true, .sameEnvironment = true, .defaultServer = true},
		{.sameEnvironment = true, .defaultServer = true},
	};
	const auto selected = SelectSignupInviteSlot(slots);
	CHECK(selected.has_value());
	if (selected) {
		CHECK(*selected == 1U);
	}
}

TEST_CASE(SignupInviteSlotSelectionFailsWhenAllSlotsAreUnavailable) {
	using Main::details::SelectSignupInviteSlot;
	using Main::details::SignupInviteSlot;

	const auto slots = std::vector<SignupInviteSlot>{
		{.hasSession = true, .sameEnvironment = true, .defaultServer = true},
		{.sameEnvironment = true, .defaultServer = false},
	};
	CHECK(!SelectSignupInviteSlot(slots).has_value());
}

TEST_CASE(SignupInviteSlotSelectionRequiresAnUnsessionedDefaultOriginSlot) {
	using Main::details::SelectSignupInviteSlot;
	using Main::details::SignupInviteSlot;

	const auto slots = std::vector<SignupInviteSlot>{
		{.sameEnvironment = false, .defaultServer = true},
		{.sameEnvironment = true, .defaultServer = false},
		{.sameEnvironment = true, .defaultServer = true, .blocked = true},
		{.sameEnvironment = true, .defaultServer = true},
	};
	const auto selected = SelectSignupInviteSlot(slots);
	CHECK(selected.has_value());
	if (selected) {
		CHECK(*selected == 3U);
	}
}

TEST_CASE(SignupInviteMayBeRetainedOnlyBeforeEnrollmentOrOnDefaultOrigin) {
	using Main::details::CanReceiveDefaultServerSignupInvite;

	CHECK(CanReceiveDefaultServerSignupInvite(false, false, false));
	CHECK(CanReceiveDefaultServerSignupInvite(true, true, false));
	CHECK(!CanReceiveDefaultServerSignupInvite(true, false, false));
	CHECK(!CanReceiveDefaultServerSignupInvite(false, false, true));
}
