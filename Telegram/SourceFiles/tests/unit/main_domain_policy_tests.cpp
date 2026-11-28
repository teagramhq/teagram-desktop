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
		{.hasSession = true, .sameEnvironment = true},
		{.sameEnvironment = true},
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
		{.hasSession = true, .sameEnvironment = true},
		{.hasSession = true, .sameEnvironment = true},
	};
	CHECK(!SelectSignupInviteSlot(slots).has_value());
}

TEST_CASE(SignupInviteSlotSelectionRejectsWrongEnvironmentAndPinnedSlots) {
	using Main::details::SelectSignupInviteSlot;
	using Main::details::SignupInviteSlot;

	const auto slots = std::vector<SignupInviteSlot>{
		{.sameEnvironment = false},
		{.sameEnvironment = true, .hasCustomServer = true},
		{.sameEnvironment = true, .blocked = true},
	};
	CHECK(!SelectSignupInviteSlot(slots).has_value());
}

TEST_CASE(SignupInviteCannotBeRetainedOnCustomServerIntro) {
	using Main::details::CanReceiveDefaultServerSignupInvite;

	CHECK(CanReceiveDefaultServerSignupInvite(false, false));
	CHECK(!CanReceiveDefaultServerSignupInvite(true, false));
	CHECK(!CanReceiveDefaultServerSignupInvite(false, true));
}
