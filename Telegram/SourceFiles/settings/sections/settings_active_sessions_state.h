/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <utility>

#include <QtCore/QString>

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

	void initialize(bool hasAuthoritativeList, bool hasRows,
					const QString &failureType = QString()) {
		_hasAuthoritativeList = hasAuthoritativeList;
		_hasRows = hasRows;
		_failureType = failureType;
		_failed = !_failureType.isEmpty();
		_loading = !_failed && !hasAuthoritativeList && !hasRows;
	}

	void beginRefresh() {
		if (!_failed) {
			_loading = !_hasAuthoritativeList && !_hasRows;
		}
	}

	void beginTerminateAll() {
		_failed = false;
		_failureType = QString();
		_loading = true;
	}

	void reloadSucceeded() {
		_failed = false;
		_failureType = QString();
	}

	[[nodiscard]] RefreshTransition refreshFailed(const QString &errorType) {
		_failed = true;
		_failureType = errorType;
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

	[[nodiscard]] bool failed() const { return _failed; }

	[[nodiscard]] const QString &failureType() const { return _failureType; }

	[[nodiscard]] bool hasRows() const { return _hasRows; }

	[[nodiscard]] bool rowsVisible() const { return _hasRows && !_loading; }

	[[nodiscard]] bool contentVisible() const {
		return !_loading && (_hasAuthoritativeList || _hasRows);
	}

	[[nodiscard]] bool shouldUseCachedList(bool hasAuthoritativeList,
										   bool cacheIsFresh) const {
		return hasAuthoritativeList && cacheIsFresh && !_failed;
	}

  private:
	bool _hasAuthoritativeList = false;
	bool _hasRows = false;
	bool _loading = false;
	bool _failed = false;
	QString _failureType;
};

template <typename Error, typename SchedulePoll>
void HandleReloadFailure(ActiveSessionsState &state, const Error &error,
						 SchedulePoll &&schedulePoll) {
	const auto transition = state.refreshFailed(error.type());
	if (transition.schedulePoll) {
		std::forward<SchedulePoll>(schedulePoll)(transition.retryDelayMs);
	}
}

} // namespace Settings::details
