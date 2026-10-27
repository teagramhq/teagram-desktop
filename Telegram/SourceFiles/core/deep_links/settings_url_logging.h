/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

namespace Core::DeepLinks {

// Keep only the route in local logs. Query and fragment values come from the
// clicked message and are not needed to identify the Settings route.
[[nodiscard]] inline QString SettingsUrlForLogging(const QString &url) {
	if (!url.startsWith(u"tg://settings/"_q, Qt::CaseInsensitive)) {
		return QString();
	}
	auto end = url.size();
	const auto queryStart = url.indexOf(QChar(u'?'));
	if (queryStart >= 0) {
		end = queryStart;
	}
	const auto fragmentStart = url.indexOf(QChar(u'#'));
	if (fragmentStart >= 0 && fragmentStart < end) {
		end = fragmentStart;
	}
	return url.left(end);
}

} // namespace Core::DeepLinks
