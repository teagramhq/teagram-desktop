/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/stickers/data_custom_emoji_actions.h"
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
	const auto customEmojiTag = u"custom-emoji:42"_q;
	auto outgoingTags = QString();
	const auto inserted = Data::details::RunMessageCustomEmojiAction(
		false, true, true, [&] { outgoingTags = customEmojiTag; });
	CHECK(inserted);
	CHECK_EQ(outgoingTags, customEmojiTag);

	auto reactionToggled = false;
	auto reactionDenied = false;
	const auto reacted = Data::details::RunReactAction(
		{
			.reactionsAreTags = false,
			.canReact = true,
			.premium = false,
			.customReaction = true,
			.reactionAlreadyAdded = false,
			.broadcast = false,
			.customServer = true,
		},
		[&] { reactionToggled = true; },
		[&](Data::details::ReactActionGate) { reactionDenied = true; });
	CHECK(reacted);
	CHECK(reactionToggled);
	CHECK(!reactionDenied);

	auto pickerOpened = false;
	auto premiumPreviewShown = false;
	Data::details::RunEmojiStatusPickerAction(
		false, true, false, false, [&] { pickerOpened = true; },
		[&] { premiumPreviewShown = true; });
	CHECK(pickerOpened);
	CHECK(!premiumPreviewShown);
	CHECK(Data::details::EmojiStatusEntryVisible(false, true));

	const auto chosenStatus = EmojiStatusId{.documentId = 42};
	auto storedStatus = EmojiStatusId();
	const auto statusSet = Data::details::ApplyEmojiStatus(
		true, false, true, false, [&] { storedStatus = chosenStatus; },
		[&] { premiumPreviewShown = true; });
	CHECK(statusSet);
	CHECK(storedStatus == chosenStatus);
	CHECK(!premiumPreviewShown);

	const auto recipientContent
		= Info::Profile::details::ResolveBadgeContentForPeer(
			Info::Profile::BadgeType::None, storedStatus, true, true);
	auto displayedContent = Info::Profile::Badge::Content();
	auto displayed = false;
	const auto visibleContent = Info::Profile::details::PresentBadgeContent(
		recipientContent, true, false,
		[&](Info::Profile::Badge::Content content) {
			displayed = true;
			displayedContent = content;
		});
	CHECK(displayed);
	CHECK(visibleContent.badge == Info::Profile::BadgeType::EmojiStatus);
	CHECK(displayedContent.badge == Info::Profile::BadgeType::EmojiStatus);
	CHECK(displayedContent.emojiStatusId == chosenStatus);
	const auto stockStatus = Info::Profile::details::ResolveBadgeContentForPeer(
		Info::Profile::BadgeType::None, chosenStatus, true, false);
	CHECK(stockStatus.badge == Info::Profile::BadgeType::None);
	CHECK(!stockStatus.emojiStatusId);
}

TEST_CASE(CustomEmojiActionPremiumGatesStayScoped) {
	auto messageTagKept = false;
	CHECK(!Data::details::RunMessageCustomEmojiAction(
		false, false, false, [&] { messageTagKept = true; }));
	CHECK(!messageTagKept);
	CHECK(Data::details::ResolveMessageCustomEmojiAction(true, false, false)
		  == Data::details::MessageCustomEmojiAction::PremiumGate);
	CHECK(Data::details::ResolveMessageCustomEmojiAction(false, true, false)
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
	auto reactionToggled = false;
	auto tagsGate = Data::details::ReactActionGate::Allowed;
	CHECK(!Data::details::RunReactAction(
		{
			.reactionsAreTags = true,
			.canReact = true,
			.premium = false,
			.customReaction = true,
			.reactionAlreadyAdded = false,
			.broadcast = false,
			.customServer = true,
		},
		[&] { reactionToggled = true; }, [&](auto gate) { tagsGate = gate; }));
	CHECK(!reactionToggled);
	CHECK(tagsGate == Data::details::ReactActionGate::TagsPremium);
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
	auto collectibleStatusSet = false;
	CHECK(!Data::details::ApplyEmojiStatus(
		true, false, true, true, [&] { collectibleStatusSet = true; }, [] {}));
	CHECK(!collectibleStatusSet);
	auto pickerOpened = false;
	auto premiumPreviewShown = false;
	Data::details::RunEmojiStatusPickerAction(
		true, false, false, false, [&] { pickerOpened = true; },
		[&] { premiumPreviewShown = true; });
	CHECK(!pickerOpened);
	CHECK(premiumPreviewShown);
	CHECK(Data::details::ResolveEmojiStatusPickerAction(false, false, false,
														false)
		  == Data::details::EmojiStatusPickerAction::NoAction);
	CHECK(!Data::details::EmojiStatusEntryVisible(false, false));
	CHECK(
		Data::details::ResolveEmojiStatusPickerAction(true, false, false, false)
		== Data::details::EmojiStatusPickerAction::PremiumPreview);
	CHECK(
		Data::details::ResolveEmojiStatusPickerAction(false, true, true, false)
		== Data::details::EmojiStatusPickerAction::NoAction);

	auto premiumBadgeDisplayed = false;
	const auto hiddenPremiumBadge = Info::Profile::details::PresentBadgeContent(
		{Info::Profile::BadgeType::Premium}, true, false,
		[&](auto) { premiumBadgeDisplayed = true; });
	CHECK(hiddenPremiumBadge.badge == Info::Profile::BadgeType::None);
	CHECK(!premiumBadgeDisplayed);
}
