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

#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>

namespace Platform::Linux {
namespace SingleInstanceSocketPathDetails {

[[nodiscard]] inline QString
CanonicalExistingDirectory(const QString &path, struct stat *status,
						   bool rejectFinalSymlink) {
	if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
		return QString();
	}
	const auto cleanPath = QDir::cleanPath(path);
	struct stat pathStatus = {};
	if (rejectFinalSymlink) {
		const auto encodedPath = QFile::encodeName(cleanPath);
		if ((lstat(encodedPath.constData(), &pathStatus) != 0)
			|| !S_ISDIR(pathStatus.st_mode)) {
			return QString();
		}
	}
	const auto canonicalPath = QFileInfo(cleanPath).canonicalFilePath();
	if (canonicalPath.isEmpty()) {
		return QString();
	}
	const auto encodedCanonicalPath = QFile::encodeName(canonicalPath);
	struct stat canonicalStatus = {};
	if ((lstat(encodedCanonicalPath.constData(), &canonicalStatus) != 0)
		|| !S_ISDIR(canonicalStatus.st_mode)
		|| (rejectFinalSymlink
			&& ((pathStatus.st_dev != canonicalStatus.st_dev)
				|| (pathStatus.st_ino != canonicalStatus.st_ino)))) {
		return QString();
	}
	*status = canonicalStatus;
	return canonicalPath;
}

[[nodiscard]] inline QString
VerifiedPrivateRuntimeDirectory(const QString &path, uid_t effectiveUid) {
	struct stat status = {};
	const auto canonicalPath = CanonicalExistingDirectory(path, &status, true);
	if (canonicalPath.isEmpty() || (status.st_uid != effectiveUid)
		|| ((status.st_mode & 07777) != 0700)) {
		return QString();
	}
	return canonicalPath;
}

[[nodiscard]] inline QString TrustedTemporaryDirectory(const QString &path,
													   uid_t effectiveUid) {
	struct stat status = {};
	const auto canonicalPath = CanonicalExistingDirectory(path, &status, false);
	if (canonicalPath.isEmpty()) {
		return QString();
	}
	const auto writableByOthers = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
	const auto ownedPrivate
		= (status.st_uid == effectiveUid) && !writableByOthers;
	const auto rootManaged
		= (status.st_uid == 0)
		  && (!writableByOthers || (status.st_mode & S_ISVTX));
	return (ownedPrivate || rootManaged) ? canonicalPath : QString();
}

[[nodiscard]] inline bool IsValidServerName(const QString &serverName) {
	return !serverName.isEmpty() && !serverName.contains('/')
		   && (serverName != u"."_q) && (serverName != u".."_q);
}

[[nodiscard]] inline QString
CreateFallbackRuntimeDirectory(const QString &temporaryDirectory,
							   uid_t effectiveUid) {
	const auto canonicalTemporaryDirectory
		= TrustedTemporaryDirectory(temporaryDirectory, effectiveUid);
	if (canonicalTemporaryDirectory.isEmpty()) {
		return QString();
	}
	const auto runtimeName = u"teagram-runtime-%1"_q.arg(
		QString::number(static_cast<qulonglong>(effectiveUid)));
	const auto runtimeDirectory
		= QDir(canonicalTemporaryDirectory).filePath(runtimeName);
	const auto encodedRuntimeDirectory = QFile::encodeName(runtimeDirectory);
	const auto created
		= (mkdir(encodedRuntimeDirectory.constData(), 0700) == 0);
	if (!created && (errno != EEXIST)) {
		return QString();
	}
	if (created) {
		struct stat status = {};
		if ((lstat(encodedRuntimeDirectory.constData(), &status) != 0)
			|| !S_ISDIR(status.st_mode) || (status.st_uid != effectiveUid)
			|| (chmod(encodedRuntimeDirectory.constData(), 0700) != 0)) {
			return QString();
		}
	}
	return VerifiedPrivateRuntimeDirectory(runtimeDirectory, effectiveUid);
}

} // namespace SingleInstanceSocketPathDetails

[[nodiscard]] inline QString
SingleInstanceSocketPathInDirectory(const QString &runtimeDirectory,
									const QString &serverName,
									uid_t effectiveUid) {
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		return QString();
	}
	const auto canonicalDirectory
		= SingleInstanceSocketPathDetails::VerifiedPrivateRuntimeDirectory(
			runtimeDirectory, effectiveUid);
	return canonicalDirectory.isEmpty()
			   ? QString()
			   : QDir(canonicalDirectory).filePath(serverName);
}

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &runtimeDirectory,
						 const QString &serverName, uid_t effectiveUid,
						 const QString &temporaryDirectory) {
	const auto runtimePath = SingleInstanceSocketPathInDirectory(
		runtimeDirectory, serverName, effectiveUid);
	if (!runtimePath.isEmpty()) {
		return runtimePath;
	}
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		return QString();
	}
	const auto fallbackDirectory
		= SingleInstanceSocketPathDetails::CreateFallbackRuntimeDirectory(
			temporaryDirectory, effectiveUid);
	return fallbackDirectory.isEmpty()
			   ? QString()
			   : QDir(fallbackDirectory).filePath(serverName);
}

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &runtimeDirectory,
						 const QString &serverName, uid_t effectiveUid) {
	return SingleInstanceSocketPath(runtimeDirectory, serverName, effectiveUid,
									QDir::tempPath());
}

} // namespace Platform::Linux
