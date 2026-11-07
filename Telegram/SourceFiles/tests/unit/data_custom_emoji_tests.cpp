/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/stickers/data_custom_emoji_policy.h"

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
	CHECK(!Data::details::MessageCustomEmojiRequiresPremium(false, true, true));
	CHECK(Data::details::ResolveReactActionGate({
			  .reactionsAreTags = false,
			  .canReact = true,
			  .premium = false,
			  .customReaction = true,
			  .reactionAlreadyAdded = false,
			  .broadcast = false,
			  .customServer = true,
		  })
		  == Data::details::ReactActionGate::Allowed);
	CHECK(!Data::details::EmojiStatusRequiresPremium(false, true, false));
}

TEST_CASE(CustomEmojiActionPremiumGatesStayScoped) {
	CHECK(Data::details::MessageCustomEmojiRequiresPremium(true, false, false));
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
}
