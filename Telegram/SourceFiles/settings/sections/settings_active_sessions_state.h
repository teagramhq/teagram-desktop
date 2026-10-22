/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Settings::details {

[[nodiscard]] constexpr bool ShouldShowSessionsLoading(
		bool hasReceivedResponse,
		bool hasCachedSessions) {
	return !hasReceivedResponse && !hasCachedSessions;
}

} // namespace Settings::details
