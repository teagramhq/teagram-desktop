/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data::details {

[[nodiscard]] constexpr bool
CustomEmojiRequiresPremium(bool premiumPossible, bool allowWithoutPremium) {
	return !premiumPossible && !allowWithoutPremium;
}

[[nodiscard]] constexpr bool
MessageCustomEmojiRequiresPremium(bool premiumPossible, bool customServer,
								  bool peerAllowsWithoutPremium) {
	return CustomEmojiRequiresPremium(premiumPossible, customServer)
		   || !peerAllowsWithoutPremium;
}

enum class ReactActionGate {
	Allowed,
	Restricted,
	TagsPremium,
	CustomReactionPremium,
};

struct ReactActionGateInput {
	bool reactionsAreTags = false;
	bool canReact = false;
	bool premium = false;
	bool customReaction = false;
	bool reactionAlreadyAdded = false;
	bool broadcast = false;
	bool customServer = false;
};

[[nodiscard]] constexpr ReactActionGate
ResolveReactActionGate(ReactActionGateInput input) {
	if (input.reactionsAreTags) {
		return input.premium ? ReactActionGate::Allowed
							 : ReactActionGate::TagsPremium;
	} else if (!input.canReact) {
		return ReactActionGate::Restricted;
	} else if (input.premium || input.reactionAlreadyAdded || input.broadcast
			   || !input.customReaction || input.customServer) {
		return ReactActionGate::Allowed;
	}
	return ReactActionGate::CustomReactionPremium;
}

[[nodiscard]] constexpr bool
EmojiStatusRequiresPremium(bool premium, bool customServer, bool collectible) {
	return CustomEmojiRequiresPremium(premium, customServer && !collectible);
}

} // namespace Data::details
