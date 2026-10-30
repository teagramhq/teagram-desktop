/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "settings/sections/settings_invite_state.h"

#include <functional>
#include <utility>
#include <vector>

namespace {

using namespace Settings::details;

struct FakeInviteListResponse {
	std::vector<int64> ids;
	int64 quota = 0;
	int64 weeklyCount = 0;
};

struct FakeInviteLinkResponse {
	int64 id = 0;
	QString secret;
};

class FakeInviteTransport final {
  public:
	using ListDone = std::function<void(const FakeInviteListResponse &)>;
	using CreateDone = std::function<void(const FakeInviteLinkResponse &)>;
	using RevokeDone = std::function<void(const bool &)>;
	using Fail = std::function<void(const QString &)>;

	mtpRequestId list(ListDone done, [[maybe_unused]] Fail fail) {
		++listRequests;
		_listDone = std::move(done);
		return mtpRequestId(listRequests);
	}

	mtpRequestId create(CreateDone done, Fail fail) {
		++createRequests;
		_createDone = std::move(done);
		_createFail = std::move(fail);
		return mtpRequestId(createRequests);
	}

	mtpRequestId revoke(int64 id, RevokeDone done, [[maybe_unused]] Fail fail) {
		++revokeRequests;
		revokedIds.push_back(id);
		_revokeDone = std::move(done);
		return mtpRequestId(revokeRequests);
	}

	void succeedList(FakeInviteListResponse response) {
		const auto done = std::exchange(_listDone, nullptr);
		if (done) {
			done(response);
		}
	}

	void succeedCreate(const FakeInviteLinkResponse &response,
					   int deliveries = 1) {
		const auto done = _createDone;
		for (auto i = 0; i != deliveries && done; ++i) {
			done(response);
		}
		_createDone = nullptr;
		_createFail = nullptr;
	}

	void failCreate(const QString &error) {
		const auto fail = std::exchange(_createFail, nullptr);
		_createDone = nullptr;
		if (fail) {
			fail(error);
		}
	}

	void succeedRevoke(bool result) {
		const auto done = std::exchange(_revokeDone, nullptr);
		if (done) {
			done(result);
		}
	}

	int listRequests = 0;
	int createRequests = 0;
	int revokeRequests = 0;
	std::vector<int64> revokedIds;

