/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/specific_linux.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>

QString Platform::LinuxAppDataPath(
	const QString &home,
	const QString &standardPath) {
#ifndef TDESKTOP_TEAGRAM
	if (!home.isEmpty()) {
		auto oldPath = home + u"/.TelegramDesktop"_q;
		auto oldSettingsBase = oldPath + u"/tdata/settings"_q;
		if (QFile::exists(oldSettingsBase + '0')
			|| QFile::exists(oldSettingsBase + '1')
			|| QFile::exists(oldSettingsBase + 's')) {
			return oldPath + '/';
		}
	}
#endif // !TDESKTOP_TEAGRAM

	return standardPath + '/';
}

QString psAppDataPath() {
	return Platform::LinuxAppDataPath(
		QDir::homePath(),
		QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
}
