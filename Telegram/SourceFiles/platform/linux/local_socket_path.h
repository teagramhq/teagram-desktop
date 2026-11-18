/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QString>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <cerrno>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

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

[[nodiscard]] inline QString VerifiedUserHomeDirectory(const QString &path,
													   uid_t effectiveUid) {
	struct stat status = {};
	const auto canonicalPath = CanonicalExistingDirectory(path, &status, true);
	if (canonicalPath.isEmpty() || (status.st_uid != effectiveUid)
		|| (status.st_mode & (S_IWGRP | S_IWOTH))) {
		return QString();
	}
	return canonicalPath;
}

[[nodiscard]] inline bool IsValidServerName(const QString &serverName) {
	return !serverName.isEmpty() && !serverName.contains('/')
		   && (serverName != u"."_q) && (serverName != u".."_q);
}

[[nodiscard]] inline QString
CreateFallbackRuntimeDirectory(const QString &homeDirectory,
							   uid_t effectiveUid) {
	const auto canonicalHomeDirectory
		= VerifiedUserHomeDirectory(homeDirectory, effectiveUid);
	if (canonicalHomeDirectory.isEmpty()) {
		return QString();
	}
	const auto runtimeName = u".teagram-runtime-%1"_q.arg(
		QString::number(static_cast<qulonglong>(effectiveUid)));
	const auto runtimeDirectory
		= QDir(canonicalHomeDirectory).filePath(runtimeName);
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

[[nodiscard]] inline QString
SingleInstanceRuntimeDirectory(uid_t effectiveUid) {
	return u"/run/user/%1"_q.arg(
		QString::number(static_cast<qulonglong>(effectiveUid)));
}

[[nodiscard]] inline QString
ChooseHomeDirectory(const QString &passwdHome, const QString &environmentHome) {
	return passwdHome.isEmpty() ? environmentHome : passwdHome;
}

[[nodiscard]] inline QString UserHomeDirectory(uid_t effectiveUid,
											   const QString &environmentHome) {
	const auto entry = getpwuid(effectiveUid);
	auto passwdHome = QString();
	if (entry && entry->pw_dir) {
		passwdHome = QFile::decodeName(QByteArray(entry->pw_dir));
	}
	return ChooseHomeDirectory(passwdHome, environmentHome);
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
	if (canonicalDirectory.isEmpty()) {
		return QString();
	}
	const auto socketPath = QDir(canonicalDirectory).filePath(serverName);
	const auto encodedSocketPath = QFile::encodeName(socketPath);
	auto socketAddress = sockaddr_un();
	const auto socketPathLimit = sizeof(socketAddress.sun_path) - 2;
	return size_t(encodedSocketPath.size()) > socketPathLimit ? QString()
															  : socketPath;
}

[[nodiscard]] inline QString
SingleInstanceSocketPathForLaunch(const QString &xdgRuntimeDirectory,
								  const QString &stableRuntimeDirectory,
								  const QString &serverName, uid_t effectiveUid,
								  const QString &homeDirectory) {
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		return QString();
	}
	const auto stableRuntimePath = SingleInstanceSocketPathInDirectory(
		stableRuntimeDirectory, serverName, effectiveUid);
	const auto xdgRuntimePath = SingleInstanceSocketPathInDirectory(
		xdgRuntimeDirectory, serverName, effectiveUid);
	if (!stableRuntimePath.isEmpty()) {
		return (xdgRuntimePath == stableRuntimePath) ? xdgRuntimePath
													 : stableRuntimePath;
	}
	const auto fallbackDirectory
		= SingleInstanceSocketPathDetails::CreateFallbackRuntimeDirectory(
			homeDirectory, effectiveUid);
	return fallbackDirectory.isEmpty()
			   ? QString()
			   : SingleInstanceSocketPathInDirectory(fallbackDirectory,
													 serverName, effectiveUid);
}

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &xdgRuntimeDirectory,
						 const QString &serverName, uid_t effectiveUid,
						 const QString &homeDirectory) {
	return SingleInstanceSocketPathForLaunch(
		xdgRuntimeDirectory,
		SingleInstanceSocketPathDetails::SingleInstanceRuntimeDirectory(
			effectiveUid),
		serverName, effectiveUid, homeDirectory);
}

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &xdgRuntimeDirectory,
						 const QString &serverName, uid_t effectiveUid) {
	return SingleInstanceSocketPath(
		xdgRuntimeDirectory, serverName, effectiveUid,
		SingleInstanceSocketPathDetails::UserHomeDirectory(
			effectiveUid, qEnvironmentVariable("HOME")));
}

enum class SingleInstanceServerResult {
	Primary,
	Secondary,
	Failed,
};

template <typename Action>
[[nodiscard]] inline bool
WithSingleInstanceServerLock(const QString &serverPath, Action action) {
	QLockFile electionLock(serverPath + u".lock"_q);
	if (!electionLock.tryLock(30'000)) {
		return false;
	}
	action();
	return true;
}

template <typename Cleanup>
[[nodiscard]] inline SingleInstanceServerResult
StartSingleInstanceServer(QLocalServer &server, const QString &serverPath,
						  Cleanup cleanup) {
	auto result = SingleInstanceServerResult::Failed;
	if (!WithSingleInstanceServerLock(serverPath, [&] {
			QLocalSocket existing;
			existing.connectToServer(serverPath);
			if (existing.waitForConnected(1'000)) {
				result = SingleInstanceServerResult::Secondary;
				return;
			} else if (existing.error() != QLocalSocket::ServerNotFoundError
					   && existing.error()
							  != QLocalSocket::ConnectionRefusedError) {
				return;
			}

			cleanup(serverPath);
			result = server.listen(serverPath)
						 ? SingleInstanceServerResult::Primary
						 : SingleInstanceServerResult::Failed;
		})) {
		return SingleInstanceServerResult::Failed;
	}
	return result;
}

} // namespace Platform::Linux
