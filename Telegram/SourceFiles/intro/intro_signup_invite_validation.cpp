/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_signup_invite_validation.h"

namespace Intro::details {

QString NormalizeInviteSecret(const QString &input) {
	auto result = input.trimmed();
	const auto fragment = result.lastIndexOf(QChar(u'#'));
	if (fragment >= 0) {
		result = result.mid(fragment + 1).trimmed();
	}
	result = result.toLower();
	if (result.size() != 64) {
		return QString();
	}
	for (const auto ch : result) {
		if ((ch < QChar(u'0') || ch > QChar(u'9'))
			&& (ch < QChar(u'a') || ch > QChar(u'f'))) {
			return QString();
		}
	}
	return result;
}

InviteSignInFailure ClassifyInviteSignInFailure(const QString &type,
												bool floodError) {
	if (floodError) {
		return InviteSignInFailure::FloodWait;
	} else if (type == u"INVITE_HASH_INVALID"_q) {
		return InviteSignInFailure::Unusable;
	} else if (type == u"PHONE_CODE_INVALID"_q
			   || type == u"PHONE_CODE_EXPIRED"_q) {
		return InviteSignInFailure::UsernameCodeExpired;
	} else if (type == u"INPUT_REQUEST_INVALID"_q) {
		return InviteSignInFailure::RegistrationClosed;
	}
	return InviteSignInFailure::Other;
}

} // namespace Intro::details
