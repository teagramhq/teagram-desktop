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
