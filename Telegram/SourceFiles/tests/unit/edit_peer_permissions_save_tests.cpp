/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_modify_requests.h"
#include "boxes/peers/edit_peer_permissions_save.h"

#include <vector>

namespace {

namespace Permissions = Ui::EditPeer::Permissions;

TEST_CASE(CustomSupergroupSaveOmitsPaidRequests) {
	const auto plan = Permissions::PlanSave(
		true,
		false,
		10,
		3,
		25);
	CHECK(!plan.migrateChat);
	CHECK(plan.saveSlowmode);
	CHECK(!plan.saveBoosts);
	CHECK(!plan.savePrice);
	CHECK_EQ(plan.requestCount(), 2);
}

TEST_CASE(CustomBasicGroupIgnoresHiddenPaidValues) {
	const auto plan = Permissions::PlanSave(
		false,
		false,
		10,
		3,
		25);
	CHECK(!plan.migrateChat);
	CHECK(!plan.saveSlowmode);
	CHECK(!plan.saveBoosts);
	CHECK(!plan.savePrice);
	CHECK_EQ(plan.requestCount(), 1);
}

TEST_CASE(StockSupergroupKeepsPaidRequests) {
	const auto plan = Permissions::PlanSave(
		true,
		true,
		10,
		3,
		25);
	CHECK(!plan.migrateChat);
	CHECK(plan.saveSlowmode);
	CHECK(plan.saveBoosts);
	CHECK(plan.savePrice);
	CHECK_EQ(plan.requestCount(), 4);
}

TEST_CASE(StockBasicGroupStillMigratesForSupportedSettings) {
	const auto plan = Permissions::PlanSave(
		false,
		true,
		10,
		3,
		25);
	CHECK(plan.migrateChat);
	CHECK(!plan.saveSlowmode);
	CHECK(!plan.saveBoosts);
	CHECK(!plan.savePrice);
}

TEST_CASE(PermissionsSaveWaitsForEveryResponseAndKeepsTheRightsError) {
	Permissions::SaveProgress progress(2);
	CHECK(!progress.complete(u"CHAT_ADMIN_REQUIRED"_q));
	CHECK(progress.complete(u"CHAT_NOT_MODIFIED"_q));
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
	CHECK_EQ(progress.error(), u"CHAT_ADMIN_REQUIRED"_q);
}

// Every request answering CHAT_NOT_MODIFIED is a successful save: the box
// must close, not report a failure. Kept separate from the mixed case, where
// the first real error wins and a CHAT_NOT_MODIFIED mishandling
// would stay hidden.
TEST_CASE(PermissionsSaveAllUnchangedSucceeds) {
	Permissions::SaveProgress progress(2);
	CHECK(!progress.complete(u"CHAT_NOT_MODIFIED"_q));
	CHECK(progress.complete(u"CHAT_NOT_MODIFIED"_q));
	CHECK(progress.finished());
	CHECK(progress.succeeded());
	CHECK(progress.error().isEmpty());
}

// The mirrored order of the mixed failure: rights succeed, slow mode
// fails, so the box stays open with the slow mode error.
TEST_CASE(PermissionsSaveKeepsTheSlowmodeError) {
	Permissions::SaveProgress progress(2);
	CHECK(!progress.complete(QString()));
	CHECK(progress.complete(u"CHAT_ADMIN_REQUIRED"_q));
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
	CHECK_EQ(progress.error(), u"CHAT_ADMIN_REQUIRED"_q);
}

// One save's request superseded by a newer save of the same setting, from a
// second Permissions box for the same peer: the cancelled request never answers,
// so the first save must finish on the supersession, not stay busy forever.
TEST_CASE(PermissionsSaveSupersededRequestDoesNotStayBusy) {
	Permissions::SaveProgress progress(2);
	CHECK(!progress.superseded());
	CHECK(progress.complete(QString()));
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
	CHECK(progress.error().isEmpty());
}

// Every request of a save cancelled by a newer save: Save still ends, and it
// does not claim success.
TEST_CASE(PermissionsSaveAllSupersededCompletes) {
	Permissions::SaveProgress progress(2);
	CHECK(!progress.superseded());
	CHECK(progress.superseded());
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
}

// A superseded request answers nothing, so a completion that arrives after the
// supersession must not run the count below zero: that would leave the save
// waiting for a request that can never arrive.
TEST_CASE(PermissionsSaveDoesNotCountARequestTwice) {
	Permissions::SaveProgress progress(1);
	CHECK(progress.superseded());
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
	CHECK(!progress.complete());
	CHECK(progress.finished());
	CHECK(!progress.succeeded());
}

// The registration path the Permissions boxes use, not SaveProgress on its own:
// a save from a second box for the same peer replaces the first box's in-flight
// request under the same key. A save of a custom basic group is one request, so
// the replaced save is told exactly once, and that notification is its last
// answer: `finished() && !succeeded()` is what the box keys on to keep Save
// retryable. Drop the notification from the registry and these tests fail
// along with the permanently busy Save they cover.
TEST_CASE(RegistrySupersedesTheInFlightSaveExactlyOnce) {
	Api::ModifyRequestRegistry registry;
	std::vector<int> cancelled;
	const auto cancel = [&](int requestId) { cancelled.push_back(requestId); };

	Permissions::SaveProgress first(1);
	int firstSuperseded = 0;
	int firstLastAnswer = 0;
	const auto onFirstSuperseded = [&] {
		++firstSuperseded;
		if (first.superseded()) {
			++firstLastAnswer;
		}
	};
	registry.registerRequest(
		"default_restrictions|1",
		101,
		onFirstSuperseded,
		cancel);
	CHECK_EQ(firstSuperseded, 0);
	CHECK(cancelled.empty());
	CHECK(!first.finished());

	Permissions::SaveProgress second(1);
	int secondSuperseded = 0;
	int secondLastAnswer = 0;
	registry.registerRequest(
		"default_restrictions|1",
		201,
		[&] {
			++secondSuperseded;
			if (second.superseded()) {
				++secondLastAnswer;
			}
		},
		cancel);

	CHECK_EQ(firstSuperseded, 1);
	CHECK_EQ(firstLastAnswer, 1);
	CHECK_EQ(secondSuperseded, 0);
	CHECK_EQ(secondLastAnswer, 0);
	CHECK_EQ(static_cast<int>(cancelled.size()), 1);
	CHECK_EQ(cancelled[0], 101);
	CHECK(first.finished());
	CHECK(!first.succeeded());
	CHECK(first.error().isEmpty());

	// The replacement save answers on its own and is not tied to the first.
	CHECK(second.complete(QString()));
	CHECK(second.succeeded());
	CHECK(!first.succeeded());
}

// A second box saving every setting of the peer replaces every request of the
// first: one notification per replaced request, its requests cancelled in order,
// and exactly one notification reported as the last answer.
TEST_CASE(RegistrySupersedesEveryRequestOfAReplacedSave) {
	Api::ModifyRequestRegistry registry;
	std::vector<int> cancelled;
	const auto cancel = [&](int requestId) { cancelled.push_back(requestId); };

	Permissions::SaveProgress first(2);
	int firstSuperseded = 0;
	int firstLastAnswer = 0;
	const auto onFirstSuperseded = [&] {
		++firstSuperseded;
		if (first.superseded()) {
			++firstLastAnswer;
		}
	};
	registry.registerRequest(
		"default_restrictions|1",
		101,
		onFirstSuperseded,
		cancel);
	registry.registerRequest(
		"slowmode_seconds|1",
		102,
		onFirstSuperseded,
		cancel);
	CHECK_EQ(firstSuperseded, 0);

	Permissions::SaveProgress second(2);
	int secondSuperseded = 0;
	int secondLastAnswer = 0;
	const auto onSecondSuperseded = [&] {
		++secondSuperseded;
		if (second.superseded()) {
			++secondLastAnswer;
		}
	};
	registry.registerRequest(
		"default_restrictions|1",
		201,
		onSecondSuperseded,
		cancel);
	registry.registerRequest(
		"slowmode_seconds|1",
		202,
		onSecondSuperseded,
		cancel);

	CHECK_EQ(firstSuperseded, 2);
	CHECK_EQ(firstLastAnswer, 1);
	CHECK_EQ(secondSuperseded, 0);
	CHECK_EQ(secondLastAnswer, 0);
	CHECK_EQ(static_cast<int>(cancelled.size()), 2);
	CHECK_EQ(cancelled[0], 101);
	CHECK_EQ(cancelled[1], 102);
	CHECK(first.finished());
	CHECK(!first.succeeded());

	// The replacement completes independently of the save it replaced.
	CHECK(!second.complete(QString()));
	CHECK(second.complete(QString()));
	CHECK(second.succeeded());
	CHECK(!first.succeeded());
}

// The key carries the peer, so a save in another window for another peer cannot
// supersede this peer's requests.
TEST_CASE(RegistryKeepsSavesOfDifferentPeersApart) {
	Api::ModifyRequestRegistry registry;
	std::vector<int> cancelled;
	const auto cancel = [&](int requestId) { cancelled.push_back(requestId); };

	int superseded = 0;
	const auto onSuperseded = [&] { ++superseded; };
	registry.registerRequest(
		"default_restrictions|1",
		101,
		onSuperseded,
		cancel);
	registry.registerRequest(
		"default_restrictions|2",
		102,
		onSuperseded,
		cancel);
	CHECK_EQ(superseded, 0);
	CHECK(cancelled.empty());
}

// A request that answered is cleared, so a later request under the same key is
// never reported to a save whose answer already arrived.
TEST_CASE(RegistryDoesNotSupersedeAnAnsweredRequest) {
	Api::ModifyRequestRegistry registry;
	std::vector<int> cancelled;
	const auto cancel = [&](int requestId) { cancelled.push_back(requestId); };

	int superseded = 0;
	const auto onSuperseded = [&] { ++superseded; };
	registry.registerRequest(
		"slowmode_seconds|1",
		101,
		onSuperseded,
		cancel);
	registry.clear("slowmode_seconds|1");
	registry.registerRequest(
		"slowmode_seconds|1",
		102,
		onSuperseded,
		cancel);
	CHECK_EQ(superseded, 0);
	CHECK(cancelled.empty());
}

TEST_CASE(LatePermissionsSaveSuccessCannotCompleteRetry) {
	Permissions::SaveProgress failed(2);
	Permissions::SaveProgress retry(2);
	CHECK(!failed.complete(u"CHAT_ADMIN_REQUIRED"_q));
	CHECK(!retry.complete());
	CHECK(failed.complete());
	CHECK(!failed.succeeded());
	CHECK(!retry.succeeded());
	CHECK(retry.complete());
	CHECK(retry.succeeded());
}

} // namespace
