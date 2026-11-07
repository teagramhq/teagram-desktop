/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data::details {

[[nodiscard]] constexpr bool CustomEmojiRequiresPremium(
		bool premiumPossible,
		bool allowWithoutPremium) {
	return !premiumPossible && !allowWithoutPremium;
}

[[nodiscard]] constexpr bool EmojiStatusRequiresPremium(
		bool premium,
		bool customServer,
		bool collectible) {
	return CustomEmojiRequiresPremium(premium, customServer && !collectible);
}

} // namespace Data::details
