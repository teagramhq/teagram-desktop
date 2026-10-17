/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_chat_participants.h"
#include "data/data_user_loading.h"

namespace {

using Data::details::BasicChatAdminCapabilitiesFor;
using Data::details::BasicChatAdminDefaultRightsFor;
using Data::details::BasicChatAdminRightsFor;
using Data::details::BasicChatAdminRoleForSave;
using Data::details::BasicChatControlsFor;
using Data::details::BasicChatRole;
using Data::details::CanAddBasicChatAdmins;
using Data::details::CanManageBasicChatCall;
using Data::details::CanMarkUserLoadedNormally;
using Data::details::ChatParticipantInfo;
using Data::details::ResolveChatParticipants;

auto ResolveForAccount(
		const std::vector<ChatParticipantInfo> &participants,
		UserId self,
		bool pinned,
		bool selfHasPhone = false) {
	return ResolveChatParticipants(participants, [=](UserId id)
			-> std::optional<UserId> {
		const auto isSelf = (id == self);
		if (!CanMarkUserLoadedNormally(
				isSelf,
				isSelf && selfHasPhone,
				pinned)) {
			return std::nullopt;
		}
		return id;
	});
}

struct RestrictionsChat {
	bool customServer = false;
	bool usesCustomServer() const;
};

struct RestrictionsPeer {
	const RestrictionsChat *chat = nullptr;
	const RestrictionsChat *asChat() const;
};

bool RestrictionsChat::usesCustomServer() const { return customServer; }

const RestrictionsChat *RestrictionsPeer::asChat() const { return chat; }

} // namespace

TEST_CASE(BasicGroupAdminPromotionRequiresCreatorMembership) {
	CHECK(CanAddBasicChatAdmins(true, true));
	CHECK(!CanAddBasicChatAdmins(false, true));
	CHECK(!CanAddBasicChatAdmins(true, false));
}

TEST_CASE(CustomServerBasicGroupAdminEditorIsRoleOnly) {
	const auto custom = BasicChatAdminCapabilitiesFor(true);
	CHECK(!custom.canEditRights);
	CHECK(!custom.canSetRank);
	CHECK(!custom.canTransferOwnership);

	const auto normal = BasicChatAdminCapabilitiesFor(false);
	CHECK(normal.canEditRights);
	CHECK(normal.canSetRank);
	CHECK(normal.canTransferOwnership);
}

// A custom server has no chat photos, migration, member tags or per-member
// restrictions, so none of these controls is built for its basic groups.
TEST_CASE(CustomServerBasicGroupHidesUnsupportedControls) {
	const auto custom = BasicChatControlsFor(true);
	CHECK(!custom.canEditPhoto);
	CHECK(!custom.canEditType);
	CHECK(!custom.canEditHistoryVisibility);
	CHECK(!custom.canRestrictMembers);
	CHECK(!custom.canEditMemberTags);
}

TEST_CASE(StockBasicGroupKeepsEveryControl) {
	const auto stock = BasicChatControlsFor(false);
	CHECK(stock.canEditPhoto);
	CHECK(stock.canEditType);
	CHECK(stock.canEditHistoryVisibility);
	CHECK(stock.canRestrictMembers);
	CHECK(stock.canEditMemberTags);
}

TEST_CASE(CustomBasicGroupCannotOpenMemberRestrictions) {
	const auto chat = RestrictionsChat{true};
	const auto peer = RestrictionsPeer{&chat};
	CHECK(!Data::details::SupportsMemberRestrictions(&peer));
}

TEST_CASE(StockBasicGroupCanOpenMemberRestrictions) {
	const auto chat = RestrictionsChat{false};
	const auto peer = RestrictionsPeer{&chat};
	CHECK(Data::details::SupportsMemberRestrictions(&peer));
}

TEST_CASE(ChannelMemberRestrictionsAreUnchanged) {
	const auto peer = RestrictionsPeer();
	CHECK(Data::details::SupportsMemberRestrictions(&peer));
}

