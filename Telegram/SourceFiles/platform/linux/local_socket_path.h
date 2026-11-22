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
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <utility>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

namespace Platform::Linux {
namespace SingleInstanceSocketPathDetails {

inline void SetFailureDetail(QString *detail, QString message) {
	if (detail) {
		*detail = std::move(message);
	}
}

inline void SetSystemCallFailure(QString *detail, const char *operation,
								 const QString &path, int errorNumber) {
	if (detail) {
		*detail = u"%1('%2') failed: %3 (errno %4)."_q
					  .arg(QString::fromLatin1(operation), path,
						   QString::fromLocal8Bit(strerror(errorNumber)))
					  .arg(errorNumber);
	}
}

[[nodiscard]] inline QString
CanonicalExistingDirectory(const QString &path, struct stat *status,
						   bool rejectFinalSymlink,
						   QString *failureDetail = nullptr) {
	SetFailureDetail(failureDetail, QString());
	if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
		SetFailureDetail(
			failureDetail,
			u"directory path '%1' is empty or not absolute."_q.arg(path));
		return QString();
	}
	const auto cleanPath = QDir::cleanPath(path);
	struct stat pathStatus = {};
	if (rejectFinalSymlink) {
		const auto encodedPath = QFile::encodeName(cleanPath);
		if (lstat(encodedPath.constData(), &pathStatus) != 0) {
			const auto errorNumber = errno;
			SetSystemCallFailure(failureDetail, "lstat", cleanPath,
								 errorNumber);
			return QString();
		}
		if (!S_ISDIR(pathStatus.st_mode)) {
			SetFailureDetail(
				failureDetail,
				u"directory path '%1' is not a directory or is a symlink."_q
					.arg(cleanPath));
			return QString();
		}
	}
	const auto canonicalPath = QFileInfo(cleanPath).canonicalFilePath();
	if (canonicalPath.isEmpty()) {
		SetFailureDetail(
			failureDetail,
			u"directory path '%1' could not be canonicalized."_q.arg(
				cleanPath));
		return QString();
	}
	const auto encodedCanonicalPath = QFile::encodeName(canonicalPath);
	struct stat canonicalStatus = {};
	if (lstat(encodedCanonicalPath.constData(), &canonicalStatus) != 0) {
		const auto errorNumber = errno;
		SetSystemCallFailure(failureDetail, "lstat", canonicalPath,
							 errorNumber);
		return QString();
	}
	if (!S_ISDIR(canonicalStatus.st_mode)
		|| (rejectFinalSymlink
			&& ((pathStatus.st_dev != canonicalStatus.st_dev)
				|| (pathStatus.st_ino != canonicalStatus.st_ino)))) {
		SetFailureDetail(
			failureDetail,
			u"directory path '%1' failed the directory or symlink check."_q.arg(
				canonicalPath));
		return QString();
	}
	*status = canonicalStatus;
	return canonicalPath;
}

[[nodiscard]] inline QString
VerifiedPrivateRuntimeDirectory(const QString &path, uid_t effectiveUid,
								QString *failureDetail = nullptr) {
	struct stat status = {};
	const auto canonicalPath
		= CanonicalExistingDirectory(path, &status, true, failureDetail);
	if (canonicalPath.isEmpty()) {
		return QString();
	}
	if ((status.st_uid != effectiveUid) || ((status.st_mode & 07777) != 0700)) {
		SetFailureDetail(
			failureDetail,
			u"directory '%1' is not owned by uid %2 with mode 0700."_q
				.arg(canonicalPath)
				.arg(qulonglong(effectiveUid)));
		return QString();
	}
	return canonicalPath;
}

[[nodiscard]] inline QString
VerifiedUserHomeDirectory(const QString &path, uid_t effectiveUid,
						  QString *failureDetail = nullptr) {
	struct stat status = {};
	const auto canonicalPath
		= CanonicalExistingDirectory(path, &status, true, failureDetail);
	if (canonicalPath.isEmpty()) {
		return QString();
	}
	if ((status.st_uid != effectiveUid)
		|| (status.st_mode & (S_IWGRP | S_IWOTH))) {
		SetFailureDetail(
			failureDetail,
			u"home directory '%1' is not owned by uid %2 or is group/world writable."_q
				.arg(canonicalPath)
				.arg(qulonglong(effectiveUid)));
		return QString();
	}
	return canonicalPath;
}

[[nodiscard]] inline bool IsValidServerName(const QString &serverName) {
	return !serverName.isEmpty() && !serverName.contains('/')
		   && (serverName != u"."_q) && (serverName != u".."_q);
}

[[nodiscard]] inline QString
CreateFallbackRuntimeDirectory(const QString &homeDirectory, uid_t effectiveUid,
							   QString *failureDetail = nullptr) {
	const auto canonicalHomeDirectory
		= VerifiedUserHomeDirectory(homeDirectory, effectiveUid, failureDetail);
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
	if (!created) {
		const auto errorNumber = errno;
		if (errorNumber != EEXIST) {
			SetSystemCallFailure(failureDetail, "mkdir", runtimeDirectory,
								 errorNumber);
			return QString();
		}
	}
	if (created) {
		struct stat status = {};
		if (lstat(encodedRuntimeDirectory.constData(), &status) != 0) {
			const auto errorNumber = errno;
			SetSystemCallFailure(failureDetail, "lstat", runtimeDirectory,
								 errorNumber);
			return QString();
		}
		if (!S_ISDIR(status.st_mode) || (status.st_uid != effectiveUid)) {
			SetFailureDetail(
				failureDetail,
				u"new runtime directory '%1' is not a user-owned directory."_q
					.arg(runtimeDirectory));
			return QString();
		}
		if (chmod(encodedRuntimeDirectory.constData(), 0700) != 0) {
			const auto errorNumber = errno;
			SetSystemCallFailure(failureDetail, "chmod", runtimeDirectory,
								 errorNumber);
			return QString();
		}
	}
	return VerifiedPrivateRuntimeDirectory(runtimeDirectory, effectiveUid,
										   failureDetail);
}

[[nodiscard]] inline QString
SingleInstanceRuntimeDirectory(uid_t effectiveUid) {
	return u"/run/user/%1"_q.arg(
		QString::number(static_cast<qulonglong>(effectiveUid)));
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

[[nodiscard]] inline QString
SingleInstanceSocketPathFailureMessage(SingleInstanceSocketPathFailure failure,
									   const QString &failureDetail
									   = QString()) {
	if (!failureDetail.isEmpty()) {
		return u"Could not create the single-instance IPC socket: %1"_q.arg(
			failureDetail);
	}
	switch (failure) {
	case SingleInstanceSocketPathFailure::InvalidServerName:
		return u"Could not create the single-instance IPC socket: invalid server name."_q;
	case SingleInstanceSocketPathFailure::XdgRuntimeDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket: the Snap XDG_RUNTIME_DIR failed its owner, mode, or path-length check."_q;
	case SingleInstanceSocketPathFailure::HomeDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket: /run/user/<uid> is unavailable or too long, and the HOME directory failed its owner or mode check."_q;
	case SingleInstanceSocketPathFailure::FallbackDirectoryCheckFailed:
		return u"Could not create the single-instance IPC socket: /run/user/<uid> is unavailable or too long, and the private .teagram-runtime-<uid> directory under HOME failed its owner, mode, or symlink check."_q;
	case SingleInstanceSocketPathFailure::SocketPathTooLong:
		return u"Could not create the single-instance IPC socket: /run/user/<uid> is unavailable or too long, and the .teagram-runtime-<uid> fallback under HOME exceeds Linux's 106-byte AF_UNIX socket path limit."_q;
	case SingleInstanceSocketPathFailure::None:
		break;
	}
	return QString();
}

[[nodiscard]] inline QString SingleInstanceSocketPathInDirectory(
	const QString &runtimeDirectory, const QString &serverName,
	uid_t effectiveUid, QString *failureDetail = nullptr) {
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail, u"server name is invalid."_q);
		return QString();
	}
	const auto canonicalDirectory
		= SingleInstanceSocketPathDetails::VerifiedPrivateRuntimeDirectory(
			runtimeDirectory, effectiveUid, failureDetail);
	if (canonicalDirectory.isEmpty()) {
		return QString();
	}
	const auto socketPath = QDir(canonicalDirectory).filePath(serverName);
	const auto encodedSocketPath = QFile::encodeName(socketPath);
	auto socketAddress = sockaddr_un();
	const auto socketPathLimit = sizeof(socketAddress.sun_path) - 2;
	if (size_t(encodedSocketPath.size()) > socketPathLimit) {
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail,
			u"encoded socket path '%1' is %2 bytes; the Linux limit is %3 bytes."_q
				.arg(socketPath)
				.arg(encodedSocketPath.size())
				.arg(socketPathLimit));
		return QString();
	}
	return socketPath;
}

