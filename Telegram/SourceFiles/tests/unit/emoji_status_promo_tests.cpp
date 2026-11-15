/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/stickers/data_custom_emoji_policy.h"

using Data::details::EmojiStatusVisibleForPeerBadge;
using Data::details::ShouldShowEmojiStatusPremiumPromo;

TEST_CASE(CustomServerUserEmojiStatusDoesNotShowPremiumPromo) {
	CHECK(!ShouldShowEmojiStatusPremiumPromo(true, true, true, false));
}

TEST_CASE(TelegramUserEmojiStatusStillShowsPremiumPromo) {
	CHECK(ShouldShowEmojiStatusPremiumPromo(false, true, true, false));
}

TEST_CASE(ChannelEmojiStatusStillShowsPremiumPromo) {
	CHECK(ShouldShowEmojiStatusPremiumPromo(true, false, true, false));
}

TEST_CASE(PremiumBadgeWithoutStatusStillShowsPremiumPromo) {
	CHECK(ShouldShowEmojiStatusPremiumPromo(true, true, false, false));
}

TEST_CASE(CollectibleStatusKeepsExistingPremiumPromo) {
	CHECK(ShouldShowEmojiStatusPremiumPromo(true, true, true, true));
}

TEST_CASE(NonPremiumRecipientSeesCustomServerUserStatus) {
	CHECK(EmojiStatusVisibleForPeerBadge(false, false, true));
}

TEST_CASE(PeerBadgeStatusPreservesPremiumRulesElsewhere) {
	CHECK(!EmojiStatusVisibleForPeerBadge(false, false, false));
	CHECK(!EmojiStatusVisibleForPeerBadge(false, true, false));
	CHECK(!EmojiStatusVisibleForPeerBadge(true, false, false));
	CHECK(EmojiStatusVisibleForPeerBadge(true, true, false));
}
