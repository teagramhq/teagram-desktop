/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "boxes/peers/edit_admin_rights_save.h"

namespace {

namespace AdminRights = Ui::EditPeer::AdminRights;
using Flag = ChatAdminRight;
using Flags = ChatAdminRights;

[[nodiscard]] int Bits(Flags rights) { return int(rights.value()); }

[[nodiscard]] Flags ChannelVisibleRights() {
	return Flag::ChangeInfo | Flag::PostMessages | Flag::EditMessages
		   | Flag::DeleteMessages | Flag::InviteByLinkOrAdd | Flag::PinMessages
		   | Flag::ManageCall | Flag::BanUsers;
}

[[nodiscard]] Flags NaiveCheckboxSave(Flags checked, Flags actorLimit) {
	return (checked | Flag::Other) & actorLimit;
}

[[nodiscard]] Flags UncheckPin(Flags existing) {
	return existing & ~AdminRights::StoryRights() & ~Flags(Flag::PinMessages);
}

} // namespace

TEST_CASE(UnsupportedEditKeepsAllStoryBitsWhenPinIsCleared) {
	const auto existing
		= ChannelVisibleRights() | AdminRights::StoryRights() | Flag::Other;
	const auto checked = UncheckPin(existing);
	const auto saved = AdminRights::RightsToSave({
		.checked = checked,
		.existing = existing,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved), Bits(existing & ~Flags(Flag::PinMessages)));
	CHECK(!(saved & Flag::PinMessages));
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()),
			 Bits(AdminRights::StoryRights()));
	CHECK(Bits(saved) != Bits(NaiveCheckboxSave(checked, ~Flags(0))));
}

TEST_CASE(UnsupportedEditKeepsClearedStoryBitsWhenPinIsCleared) {
	const auto existing = ChannelVisibleRights() | Flag::Other;
	const auto checked = UncheckPin(existing) | AdminRights::StoryRights();
	const auto saved = AdminRights::RightsToSave({
		.checked = checked,
		.existing = existing,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved), Bits(existing & ~Flags(Flag::PinMessages)));
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()), 0);
	CHECK(Bits(saved) != Bits(NaiveCheckboxSave(checked, ~Flags(0))));
}

TEST_CASE(UnsupportedEditKeepsMixedStoryBitsWhenPinIsCleared) {
	const auto stories = Flags(Flag::PostStories) | Flag::DeleteStories;
	const auto existing = ChannelVisibleRights() | stories | Flag::Other;
	const auto checked = UncheckPin(existing);
	const auto saved = AdminRights::RightsToSave({
		.checked = checked,
		.existing = existing,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved), Bits(existing & ~Flags(Flag::PinMessages)));
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()), Bits(stories));
	CHECK(Bits(saved) != Bits(NaiveCheckboxSave(checked, ~Flags(0))));
}

TEST_CASE(UnsupportedEditDropsStoryBitsOutsideTheActorLimit) {
	const auto existing
		= ChannelVisibleRights() | AdminRights::StoryRights() | Flag::Other;
	const auto actorLimit = ChannelVisibleRights() | Flag::Other;
	const auto saved = AdminRights::RightsToSave({
		.checked = UncheckPin(existing),
		.existing = existing,
		.actorLimit = actorLimit,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()), 0);
	CHECK_EQ(Bits(saved),
			 Bits((ChannelVisibleRights() & ~Flags(Flag::PinMessages))
				  | Flag::Other));
}

TEST_CASE(UnsupportedEditKeepsOnlyStoryBitsInsideTheActorLimit) {
	const auto existing = ChannelVisibleRights() | Flag::PostStories
						  | Flag::EditStories | Flag::Other;
	const auto actorLimit = ChannelVisibleRights() | Flag::EditStories
							| Flag::DeleteStories | Flag::Other;
	const auto saved = AdminRights::RightsToSave({
		.checked = UncheckPin(existing),
		.existing = existing,
		.actorLimit = actorLimit,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()),
			 Bits(Flags(Flag::EditStories)));
}

TEST_CASE(UnsupportedNewBotIgnoresLinkAndBotStoryDefaults) {
	const auto requested = Flags(Flag::ChangeInfo) | Flag::PinMessages
						   | AdminRights::StoryRights();
	const auto saved = AdminRights::RightsToSave({
		.checked = requested,
		.existing = Flags(),
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()), 0);
	CHECK_EQ(Bits(saved),
			 Bits(Flags(Flag::ChangeInfo) | Flag::PinMessages | Flag::Other));
}