[[nodiscard]] inline QString
SingleInstanceProfileLockIdentity(const QString &serverPath, bool snap) {
	const auto info = QFileInfo(serverPath);
	return snap ? (info.absolutePath() + '/' + info.fileName())
				: info.fileName();
}

[[nodiscard]] inline QString
SingleInstanceProfileLockIdentityForServerName(const QString &serverName,
											   const QString &runtimeDirectory,
											   uid_t effectiveUid, bool snap) {
	if (!snap) {
		return SingleInstanceSocketPathDetails::IsValidServerName(serverName)
				   ? serverName
				   : QString();
	}
	const auto serverPath = SingleInstanceSocketPathInDirectory(
		runtimeDirectory, serverName, effectiveUid);
	return serverPath.isEmpty()
			   ? QString()
			   : SingleInstanceProfileLockIdentity(serverPath, true);
}

[[nodiscard]] inline QString
SingleInstanceServerPathFromLockContentsForIdentity(
	const QString &contents, const QString &profileLockIdentity,
	uid_t effectiveUid, bool snap = false) {
	if (profileLockIdentity.isEmpty() || !QDir::isAbsolutePath(contents)
		|| (SingleInstanceProfileLockIdentity(contents, snap)
			!= profileLockIdentity)) {
		return QString();
	}
	const auto validatedPath = SingleInstanceSocketPathInDirectory(
		QFileInfo(contents).absolutePath(), QFileInfo(contents).fileName(),
		effectiveUid);
	return (validatedPath == contents) ? validatedPath : QString();
}

