/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace Main::details {

struct SignupInviteSlot {
	bool hasSession = false;
	bool sameEnvironment = false;
	bool defaultServer = false;
	bool blocked = false;
};

[[nodiscard]] constexpr bool
CanReceiveDefaultServerSignupInvite(
	bool hasCustomServer,
	bool defaultServer,
	bool blocked) {
	return !blocked && (!hasCustomServer || defaultServer);
}

[[nodiscard]] inline std::optional<std::size_t>
SelectSignupInviteSlot(const std::vector<SignupInviteSlot> &slots) {
	for (auto i = std::size_t(0); i != slots.size(); ++i) {
		const auto &slot = slots[i];
		if (!slot.hasSession && slot.sameEnvironment
			&& slot.defaultServer && !slot.blocked) {
			return i;
		}
	}
	return std::nullopt;
}

} // namespace Main::details
