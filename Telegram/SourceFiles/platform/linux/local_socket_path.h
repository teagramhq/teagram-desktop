/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QString>

#include <sys/stat.h>

#include <sys/types.h>

namespace Platform::Linux {

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &runtimeDirectory,
						 const QString &serverName, uid_t effectiveUid) {
	if (runtimeDirectory.isEmpty() || !QDir::isAbsolutePath(runtimeDirectory)
		|| serverName.isEmpty() || serverName.contains('/')
		|| serverName == u"."_q || serverName == u".."_q) {
		return QString();
	}
	const auto canonicalDirectory
		= QFileInfo(runtimeDirectory).canonicalFilePath();
	if (canonicalDirectory.isEmpty()) {
		return QString();
	}
	const auto encodedDirectory = QFile::encodeName(canonicalDirectory);
	struct stat status = {};
	if ((lstat(encodedDirectory.constData(), &status) != 0)
		|| !S_ISDIR(status.st_mode) || (status.st_uid != effectiveUid)
		|| ((status.st_mode & 07777) != 0700)) {
		return QString();
	}
	return QDir(canonicalDirectory).filePath(serverName);
}

} // namespace Platform::Linux
