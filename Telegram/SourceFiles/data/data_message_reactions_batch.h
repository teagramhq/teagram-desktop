/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data::details {

template <typename Items, typename Callback>
[[nodiscard]] bool ProcessAvailableReactionsBatch(const Items &items,
												  int &position,
												  Callback &&callback) {
	const auto count = int(items.size());
	if (position >= count) {
		return false;
	}
	callback(items[position++]);
	return position < count;
}

} // namespace Data::details
