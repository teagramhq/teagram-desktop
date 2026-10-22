/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Settings::details {

struct EmptySessionsResponseTransition {
	bool updateLoading;
	bool loading;
	bool schedulePoll;
};

[[nodiscard]] constexpr bool ShouldShowSessionsLoading(bool hasReceivedResponse,
													   bool hasCachedSessions) {
	return !hasReceivedResponse && !hasCachedSessions;
}

[[nodiscard]] constexpr EmptySessionsResponseTransition
HandleEmptySessionsResponse(bool hasReceivedResponse, bool loading) {
	if (!hasReceivedResponse) {
		return {false, loading, false};
	}
	return {true, false, true};
}

} // namespace Settings::details