[[nodiscard]] inline QString SingleInstanceServerPathFromLockContents(
	const QString &contents, const QString &candidatePath, uid_t effectiveUid,
	bool snap = false) {
	return SingleInstanceServerPathFromLockContentsForIdentity(
		contents, SingleInstanceProfileLockIdentity(candidatePath, snap),
		effectiveUid, snap);
}

[[nodiscard]] inline QString SingleInstanceSocketPathForLaunch(
	const QString &xdgRuntimeDirectory, const QString &stableRuntimeDirectory,
	const QString &serverName, uid_t effectiveUid, const QString &homeDirectory,
	SingleInstanceSocketPathFailure *failure = nullptr,
	bool useXdgRuntimeForSingleInstance = false,
	QString *failureDetail = nullptr, QString *selectionWarning = nullptr) {
	if (failure) {
		*failure = SingleInstanceSocketPathFailure::None;
	}
	SingleInstanceSocketPathDetails::SetFailureDetail(failureDetail, QString());
	SingleInstanceSocketPathDetails::SetFailureDetail(selectionWarning,
													  QString());
	if (!SingleInstanceSocketPathDetails::IsValidServerName(serverName)) {
		if (failure) {
			*failure = SingleInstanceSocketPathFailure::InvalidServerName;
		}
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail, u"the requested server name is invalid."_q);
		return QString();
	}
	if (useXdgRuntimeForSingleInstance) {
		auto xdgFailureDetail = QString();
		const auto xdgRuntimePath = SingleInstanceSocketPathInDirectory(
			xdgRuntimeDirectory, serverName, effectiveUid, &xdgFailureDetail);
		if (!xdgRuntimePath.isEmpty()) {
			return xdgRuntimePath;
		}
		if (failure) {
			*failure = SingleInstanceSocketPathFailure::
				XdgRuntimeDirectoryCheckFailed;
		}
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail,
			u"Snap XDG_RUNTIME_DIR '%1' failed verification: %2"_q.arg(
				xdgRuntimeDirectory.isEmpty() ? u"<unset>"_q
											  : xdgRuntimeDirectory,
				xdgFailureDetail));
		return QString();
	}
	auto stableFailureDetail = QString();
	const auto stableRuntimePath = SingleInstanceSocketPathInDirectory(
		stableRuntimeDirectory, serverName, effectiveUid, &stableFailureDetail);
	if (!stableRuntimePath.isEmpty()) {
		return stableRuntimePath;
	}
	auto homeFailureDetail = QString();
	const auto verifiedHomeDirectory
		= SingleInstanceSocketPathDetails::VerifiedUserHomeDirectory(
			homeDirectory, effectiveUid, &homeFailureDetail);
	if (verifiedHomeDirectory.isEmpty()) {
		if (failure) {
			*failure
				= SingleInstanceSocketPathFailure::HomeDirectoryCheckFailed;
		}
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail,
			u"runtime candidate '%1' was rejected (%2); HOME directory '%3' failed validation (%4)."_q
				.arg(stableRuntimeDirectory, stableFailureDetail, homeDirectory,
					 homeFailureDetail));
		return QString();
	}
	const auto fallbackCandidate
		= QDir(verifiedHomeDirectory)
			  .filePath(u".teagram-runtime-%1"_q.arg(qulonglong(effectiveUid)));
	auto fallbackFailureDetail = QString();
	const auto fallbackDirectory
		= SingleInstanceSocketPathDetails::CreateFallbackRuntimeDirectory(
			verifiedHomeDirectory, effectiveUid, &fallbackFailureDetail);
	if (fallbackDirectory.isEmpty()) {
		if (failure) {
			*failure
				= SingleInstanceSocketPathFailure::FallbackDirectoryCheckFailed;
		}
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail,
			u"runtime candidate '%1' was rejected (%2); private fallback '%3' under HOME directory '%4' failed (%5)."_q
				.arg(stableRuntimeDirectory, stableFailureDetail,
					 fallbackCandidate, verifiedHomeDirectory,
					 fallbackFailureDetail));
		return QString();
	}
	auto fallbackPathFailureDetail = QString();
	const auto fallbackPath = SingleInstanceSocketPathInDirectory(
		fallbackDirectory, serverName, effectiveUid,
		&fallbackPathFailureDetail);
	if (fallbackPath.isEmpty() && failure) {
		*failure = SingleInstanceSocketPathFailure::SocketPathTooLong;
	}
	if (fallbackPath.isEmpty()) {
		SingleInstanceSocketPathDetails::SetFailureDetail(
			failureDetail,
			u"runtime candidate '%1' was rejected (%2); private fallback '%3' under HOME directory '%4' is unusable (%5)."_q
				.arg(stableRuntimeDirectory, stableFailureDetail,
					 fallbackDirectory, verifiedHomeDirectory,
					 fallbackPathFailureDetail));
		return QString();
	}
	SingleInstanceSocketPathDetails::SetFailureDetail(
		selectionWarning,
		u"Linux single-instance runtime candidate '%1' was rejected (%2); using private fallback '%3'."_q
			.arg(stableRuntimeDirectory, stableFailureDetail, fallbackPath));
	return fallbackPath;
}

