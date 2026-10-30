/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "settings/sections/settings_invite_state.h"

namespace {

using namespace Settings::details;

TEST_CASE(InviteManagementCreateAndRevokeHappyPathSmoke) {
	const auto canCreate = InviteCreateConditions{
		.available = true,
		.loaded = true,
		.quota = 2,
		.weeklyCount = 1,
	};
	CHECK(CanCreateInvite(canCreate));

	auto request = InviteCreateRequest();
	CHECK(request.begin(CanCreateInvite(canCreate)));
	CHECK(request.active());
	CHECK(!request.begin(CanCreateInvite(canCreate)));
	auto displayedLinkCount = 0;
	if (request.finish()) {
		++displayedLinkCount;
	}
	if (request.finish()) {
		++displayedLinkCount;
	}
	CHECK_EQ(displayedLinkCount, 1);
	CHECK(request.begin(CanCreateInvite(canCreate)));
	CHECK(request.finish());

	auto afterCreate = canCreate;
	++afterCreate.weeklyCount;
	CHECK_EQ(InviteQuotaLeft(
		afterCreate.quota,
		afterCreate.weeklyCount), 0);
	CHECK(!CanCreateInvite(afterCreate));

	const auto nextAvailableAt = InviteNextAvailableAtAfterCreate(
		canCreate.quota,
		canCreate.weeklyCount,
		std::nullopt,
		1000);
	CHECK(nextAvailableAt.has_value());
	CHECK_EQ(*nextAvailableAt, 1000 + 7 * 86400);

	CHECK(CanRevokeInvite(u"pending"_q, false));
	CHECK(!CanRevokeInvite(u"pending"_q, true));
	CHECK(!CanRevokeInvite(u"used"_q, false));
}

TEST_CASE(InviteManagementQuotaAndUnavailableStates) {
	CHECK_EQ(InviteQuotaLeft(5, 2), 3);
	CHECK_EQ(InviteQuotaLeft(5, 7), 0);
	CHECK_EQ(InviteQuotaLeft(0, 0), 0);
	CHECK(InviteNextAvailableAtAfterCreate(5, 1, TimeId(2000), 1000)
		== std::optional<TimeId>(2000));
	CHECK(!InviteNextAvailableAtAfterCreate(5, 1, std::nullopt, 1000));

	const auto base = InviteCreateConditions{
		.available = true,
		.loaded = true,
		.quota = 5,
		.weeklyCount = 4,
	};
	auto notLoaded = base;
	notLoaded.loaded = false;
	CHECK(!CanCreateInvite(notLoaded));
	CHECK(CanCreateInvite(base));

	auto condition = base;
	condition.available = false;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.loaded = false;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.listLoading = true;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.creating = true;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.serverDisabled = true;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.createBlocked = true;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.weeklyCount = 5;
	CHECK(!CanCreateInvite(condition));
	condition = base;
	condition.quota = 0;
	CHECK(!CanCreateInvite(condition));
}

TEST_CASE(InviteManagementClassifiesListFailures) {
	CHECK(ClassifyInviteListFailure(u"NETWORK_TIMEOUT"_q)
		== InviteListFailure::Retry);
	CHECK(ClassifyInviteListFailure(u"USER_INVITES_DISABLED"_q)
		== InviteListFailure::Disabled);
}

TEST_CASE(InviteManagementClassifiesCreateFailures) {
	const auto waited = ClassifyInviteCreateFailure(
		u"USER_INVITE_ACCOUNT_TOO_NEW_WAIT_3600"_q,
		false);
	CHECK(waited.kind == InviteCreateFailureKind::AccountTooNew);
	CHECK(waited.waitSeconds.has_value());
	CHECK_EQ(*waited.waitSeconds, 3600);

	const auto unknownWait = ClassifyInviteCreateFailure(
		u"USER_INVITE_ACCOUNT_TOO_NEW_WAIT_unknown"_q,
		false);
	CHECK(unknownWait.kind == InviteCreateFailureKind::AccountTooNew);
	CHECK(!unknownWait.waitSeconds);

	const auto closed = ClassifyInviteCreateFailure(
		u"USER_INVITE_REGISTRATION_CLOSED"_q,
		false);
	CHECK(closed.kind == InviteCreateFailureKind::RegistrationClosed);
	CHECK(!closed.registrationOpen);

	const auto open = ClassifyInviteCreateFailure(
		u"USER_INVITE_REGISTRATION_CLOSED"_q,
		true);
	CHECK(open.kind == InviteCreateFailureKind::RegistrationClosed);
	CHECK(open.registrationOpen);

	CHECK(ClassifyInviteCreateFailure(u"USER_INVITES_DISABLED"_q, false).kind
		== InviteCreateFailureKind::InvitesDisabled);
	CHECK(ClassifyInviteCreateFailure(
			  u"USER_INVITE_WEEKLY_LIMIT_REACHED"_q,
			  false)
			.kind == InviteCreateFailureKind::WeeklyLimitReached);
	CHECK(ClassifyInviteCreateFailure(u"NETWORK_TIMEOUT"_q, false).kind
		== InviteCreateFailureKind::Other);
}

} // namespace
