/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Settings::details {

class ActiveSessionsState final {
  public:
	struct ListTransition {
		bool clearRows = false;
		bool schedulePoll = false;
	};

	void initialize(bool hasAuthoritativeList, bool hasRows) {
		_hasAuthoritativeList = hasAuthoritativeList;
		_hasRows = hasRows;
		_loading = !hasAuthoritativeList && !hasRows;
	}

	void beginRefresh() { _loading = !_hasAuthoritativeList && !_hasRows; }

	void beginTerminateAll() { _loading = true; }

	void refreshFailed() { _loading = !_hasAuthoritativeList && !_hasRows; }

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

} // namespace Settings::details
