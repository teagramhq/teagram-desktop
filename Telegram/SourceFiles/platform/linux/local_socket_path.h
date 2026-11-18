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
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <cerrno>
#include <pwd.h>
#include <utility>
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

enum class SingleInstanceSocketPathFailure {
	None,
	InvalidServerName,
	XdgRuntimeDirectoryCheckFailed,
	HomeDirectoryCheckFailed,
	FallbackDirectoryCheckFailed,
	SocketPathTooLong,
};

[[nodiscard]] inline QString SingleInstanceSocketPathFailureMessage(
	SingleInstanceSocketPathFailure failure) {
	switch (failure) {
	case SingleInstanceSocketPathFailure::InvalidServerName:
		return u"Could not create the single-instance IPC socket: invalid server name."_q;
	case SingleInstanceSocketPathFailure::XdgRuntimeDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket: the Snap XDG_RUNTIME_DIR failed its owner, mode, or path-length check."_q;
	case SingleInstanceSocketPathFailure::HomeDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket. XDG_RUNTIME_DIR is unset, unsafe, too long, or unavailable, and HOME failed the owner or mode check."_q;
	case SingleInstanceSocketPathFailure::FallbackDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket. XDG_RUNTIME_DIR is unset, unsafe, too long, or unavailable, and the private HOME runtime directory failed its owner, mode, or symlink check."_q;
	case SingleInstanceSocketPathFailure::SocketPathTooLong:
		return u"Could not create the single-instance IPC socket. XDG_RUNTIME_DIR is unset, unsafe, too long, or unavailable, and the HOME fallback exceeds Linux's 106-byte AF_UNIX socket path limit."_q;
	case SingleInstanceSocketPathFailure::None:
		break;
	}
	return QString();
}

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

[[nodiscard]] inline QString SingleInstanceSocketPathForLaunch(
	const QString &xdgRuntimeDirectory, const QString &stableRuntimeDirectory,
	const QString &serverName, uid_t effectiveUid, const QString &homeDirectory,
	SingleInstanceSocketPathFailure *failure = nullptr,
	bool useXdgRuntimeForSingleInstance = false) {
	if (failure) {
		*failure = SingleInstanceSocketPathFailure::None;
	}
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		if (failure) {
			*failure = SingleInstanceSocketPathFailure::InvalidServerName;
		}
		return QString();
	}
	if (useXdgRuntimeForSingleInstance) {
		const auto xdgRuntimePath = SingleInstanceSocketPathInDirectory(
			xdgRuntimeDirectory, serverName, effectiveUid);
		if (!xdgRuntimePath.isEmpty()) {
			return xdgRuntimePath;
		}
		if (failure) {
			*failure = SingleInstanceSocketPathFailure::
				XdgRuntimeDirectoryCheckFailed;
		}
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
	if (SingleInstanceSocketPathDetails::VerifiedUserHomeDirectory(
			homeDirectory, effectiveUid)
			.isEmpty()) {
		if (failure) {
			*failure
				= SingleInstanceSocketPathFailure::HomeDirectoryCheckFailed;
		}
		return QString();
	}
	const auto fallbackDirectory
		= SingleInstanceSocketPathDetails::CreateFallbackRuntimeDirectory(
			homeDirectory, effectiveUid);
	if (fallbackDirectory.isEmpty()) {
		if (failure) {
			*failure
				= SingleInstanceSocketPathFailure::FallbackDirectoryCheckFailed;
		}
		return QString();
	}
	const auto fallbackPath = SingleInstanceSocketPathInDirectory(
		fallbackDirectory, serverName, effectiveUid);
	if (fallbackPath.isEmpty() && failure) {
		*failure = SingleInstanceSocketPathFailure::SocketPathTooLong;
	}
	return fallbackPath;
}