[[nodiscard]] inline QString SingleInstanceSocketPath(
	const QString &xdgRuntimeDirectory, const QString &serverName,
	uid_t effectiveUid, const QString &homeDirectory,
	SingleInstanceSocketPathFailure *failure = nullptr,
	bool useXdgRuntimeForSingleInstance = false,
	QString *failureDetail = nullptr, QString *selectionWarning = nullptr) {
	return SingleInstanceSocketPathForLaunch(
		xdgRuntimeDirectory,
		SingleInstanceSocketPathDetails::SingleInstanceRuntimeDirectory(
			effectiveUid),
		serverName, effectiveUid, homeDirectory, failure,
		useXdgRuntimeForSingleInstance, failureDetail, selectionWarning);
}

enum class SingleInstanceServerResult {
	Primary,
	Secondary,
	Failed,
};

enum class SingleInstanceProfileLockState {
	Unlocked,
	Locked,
	Error,
};

struct SingleInstanceProfileLockSnapshot {
	SingleInstanceProfileLockState state
		= SingleInstanceProfileLockState::Error;
	QString contents;
	QString error;
};

enum class SingleInstanceEndpointStatus {
	NotRunning,
	Located,
	Connected,
	Failed,
};

struct SingleInstanceEndpointResult {
	SingleInstanceEndpointStatus status = SingleInstanceEndpointStatus::Failed;
	QString serverPath;
	QString error;
};

