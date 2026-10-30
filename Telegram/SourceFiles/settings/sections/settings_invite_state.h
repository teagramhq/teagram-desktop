/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

#include <optional>

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

class InviteCreateRequest final {
  public:
	[[nodiscard]] bool begin(bool allowed);
	[[nodiscard]] bool finish();
	[[nodiscard]] bool active() const;

  private:
	bool _active = false;
};

[[nodiscard]] bool CanCreateInvite(const InviteCreateConditions &conditions);
[[nodiscard]] int64 InviteQuotaLeft(int64 quota, int64 weeklyCount);
[[nodiscard]] std::optional<TimeId>
InviteNextAvailableAtAfterCreate(int64 quota, int64 weeklyCount,
								 std::optional<TimeId> nextAvailableAt,
								 TimeId createdAt);
[[nodiscard]] InviteListFailure ClassifyInviteListFailure(const QString &type);
[[nodiscard]] InviteCreateFailure
ClassifyInviteCreateFailure(const QString &type, bool registrationOpen);
[[nodiscard]] bool CanRevokeInvite(const QString &state, bool inFlight);

} // namespace Settings::details
