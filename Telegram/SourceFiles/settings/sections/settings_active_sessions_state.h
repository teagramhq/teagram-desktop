/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <utility>

namespace Settings::details {

class ActiveSessionsState final {
  public:
	static constexpr auto kFailedReloadRetryDelayMs = 5'000;

	struct ListTransition {
		bool clearRows = false;
		bool schedulePoll = false;
	};
	struct RefreshTransition {
		bool schedulePoll = false;
		int retryDelayMs = 0;
	};

	void initialize(bool hasAuthoritativeList, bool hasRows) {
		_hasAuthoritativeList = hasAuthoritativeList;
		_hasRows = hasRows;
		_loading = !hasAuthoritativeList && !hasRows;
	}

	void beginRefresh() { _loading = !_hasAuthoritativeList && !_hasRows; }

	void beginTerminateAll() { _loading = true; }

	[[nodiscard]] RefreshTransition refreshFailed() {
		_loading = false;
		return {
			.schedulePoll = true,
			.retryDelayMs = kFailedReloadRetryDelayMs,
		};
	}

	[[nodiscard]] ListTransition receiveList(bool hasRows,
											 bool hasAuthoritativeList) {
		_hasRows = hasRows;
		_hasAuthoritativeList = hasAuthoritativeList;
		if (_hasRows || _hasAuthoritativeList) {
			_loading = false;
		}
		return {.clearRows = !hasRows, .schedulePoll = true};
	}

	[[nodiscard]] bool loading() const { return _loading; }

	[[nodiscard]] bool hasRows() const { return _hasRows; }

	[[nodiscard]] bool rowsVisible() const { return _hasRows && !_loading; }

  private:
	bool _hasAuthoritativeList = false;
	bool _hasRows = false;
	bool _loading = false;
};

template <typename SchedulePoll>
void HandleReloadFailure(ActiveSessionsState &state,
						 SchedulePoll &&schedulePoll) {
	const auto transition = state.refreshFailed();
	if (transition.schedulePoll) {
		std::forward<SchedulePoll>(schedulePoll)(transition.retryDelayMs);
	}
}

} // namespace Settings::details