template <typename LockProbe, typename Finished>
class SingleInstanceServerPathDiscovery final : public QObject {
  public:
	SingleInstanceServerPathDiscovery(QObject *context, LockProbe probe,
									  QString profileLockIdentity,
									  uid_t effectiveUid, bool snap,
									  int timeoutMs, int retryIntervalMs,
									  Finished finished)
		: QObject(context), _probe(std::move(probe)),
		  _profileLockIdentity(std::move(profileLockIdentity)),
		  _effectiveUid(effectiveUid), _snap(snap), _timeoutMs(timeoutMs),
		  _retryIntervalMs(retryIntervalMs), _finished(std::move(finished)),
		  _timer(this) {
		_timer.setSingleShot(true);
		connect(&_timer, &QTimer::timeout, this,
				[this] { checkProfileLock(); });
	}

	void start() {
		_deadline.start();
		_timer.start(0);
	}

  private:
	void checkProfileLock() {
		const auto snapshot = _probe();
		switch (snapshot.state) {
		case SingleInstanceProfileLockState::Unlocked:
			return finish({SingleInstanceEndpointStatus::NotRunning, QString(),
						   QString()});
		case SingleInstanceProfileLockState::Error:
			return finish(
				{SingleInstanceEndpointStatus::Failed, QString(),
				 snapshot.error.isEmpty()
					 ? u"Could not read the single-instance profile lock."_q
					 : snapshot.error});
		case SingleInstanceProfileLockState::Locked: {
			const auto path
				= SingleInstanceServerPathFromLockContentsForIdentity(
					snapshot.contents, _profileLockIdentity, _effectiveUid,
					_snap);
			if (!path.isEmpty()) {
				return finish(
					{SingleInstanceEndpointStatus::Located, path, QString()});
			}
			_lastError
				= u"The busy profile lock has not published a valid IPC path."_q;
		} break;
		}
		const auto elapsed = int(_deadline.elapsed());
		if (elapsed >= _timeoutMs) {
			return finish(
				{SingleInstanceEndpointStatus::Failed, QString(),
				 _lastError.isEmpty()
					 ? u"Timed out waiting for the single-instance profile lock."_q
					 : u"Timed out waiting for the profile lock to publish a "
					   u"valid IPC path: %1"_q.arg(_lastError)});
		}
		_timer.start(
			std::min(std::max(_retryIntervalMs, 1), _timeoutMs - elapsed));
	}

	void finish(SingleInstanceEndpointResult result) {
		if (_completed) {
			return;
		}
		_completed = true;
		auto finished = std::move(_finished);
		_timer.stop();
		auto guard = QPointer<SingleInstanceServerPathDiscovery>(this);
		finished(std::move(result));
		if (guard) {
			guard->deleteLater();
		}
	}

