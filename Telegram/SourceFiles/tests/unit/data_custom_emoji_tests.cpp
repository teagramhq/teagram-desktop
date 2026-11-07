/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/stickers/data_custom_emoji_policy.h"
#include "info/profile/info_profile_badge.h"

TEST_CASE(CustomEmojiPremiumGateCanBeOverriddenWithoutPremium) {
	CHECK(Data::details::CustomEmojiRequiresPremium(false, false));
	CHECK(!Data::details::CustomEmojiRequiresPremium(false, true));
	CHECK(!Data::details::CustomEmojiRequiresPremium(true, false));
}

TEST_CASE(CustomServerStatusKeepsCollectiblesPremiumOnly) {
	CHECK(!Data::details::EmojiStatusRequiresPremium(false, true, false));
	CHECK(Data::details::EmojiStatusRequiresPremium(false, true, true));
	CHECK(Data::details::EmojiStatusRequiresPremium(false, false, false));
	CHECK(!Data::details::EmojiStatusRequiresPremium(true, false, true));
}

TEST_CASE(CustomEmojiCustomServerActionsHappyPathSmoke) {
	const auto messageAction = Data::details::ResolveMessageCustomEmojiAction(
		false,
		true,
		true);
	CHECK(messageAction == Data::details::MessageCustomEmojiAction::Keep);

	const auto reactionAction = Data::details::ResolveReactActionGate({
			  .reactionsAreTags = false,
			  .canReact = true,
			  .premium = false,
			  .customReaction = true,
			  .reactionAlreadyAdded = false,
			  .broadcast = false,
			  .customServer = true,
		  });
	CHECK(reactionAction == Data::details::ReactActionGate::Allowed);

	CHECK(Data::details::ResolveEmojiStatusPickerAction(
			  false,
			  true,
			  false,
			  false)
		  == Data::details::EmojiStatusPickerAction::OpenPicker);
	CHECK(Data::details::EmojiStatusEntryVisible(false, true));
	const auto chosenStatus = EmojiStatusId{ .documentId = 42 };
	const auto statusBadge = Info::Profile::details::ResolveBadgeContentForPeer(
		Info::Profile::BadgeType::None,
		chosenStatus,
		true,
		true);
	CHECK(statusBadge.badge == Info::Profile::BadgeType::EmojiStatus);
	CHECK(statusBadge.emojiStatusId == chosenStatus);
	const auto renderedStatus = Info::Profile::details::FilterBadgeContent(
		statusBadge,
		true,
		false);
	CHECK(renderedStatus == statusBadge);
	const auto stockStatus = Info::Profile::details::ResolveBadgeContentForPeer(
		Info::Profile::BadgeType::None,
		chosenStatus,
		true,
		false);
	CHECK(stockStatus.badge == Info::Profile::BadgeType::None);
	CHECK(!stockStatus.emojiStatusId);
}

TEST_CASE(CustomEmojiActionPremiumGatesStayScoped) {
	CHECK(
		Data::details::ResolveMessageCustomEmojiAction(true, false, false)
		== Data::details::MessageCustomEmojiAction::PremiumGate);
	CHECK(
		Data::details::ResolveMessageCustomEmojiAction(false, true, false)
		== Data::details::MessageCustomEmojiAction::PremiumGate);
	CHECK(Data::details::ResolveReactActionGate({
			  .reactionsAreTags = false,
			  .canReact = true,
			  .premium = false,
			  .customReaction = true,
			  .reactionAlreadyAdded = false,
			  .broadcast = false,
			  .customServer = false,
		  })
		  == Data::details::ReactActionGate::CustomReactionPremium);
	CHECK(Data::details::ResolveReactActionGate({
			  .reactionsAreTags = true,
			  .canReact = true,
			  .premium = false,
			  .customReaction = true,
			  .reactionAlreadyAdded = false,
			  .broadcast = false,
			  .customServer = true,
		  })
		  == Data::details::ReactActionGate::TagsPremium);
	CHECK(Data::details::ResolveReactActionGate({
			  .reactionsAreTags = false,
			  .canReact = false,
			  .premium = false,
			  .customReaction = true,
			  .reactionAlreadyAdded = false,
			  .broadcast = false,
			  .customServer = true,
		  })
		  == Data::details::ReactActionGate::Restricted);
	CHECK(Data::details::EmojiStatusRequiresPremium(false, true, true));
	CHECK(!Data::details::EmojiStatusRequiresPremium(true, false, true));
	CHECK(Data::details::ResolveEmojiStatusPickerAction(
			  false,
			  false,
			  false,
			  false)
		  == Data::details::EmojiStatusPickerAction::NoAction);
	CHECK(!Data::details::EmojiStatusEntryVisible(false, false));
	CHECK(Data::details::ResolveEmojiStatusPickerAction(
			  true,
			  false,
			  false,
			  false)
		  == Data::details::EmojiStatusPickerAction::PremiumPreview);
	CHECK(Data::details::ResolveEmojiStatusPickerAction(
			  false,
			  true,
			  true,
			  false)
		  == Data::details::EmojiStatusPickerAction::NoAction);

	const auto hiddenPremiumBadge = Info::Profile::details::FilterBadgeContent(
		{ Info::Profile::BadgeType::Premium },
		true,
		false);
	CHECK(hiddenPremiumBadge.badge == Info::Profile::BadgeType::None);
}