TEST_CASE(CustomServerAdminSaveUsesSelectedRights) {
	constexpr auto defaultAdminRights = 0x25u;
	CHECK(BasicChatAdminRoleForSave(defaultAdminRights));
	CHECK(!BasicChatAdminRoleForSave(0u));
}

TEST_CASE(CustomServerBasicGroupAdminDefaultsKeepRoleWithoutPermissions) {
	constexpr auto defaultRights = 0x7Fu;
	constexpr auto roleOnlyRights = 0x1000u;
	const auto customRights = BasicChatAdminDefaultRightsFor(
		true,
		defaultRights,
		roleOnlyRights);
	CHECK(customRights == roleOnlyRights);
	CHECK(BasicChatAdminRoleForSave(customRights));
	CHECK(BasicChatAdminRightsFor(true, true, customRights) == 0u);
	CHECK(BasicChatAdminDefaultRightsFor(
		false,
		defaultRights,
		roleOnlyRights) == defaultRights);
}

TEST_CASE(BasicGroupAdminRightsMapOnlyForOfficialServer) {
	constexpr auto defaultRights = 0x25u;
	CHECK(BasicChatAdminRightsFor(true, false, defaultRights) == defaultRights);
	CHECK(BasicChatAdminRightsFor(false, false, defaultRights) == 0u);
	CHECK(BasicChatAdminRightsFor(true, true, defaultRights) == 0u);
	CHECK(BasicChatAdminRightsFor(false, true, defaultRights) == 0u);
}

TEST_CASE(BasicGroupCallManagementRespectsServerAdminRights) {
	CHECK(CanManageBasicChatCall(BasicChatRole::Creator, false));
	CHECK(CanManageBasicChatCall(BasicChatRole::Creator, true));
	CHECK(CanManageBasicChatCall(BasicChatRole::Admin, false));
	CHECK(!CanManageBasicChatCall(BasicChatRole::Admin, true));
	CHECK(!CanManageBasicChatCall(BasicChatRole::Member, false));
	CHECK(!CanManageBasicChatCall(BasicChatRole::Member, true));
}

TEST_CASE(PinnedPhoneFreeSelfMemberKeepsGroupParticipants) {
	const auto participants = std::vector<ChatParticipantInfo>{
		{ .userId = UserId(2), .creator = true },
		{ .userId = UserId(1), .inviterId = UserId(2) },
		{ .userId = UserId(9), .inviterId = UserId(1) },
	};
	const auto resolved = ResolveForAccount(
		participants,
		UserId(1),
		true);
	CHECK(resolved.has_value());
	CHECK(resolved->size() == 3);
	CHECK((*resolved)[0].participant.userId == UserId(2));
	CHECK((*resolved)[1].participant.userId == UserId(1));
	CHECK((*resolved)[2].participant.userId == UserId(9));
	CHECK((*resolved)[0].participant.creator);
}

TEST_CASE(PinnedPhoneFreeSelfCreatorKeepsOwner) {
	const auto participants = std::vector<ChatParticipantInfo>{
		{ .userId = UserId(1), .creator = true },
		{ .userId = UserId(2), .inviterId = UserId(1) },
		{ .userId = UserId(9), .inviterId = UserId(1) },
	};
	const auto resolved = ResolveForAccount(
		participants,
		UserId(1),
		true);
	CHECK(resolved.has_value());
	CHECK(resolved->size() == 3);
	CHECK((*resolved)[0].participant.userId == UserId(1));
	CHECK((*resolved)[0].participant.creator);
}

TEST_CASE(StockPhoneFreeSelfDoesNotLoadGroupParticipants) {
	const auto participants = std::vector<ChatParticipantInfo>{
		{ .userId = UserId(2), .creator = true },
		{ .userId = UserId(1), .inviterId = UserId(2) },
		{ .userId = UserId(9), .inviterId = UserId(1) },
	};
	CHECK(!ResolveForAccount(participants, UserId(1), false));
	CHECK(ResolveForAccount(participants, UserId(1), false, true).has_value());
}
