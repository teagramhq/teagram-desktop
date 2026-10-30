/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "mtproto/core_types.h"

#include <QtCore/QString>

#include <optional>
#include <utility>

namespace Settings::details {

struct InviteCreateConditions {
	bool available = false;
	bool loaded = false;
	bool listLoading = false;
	bool creating = false;
	bool serverDisabled = false;
	bool createBlocked = false;
	int64 quota = 0;
	int64 weeklyCount = 0;
};

enum class InviteListFailure {
	Retry,
	Disabled,
};

enum class InviteCreateFailureKind {
	AccountTooNew,
	RegistrationClosed,
	InvitesDisabled,
	WeeklyLimitReached,
	Other,
};

struct InviteCreateFailure {
	InviteCreateFailureKind kind = InviteCreateFailureKind::Other;
	std::optional<int> waitSeconds;
	bool registrationOpen = false;
};

[[nodiscard]] bool CanCreateInvite(const InviteCreateConditions &conditions);
[[nodiscard]] bool CanRevokeInvite(const QString &state, bool inFlight);

class InviteCreateRequest final {
  public:
	[[nodiscard]] bool begin(bool allowed);
	[[nodiscard]] bool finish();
	[[nodiscard]] bool active() const;

  private:
	bool _active = false;
};

template <typename Started, typename Send, typename Done, typename Fail>
[[nodiscard]] std::optional<mtpRequestId>
RunInviteListRequest(bool available, mtpRequestId activeRequest,
					 Started &&started, Send &&send, Done &&done, Fail &&fail) {
	if (!available || activeRequest) {
		return std::nullopt;
	}
	std::forward<Started>(started)();
	return std::forward<Send>(send)(std::forward<Done>(done),
									std::forward<Fail>(fail));
}

template <typename Started, typename Send, typename Done, typename Fail>
[[nodiscard]] std::optional<mtpRequestId> RunInviteCreateRequest(
	InviteCreateRequest &request, const InviteCreateConditions &conditions,
	Started &&started, Send &&send, Done &&done, Fail &&fail) {
	if (!request.begin(CanCreateInvite(conditions))) {
		return std::nullopt;
	}
	std::forward<Started>(started)();
	auto succeeded = [state = &request, callback = std::forward<Done>(done)](
						 auto &&result) mutable {
		if (state->finish()) {
			callback(std::forward<decltype(result)>(result));
		}
	};
	auto failed = [state = &request,
				   callback = std::forward<Fail>(fail)](auto &&error) mutable {
		if (state->finish()) {
			callback(std::forward<decltype(error)>(error));
		}
	};
	return std::forward<Send>(send)(std::move(succeeded), std::move(failed));
}

template <typename Started, typename Send, typename Done, typename Fail>
[[nodiscard]] std::optional<mtpRequestId>
RunInviteRevokeRequest(const QString &state, bool inFlight, Started &&started,
					   Send &&send, Done &&done, Fail &&fail) {
	if (!CanRevokeInvite(state, inFlight)) {
		return std::nullopt;
	}
	std::forward<Started>(started)();
	return std::forward<Send>(send)(std::forward<Done>(done),
									std::forward<Fail>(fail));
}

[[nodiscard]] int64 InviteQuotaLeft(int64 quota, int64 weeklyCount);
[[nodiscard]] std::optional<TimeId>
InviteNextAvailableAtAfterCreate(int64 quota, int64 weeklyCount,
								 std::optional<TimeId> nextAvailableAt,
								 TimeId createdAt);
[[nodiscard]] InviteListFailure ClassifyInviteListFailure(const QString &type);
[[nodiscard]] InviteCreateFailure
ClassifyInviteCreateFailure(const QString &type, bool registrationOpen);
} // namespace Settings::details
