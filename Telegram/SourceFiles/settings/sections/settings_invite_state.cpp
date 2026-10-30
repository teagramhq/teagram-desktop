/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_invite_state.h"

#include <algorithm>
#include <utility>

namespace Settings::details {

bool InviteCreateRequest::begin(bool allowed) {
	if (!allowed || _active) {
		return false;
	}
	_active = true;
	return true;
}

bool InviteCreateRequest::finish() {
	return std::exchange(_active, false);
}

bool InviteCreateRequest::active() const {
	return _active;
}

bool CanCreateInvite(const InviteCreateConditions &conditions) {
	return conditions.available
		&& conditions.loaded
		&& !conditions.listLoading
		&& !conditions.creating
		&& !conditions.serverDisabled
		&& !conditions.createBlocked
		&& conditions.quota > conditions.weeklyCount;
}

int64 InviteQuotaLeft(int64 quota, int64 weeklyCount) {
	return std::max<int64>(0, quota - weeklyCount);
}

std::optional<TimeId> InviteNextAvailableAtAfterCreate(
		int64 quota,
		int64 weeklyCount,
		std::optional<TimeId> nextAvailableAt,
		TimeId createdAt) {
	if (quota > 0 && weeklyCount < quota && weeklyCount + 1 >= quota
		&& !nextAvailableAt) {
		return createdAt + 7 * 86400;
	}
	return nextAvailableAt;
}

InviteListFailure ClassifyInviteListFailure(const QString &type) {
	if (type == u"USER_INVITES_DISABLED"_q) {
		return InviteListFailure::Disabled;
	}
	return InviteListFailure::Retry;
}

InviteCreateFailure ClassifyInviteCreateFailure(
		const QString &type,
		bool registrationOpen) {
	const auto waitPrefix = u"USER_INVITE_ACCOUNT_TOO_NEW_WAIT_"_q;
	if (type.startsWith(waitPrefix)) {
		bool ok = false;
		const auto seconds = type.mid(waitPrefix.size()).toInt(&ok);
		return {
			.kind = InviteCreateFailureKind::AccountTooNew,
			.waitSeconds = (ok && seconds > 0)
				? std::optional<int>(seconds)
				: std::nullopt,
		};
	} else if (type == u"USER_INVITE_REGISTRATION_CLOSED"_q) {
		return {
			.kind = InviteCreateFailureKind::RegistrationClosed,
			.registrationOpen = registrationOpen,
		};
	} else if (type == u"USER_INVITES_DISABLED"_q) {
		return { .kind = InviteCreateFailureKind::InvitesDisabled };
	} else if (type == u"USER_INVITE_WEEKLY_LIMIT_REACHED"_q) {
		return { .kind = InviteCreateFailureKind::WeeklyLimitReached };
	}
	return { .kind = InviteCreateFailureKind::Other };
}

bool CanRevokeInvite(const QString &state, bool inFlight) {
	return (state == u"pending"_q) && !inFlight;
}

} // namespace Settings::details