  private:
	ListDone _listDone;
	CreateDone _createDone;
	RevokeDone _revokeDone;
	Fail _createFail;
};

TEST_CASE(InviteManagementHappyPathSmoke) {
	auto transport = FakeInviteTransport();
	auto available = true;
	auto listRequestId = mtpRequestId(0);
	auto list = FakeInviteListResponse();
	const auto loadList = [&] {
		const auto requestId = RunInviteListRequest(
			available, listRequestId, [] {},
			[&](auto done, auto fail) {
				return transport.list(std::move(done), std::move(fail));
			},
			[&](const FakeInviteListResponse &response) {
				listRequestId = 0;
				list = response;
			},
			[&](const QString &) { listRequestId = 0; });
		if (requestId) {
			listRequestId = *requestId;
		}
	};

	loadList();
	CHECK_EQ(transport.listRequests, 1);
	loadList();
	CHECK_EQ(transport.listRequests, 1);
	transport.succeedList({.ids = {41}, .quota = 2, .weeklyCount = 0});
	CHECK_EQ(int(list.ids.size()), 1);
	CHECK_EQ(list.ids.front(), 41);

	auto conditions = InviteCreateConditions{
		.available = available,
		.loaded = true,
		.quota = list.quota,
		.weeklyCount = list.weeklyCount,
	};
	auto createRequest = InviteCreateRequest();
	auto creating = false;
	auto createdInviteId = int64(0);
	auto displayedLinks = std::vector<QString>();
	const auto createInvite = [&] {
		return RunInviteCreateRequest(
			createRequest, conditions, [&] { creating = true; },
			[&](auto done, auto fail) {
				return transport.create(std::move(done), std::move(fail));
			},
			[&](const FakeInviteLinkResponse &response) {
				creating = false;
				createdInviteId = response.id;
				++conditions.weeklyCount;
				displayedLinks.push_back(response.secret);
				loadList();
			},
			[&](const QString &) { creating = false; });
	};
	CHECK(createInvite().has_value());
	CHECK(creating);
	CHECK(!createInvite().has_value());
	CHECK_EQ(transport.createRequests, 1);
	transport.succeedCreate({.id = 42, .secret = u"secret-42"_q}, 2);
	CHECK(!creating);
	CHECK_EQ(int(displayedLinks.size()), 1);
	CHECK_EQ(displayedLinks.front(), u"secret-42"_q);
	CHECK_EQ(transport.listRequests, 2);
	transport.succeedList({.ids = {41, 42}, .quota = 2, .weeklyCount = 1});
	CHECK_EQ(int(list.ids.size()), 2);

	auto revokeRequestId = mtpRequestId(0);
	const auto revokeInvite = [&] {
		const auto requestId = RunInviteRevokeRequest(
			u"pending"_q, bool(revokeRequestId), [] {},
			[&](auto done, auto fail) {
				return transport.revoke(createdInviteId, std::move(done),
										std::move(fail));
			},
			[&](const bool &success) {
				revokeRequestId = 0;
				if (success) {
					loadList();
				}
			},
			[&](const QString &) { revokeRequestId = 0; });
		if (requestId) {
			revokeRequestId = *requestId;
		}
		return requestId;
	};
	CHECK(revokeInvite().has_value());
	CHECK(!revokeInvite().has_value());
	CHECK_EQ(transport.revokeRequests, 1);
	CHECK_EQ(transport.revokedIds.front(), 42);
	transport.succeedRevoke(true);
	CHECK_EQ(transport.listRequests, 3);
	transport.succeedList({.ids = {41}, .quota = 2, .weeklyCount = 1});
	CHECK_EQ(int(list.ids.size()), 1);
	CHECK_EQ(list.ids.front(), 41);
	CHECK_EQ(int(displayedLinks.size()), 1);
}

TEST_CASE(InviteManagementRevokeEligibility) {
	CHECK(CanRevokeInvite(u"pending"_q, false));
	CHECK(!CanRevokeInvite(u"pending"_q, true));
	CHECK(!CanRevokeInvite(u"used"_q, false));
}

TEST_CASE(InviteCreateFailureThenRetryShowsOnlySuccessfulLink) {
	auto transport = FakeInviteTransport();
	auto request = InviteCreateRequest();
	auto displayedLinks = std::vector<QString>();
	auto failures = 0;
	const auto conditions = InviteCreateConditions{
		.available = true,
		.loaded = true,
		.quota = 2,
		.weeklyCount = 0,
	};
	const auto create = [&] {
		return RunInviteCreateRequest(
			request, conditions, [] {},
			[&](auto done, auto fail) {
				return transport.create(std::move(done), std::move(fail));
			},
			[&](const FakeInviteLinkResponse &response) {
				displayedLinks.push_back(response.secret);
			},
			[&](const QString &) { ++failures; });
	};
	CHECK(create().has_value());
	transport.failCreate(u"NETWORK_TIMEOUT"_q);
	CHECK_EQ(failures, 1);
	CHECK(!request.active());
	CHECK(displayedLinks.empty());

	CHECK(create().has_value());
	transport.succeedCreate({.id = 43, .secret = u"secret-43"_q});
	CHECK_EQ(int(displayedLinks.size()), 1);
	CHECK_EQ(displayedLinks.front(), u"secret-43"_q);
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
		u"USER_INVITE_ACCOUNT_TOO_NEW_WAIT_3600"_q, false);
	CHECK(waited.kind == InviteCreateFailureKind::AccountTooNew);
	CHECK(waited.waitSeconds.has_value());
	CHECK_EQ(*waited.waitSeconds, 3600);

	const auto unknownWait = ClassifyInviteCreateFailure(
		u"USER_INVITE_ACCOUNT_TOO_NEW_WAIT_unknown"_q, false);
	CHECK(unknownWait.kind == InviteCreateFailureKind::AccountTooNew);
	CHECK(!unknownWait.waitSeconds);

	const auto closed = ClassifyInviteCreateFailure(
		u"USER_INVITE_REGISTRATION_CLOSED"_q, false);
	CHECK(closed.kind == InviteCreateFailureKind::RegistrationClosed);
	CHECK(!closed.registrationOpen);

	const auto open = ClassifyInviteCreateFailure(
		u"USER_INVITE_REGISTRATION_CLOSED"_q, true);
	CHECK(open.kind == InviteCreateFailureKind::RegistrationClosed);
	CHECK(open.registrationOpen);

	CHECK(ClassifyInviteCreateFailure(u"USER_INVITES_DISABLED"_q, false).kind
		  == InviteCreateFailureKind::InvitesDisabled);
	CHECK(ClassifyInviteCreateFailure(u"USER_INVITE_WEEKLY_LIMIT_REACHED"_q,
									  false)
			  .kind
		  == InviteCreateFailureKind::WeeklyLimitReached);
	CHECK(ClassifyInviteCreateFailure(u"NETWORK_TIMEOUT"_q, false).kind
		  == InviteCreateFailureKind::Other);
}

} // namespace