[[nodiscard]] inline QString
SingleInstanceSocketPath(const QString &xdgRuntimeDirectory,
						 const QString &serverName, uid_t effectiveUid,
						 const QString &homeDirectory,
						 SingleInstanceSocketPathFailure *failure = nullptr,
						 bool useXdgRuntimeForSingleInstance = false) {
	return SingleInstanceSocketPathForLaunch(
		xdgRuntimeDirectory,
		SingleInstanceSocketPathDetails::SingleInstanceRuntimeDirectory(
			effectiveUid),
		serverName, effectiveUid, homeDirectory, failure,
		useXdgRuntimeForSingleInstance);
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

inline void QueueSingleInstanceSocketReconnect(QObject *context,
											   QLocalSocket *socket,
											   QString serverPath) {
	QTimer::singleShot(0, context,
					   [socket, serverPath = std::move(serverPath)] {
						   socket->connectToServer(serverPath);
					   });
}

template <typename Cleanup, typename Finished>
class SingleInstanceServerStarter final : public QObject {
  public:
	SingleInstanceServerStarter(QLocalServer &server, QString serverPath,
								Cleanup cleanup, Finished finished,
								int lockTimeoutMs, int retryIntervalMs)
		: QObject(&server), _server(server), _serverPath(std::move(serverPath)),
		  _lock(_serverPath + u".lock"_q), _cleanup(std::move(cleanup)),
		  _finished(std::move(finished)), _retryTimer(this), _probeSocket(this),
		  _lockTimeoutMs(lockTimeoutMs), _retryIntervalMs(retryIntervalMs) {
		_retryTimer.setSingleShot(true);
		connect(&_retryTimer, &QTimer::timeout, this,
				[this] { tryAcquireLock(); });
		connect(&_probeSocket, &QLocalSocket::connected, this, [this] {
			finish(SingleInstanceServerResult::Secondary, QString());
		});
		connect(&_probeSocket, &QLocalSocket::errorOccurred, this,
				[this](QLocalSocket::LocalSocketError error) {
					if (error == QLocalSocket::ServerNotFoundError
						|| error == QLocalSocket::ConnectionRefusedError) {
						_cleanup(_serverPath);
						if (_server.listen(_serverPath)) {
							finish(SingleInstanceServerResult::Primary,
								   QString());
						} else {
							finish(SingleInstanceServerResult::Failed,
								   _server.errorString());
						}
					} else {
						finish(SingleInstanceServerResult::Failed,
							   _probeSocket.errorString());
					}
				});
		_lockTimer.start();
		QTimer::singleShot(0, this, [this] { tryAcquireLock(); });
	}

  private:
	void tryAcquireLock() {
		if (!_lock.tryLock(0)) {
			switch (_lock.error()) {
			case QLockFile::PermissionError:
				return finish(
					SingleInstanceServerResult::Failed,
					QString::fromLatin1("Permission denied acquiring the "
										"single-instance election lock."));
			case QLockFile::UnknownError:
				return finish(
					SingleInstanceServerResult::Failed,
					QString::fromLatin1("Could not acquire the single-instance "
										"election lock."));
			case QLockFile::LockFailedError:
			case QLockFile::NoError:
				break;
			}
			if (_lockTimer.elapsed() >= _lockTimeoutMs) {
				return finish(
					SingleInstanceServerResult::Failed,
					QString::fromLatin1("Timed out waiting for the "
										"single-instance election lock."));
			}
			_retryTimer.start(_retryIntervalMs);
			return;
		}
		_probeSocket.connectToServer(_serverPath);
	}

	void finish(SingleInstanceServerResult result, QString error) {
		if (_completed) {
			return;
		}
		_completed = true;
		_retryTimer.stop();
		_lock.unlock();
		_probeSocket.abort();
		auto finished = std::move(_finished);
		deleteLater();
		finished(result, error);
	}

	QLocalServer &_server;
	const QString _serverPath;
	QLockFile _lock;
	Cleanup _cleanup;
	Finished _finished;
	QElapsedTimer _lockTimer;
	QTimer _retryTimer;
	QLocalSocket _probeSocket;
	const int _lockTimeoutMs = 30'000;
	const int _retryIntervalMs = 50;
	bool _completed = false;
};

template <typename Cleanup, typename Finished>
void StartSingleInstanceServer(QLocalServer &server, const QString &serverPath,
							   Cleanup cleanup, Finished finished,
							   int lockTimeoutMs = 30'000,
							   int retryIntervalMs = 50) {
	new SingleInstanceServerStarter<Cleanup, Finished>(
		server, serverPath, std::move(cleanup), std::move(finished),
		lockTimeoutMs, retryIntervalMs);
}

} // namespace Platform::Linux