TEST_CASE(UnsupportedExistingBotKeepsOnlyItsServerStoryBits) {
	const auto server = Flags(Flag::ChangeInfo) | Flag::PostStories
						| Flag::DeleteStories | Flag::Other;
	const auto requested = Flags(Flag::ChangeInfo) | Flag::PinMessages
						   | AdminRights::StoryRights();
	const auto actorLimit = ChannelVisibleRights() | Flag::PostStories
							| Flag::EditStories | Flag::Other;
	const auto saved = AdminRights::RightsToSave({
		.checked = requested,
		.existing = server,
		.actorLimit = actorLimit,
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()),
			 Bits(Flags(Flag::PostStories)));
	CHECK(saved & Flag::PinMessages);
}

TEST_CASE(UnsupportedNewHumanAdminSendsNoStoryBits) {
	const auto defaults = ChannelVisibleRights() | AdminRights::StoryRights();
	const auto saved = AdminRights::RightsToSave({
		.checked = defaults,
		.existing = Flags(),
		.storiesSupported = false,
	});
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()), 0);
	CHECK_EQ(Bits(saved), Bits(ChannelVisibleRights() | Flag::Other));
}

TEST_CASE(StockEditStillSavesTheCheckedStoryRows) {
	const auto existing
		= ChannelVisibleRights() | AdminRights::StoryRights() | Flag::Other;
	const auto checked
		= (existing & ~Flags(Flag::PinMessages)) & ~Flags(Flag::EditStories);
	const auto actorLimit = ~Flags(Flag::DeleteStories);
	const auto saved = AdminRights::RightsToSave({
		.checked = checked,
		.existing = existing,
		.actorLimit = actorLimit,
	});
	CHECK_EQ(Bits(saved), Bits(NaiveCheckboxSave(checked, actorLimit)));
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()),
			 Bits(Flags(Flag::PostStories)));
}

TEST_CASE(StockPromotionStillSendsTheDefaultStoryRows) {
	const auto defaults = ChannelVisibleRights() | AdminRights::StoryRights();
	const auto saved = AdminRights::RightsToSave({
		.checked = defaults,
		.existing = Flags(),
	});
	CHECK_EQ(Bits(saved), Bits(NaiveCheckboxSave(defaults, ~Flags(0))));
	CHECK_EQ(Bits(saved & AdminRights::StoryRights()),
			 Bits(AdminRights::StoryRights()));
}

TEST_CASE(UnsupportedBotLookupFailureBlocksTheEditor) {
	CHECK(!AdminRights::CanEditAfterExistingRightsFailure(u"FLOOD_WAIT_5"_q,
														  false));
	CHECK(!AdminRights::CanEditAfterExistingRightsFailure(u"CHANNEL_PRIVATE"_q,
														  false));
	CHECK(!AdminRights::CanEditAfterExistingRightsFailure(QString(), false));
}

TEST_CASE(UnsupportedBotNotInChatOpensAsNewBot) {
	CHECK(AdminRights::CanEditAfterExistingRightsFailure(
		u"USER_NOT_PARTICIPANT"_q, false));
}

TEST_CASE(StockBotLookupFailureStillOpensTheEditor) {
	CHECK(AdminRights::CanEditAfterExistingRightsFailure(u"FLOOD_WAIT_5"_q,
														 true));
}

TEST_CASE(UnsupportedBotDefaultsNeedTheBotsCurrentRights) {
	const auto defaults = Flags(Flag::ChangeInfo) | Flag::PostStories;
	CHECK(AdminRights::NeedsExistingBotRights(false, defaults, false));
	CHECK(!AdminRights::NeedsExistingBotRights(false, Flags(), false));
	CHECK(AdminRights::NeedsExistingBotRights(true, Flags(), false));
}

TEST_CASE(StockBotDefaultsKeepTheirCurrentFlow) {
	const auto defaults = Flags(Flag::ChangeInfo) | Flag::PostStories;
	CHECK(!AdminRights::NeedsExistingBotRights(false, defaults, true));
	CHECK(AdminRights::NeedsExistingBotRights(true, Flags(), true));
}
