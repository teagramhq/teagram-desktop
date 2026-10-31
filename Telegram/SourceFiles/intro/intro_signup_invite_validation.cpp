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

} // namespace Intro::details