	LockProbe _probe;
	const QString _profileLockIdentity;
	const uid_t _effectiveUid = 0;
	const bool _snap = false;
	const int _timeoutMs = 0;
	const int _retryIntervalMs = 0;
	Finished _finished;
	QElapsedTimer _deadline;
	QTimer _timer;
	QString _lastError;
	bool _completed = false;
};

template <typename LockProbe, typename Finished>
void StartSingleInstanceServerPathDiscovery(QObject *context, LockProbe probe,
											const QString &profileLockIdentity,
											uid_t effectiveUid, bool snap,
											int timeoutMs, int retryIntervalMs,
											Finished finished) {
	const auto discovery
		= new SingleInstanceServerPathDiscovery<LockProbe, Finished>(
			context, std::move(probe), profileLockIdentity, effectiveUid, snap,
			timeoutMs, retryIntervalMs, std::move(finished));
	discovery->start();
}

template <typename LockProbe>
[[nodiscard]] SingleInstanceEndpointResult ConnectToSingleInstanceServer(
	QLocalSocket &socket, LockProbe probe, const QString &profileLockIdentity,
	uid_t effectiveUid, bool snap, int timeoutMs, int retryIntervalMs) {
	auto timer = QElapsedTimer();
	timer.start();
	auto lastError = QString();
	for (;;) {
		const auto snapshot = probe();
		switch (snapshot.state) {
		case SingleInstanceProfileLockState::Unlocked:
			return {SingleInstanceEndpointStatus::NotRunning, QString(),
					QString()};
		case SingleInstanceProfileLockState::Error:
			return {SingleInstanceEndpointStatus::Failed, QString(),
					snapshot.error.isEmpty()
						? u"Could not read the single-instance profile lock."_q
						: snapshot.error};
		case SingleInstanceProfileLockState::Locked: {
			const auto path
				= SingleInstanceServerPathFromLockContentsForIdentity(
					snapshot.contents, profileLockIdentity, effectiveUid, snap);
			if (!path.isEmpty()) {
				const auto elapsed = int(timer.elapsed());
				const auto remaining = timeoutMs - elapsed;
				if (remaining <= 0) {
					lastError
						= u"Timed out connecting to the single-instance server at '%1'."_q
							  .arg(path);
					break;
				}
				socket.abort();
				socket.connectToServer(path);
				const auto connectTimeout
					= std::min(std::max(retryIntervalMs, 1), remaining);
				if (socket.waitForConnected(connectTimeout)) {
					return {SingleInstanceEndpointStatus::Connected, path,
							QString()};
				}
				const auto socketError = socket.error();
				lastError = socket.errorString();
				socket.abort();
				if (socketError != QLocalSocket::ServerNotFoundError
					&& socketError != QLocalSocket::ConnectionRefusedError
					&& socketError != QLocalSocket::SocketTimeoutError) {
					return {SingleInstanceEndpointStatus::Failed, path,
							lastError};
				}
			} else {
				lastError
					= u"The busy profile lock has not published a valid IPC path."_q;
			}
		} break;
		}
		const auto elapsed = int(timer.elapsed());
		if (elapsed >= timeoutMs) {
			return {
				SingleInstanceEndpointStatus::Failed, QString(),
				lastError.isEmpty()
					? u"Timed out waiting for the single-instance server."_q
					: u"Timed out connecting to the single-instance server: %1"_q
						  .arg(lastError)};
		}
		const auto delay
			= std::min(std::max(retryIntervalMs, 1), timeoutMs - elapsed);
		QThread::msleep(static_cast<unsigned long>(delay));
	}
}

inline void QueueSingleInstanceSocketReconnect(QObject *context,
											   QLocalSocket *socket,
											   QString serverPath) {
	QTimer::singleShot(0, context,
					   [socket, serverPath = std::move(serverPath)] {
						   socket->connectToServer(serverPath);
					   });
}

