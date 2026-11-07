/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Intro::details {

enum class InviteSignInFailure {
	Unusable,
	UsernameCodeExpired,
	RegistrationClosed,
	FloodWait,
	Other,
};

[[nodiscard]] QString NormalizeInviteSecret(const QString &input);
[[nodiscard]] InviteSignInFailure
ClassifyInviteSignInFailure(const QString &type, bool floodError);

} // namespace Intro::details
