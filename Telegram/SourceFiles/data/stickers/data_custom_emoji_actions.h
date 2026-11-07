/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/stickers/data_custom_emoji_policy.h"

#include <utility>

namespace Data::details {

template <typename Keep>
[[nodiscard]] bool
RunMessageCustomEmojiAction(bool premiumPossible, bool customServer,
							bool peerAllowsWithoutPremium, Keep &&keep) {
	if (ResolveMessageCustomEmojiAction(premiumPossible, customServer,
										peerAllowsWithoutPremium)
		== MessageCustomEmojiAction::PremiumGate) {
		return false;
	}
	std::forward<Keep>(keep)();
	return true;
}

template <typename Allowed, typename Denied>
[[nodiscard]] bool RunReactAction(ReactActionGateInput input, Allowed &&allowed,
								  Denied &&denied) {
	const auto gate = ResolveReactActionGate(input);
	if (gate == ReactActionGate::Allowed) {
		std::forward<Allowed>(allowed)();
		return true;
	}
	std::forward<Denied>(denied)(gate);
	return false;
}

template <typename OpenPicker, typename PremiumPreview>
void RunEmojiStatusPickerAction(bool paidFeaturesSupported, bool customServer,
								bool frozen, bool badgeVisible,
								OpenPicker &&openPicker,
								PremiumPreview &&premiumPreview) {
	const auto action = ResolveEmojiStatusPickerAction(
		paidFeaturesSupported, customServer, frozen, badgeVisible);
	switch (action) {
	case EmojiStatusPickerAction::NoAction:
		break;
	case EmojiStatusPickerAction::OpenPicker:
		std::forward<OpenPicker>(openPicker)();
		break;
	case EmojiStatusPickerAction::PremiumPreview:
		std::forward<PremiumPreview>(premiumPreview)();
		break;
	}
}

template <typename Set, typename PremiumGate>
[[nodiscard]] bool ApplyEmojiStatus(bool hasStatus, bool premium,
									bool customServer, bool collectible,
									Set &&set, PremiumGate &&premiumGate) {
	if (hasStatus
		&& EmojiStatusRequiresPremium(premium, customServer, collectible)) {
		std::forward<PremiumGate>(premiumGate)();
		return false;
	}
	std::forward<Set>(set)();
	return true;
}

} // namespace Data::details