template <typename Callback>
inline void QueueSingleInstanceProbeCompletion(QObject *context,
											   Callback callback) {
	QTimer::singleShot(0, context, std::move(callback));
}

using SingleInstanceProbeStart
	= std::function<void(QLocalSocket &, const QString &)>;

template <typename Cleanup, typename Finished>
class SingleInstanceServerStarter final : public QObject {
  public:
	SingleInstanceServerStarter(QLocalServer &server, QString serverPath,
								Cleanup cleanup, Finished finished,
								int lockTimeoutMs, int retryIntervalMs,
								int probeTimeoutMs,
								SingleInstanceProbeStart probeStart)
		: QObject(&server), _server(server), _serverPath(std::move(serverPath)),
		  _lock(_serverPath + u".lock"_q), _cleanup(std::move(cleanup)),
		  _finished(std::move(finished)), _retryTimer(this), _probeTimer(this),
		  _probeSocket(this), _probeStart(std::move(probeStart)),
		  _lockTimeoutMs(lockTimeoutMs), _retryIntervalMs(retryIntervalMs),
		  _probeTimeoutMs(probeTimeoutMs) {
		if (!_probeStart) {
			_probeStart = [](QLocalSocket &socket, const QString &path) {
				socket.connectToServer(path);
			};
		}
		_retryTimer.setSingleShot(true);
		_probeTimer.setSingleShot(true);
		connect(&_retryTimer, &QTimer::timeout, this,
				[this] { tryAcquireLock(); });
		connect(&_probeSocket, &QLocalSocket::connected, this, [this] {
			_probeTimer.stop();
			QueueSingleInstanceProbeCompletion(this, [this] {
				finish(SingleInstanceServerResult::Secondary, QString());
			});
		});
		connect(&_probeSocket, &QLocalSocket::errorOccurred, this,
				[this](QLocalSocket::LocalSocketError error) {
					_probeTimer.stop();
					const auto errorMessage = _probeSocket.errorString();
					QueueSingleInstanceProbeCompletion(this, [this, error,
															  errorMessage] {
						if (_completed) {
							return;
						}
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
								   errorMessage);
						}
					});
				});
		connect(&_probeTimer, &QTimer::timeout, this, [this] {
			finish(
				SingleInstanceServerResult::Failed,
				QString::fromLatin1(
					"Timed out probing the existing single-instance server."));
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
					u"Timed out waiting for the single-instance election lock "
					u"'%1'. "
					"Close any stuck Telegram process and retry."_q.arg(
						_serverPath + u".lock"_q));
			}
			_retryTimer.start(_retryIntervalMs);
			return;
		}
		_probeTimer.start(_probeTimeoutMs);
		_probeStart(_probeSocket, _serverPath);
	}

	void finish(SingleInstanceServerResult result, QString error) {
		if (_completed) {
			return;
		}
		_completed = true;
		_retryTimer.stop();
		_probeTimer.stop();
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
	QTimer _probeTimer;
	QLocalSocket _probeSocket;
	SingleInstanceProbeStart _probeStart;
	const int _lockTimeoutMs = 30'000;
	const int _retryIntervalMs = 50;
	const int _probeTimeoutMs = 5'000;
	bool _completed = false;
};

template <typename Cleanup, typename Finished>
void StartSingleInstanceServer(QLocalServer &server, const QString &serverPath,
							   Cleanup cleanup, Finished finished,
							   int lockTimeoutMs = 30'000,
							   int retryIntervalMs = 50,
							   int probeTimeoutMs = 5'000,
							   SingleInstanceProbeStart probeStart = {}) {
	new SingleInstanceServerStarter<Cleanup, Finished>(
		server, serverPath, std::move(cleanup), std::move(finished),
		lockTimeoutMs, retryIntervalMs, probeTimeoutMs, std::move(probeStart));
}

} // namespace Platform::Linux
