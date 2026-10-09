/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "boxes/peers/edit_peer_permissions_save.h"

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
