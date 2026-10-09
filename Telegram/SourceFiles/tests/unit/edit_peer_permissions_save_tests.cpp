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
