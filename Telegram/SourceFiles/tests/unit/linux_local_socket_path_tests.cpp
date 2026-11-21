/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/local_socket_path.h"
#include "platform/linux/desktop_identity.h"
#include "platform/platform_specific.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool SetPrivatePermissions(const QString &path) {
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner;
	return QFile::setPermissions(path, permissions);
}

Platform::Linux::SingleInstanceServerResult
StartSingleInstanceServerAndWait(QLocalServer &server,
								 const QString &serverPath,
								 QString *errorMessage = nullptr) {
	auto result = Platform::Linux::SingleInstanceServerResult::Failed;
	auto loop = QEventLoop();
	Platform::Linux::StartSingleInstanceServer(
		server, serverPath, [](const QString &path) { QFile::remove(path); },
		[&](Platform::Linux::SingleInstanceServerResult completed,
			const QString &error) {
			result = completed;
			if (errorMessage) {
				*errorMessage = error;
			}
			loop.quit();
		});
	QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
	loop.exec();
	return result;
}

} // namespace

TEST_CASE(LinuxSingleInstanceSocketUsesPrivateRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));

	const auto path = Platform::Linux::SingleInstanceSocketPathInDirectory(
		runtimeDirectory.path(), u"teagram-instance"_q, geteuid());
	CHECK_EQ(path, runtimeDirectory.filePath(u"teagram-instance"_q));
}

TEST_CASE(LinuxSingleInstanceSocketPrefersStableRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	auto homeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	CHECK(homeDirectory.isValid());
	if (!runtimeDirectory.isValid() || !homeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));

	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), runtimeDirectory.path(), u"teagram-instance"_q, geteuid(),
		homeDirectory.path());
	CHECK_EQ(path,
			 QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q));
}

TEST_CASE(LinuxSingleInstanceProductionSocketPathIsPrivateAndAbsolute) {
	auto error = QString();
	const auto path
		= Platform::SingleInstanceLocalServerName(u"test-profile-hash"_q, &error);
	CHECK(!path.isEmpty());
	CHECK(QDir::isAbsolutePath(path));
	CHECK(error.isEmpty());
	if (path.isEmpty()) {
		return;
	}
	const auto directory = QFileInfo(path).absolutePath();
	const auto encodedDirectory = QFile::encodeName(directory);
	struct stat status = {};
	CHECK_EQ(lstat(encodedDirectory.constData(), &status), 0);
	CHECK(S_ISDIR(status.st_mode));
	CHECK_EQ(status.st_uid, geteuid());
	CHECK_EQ(status.st_mode & 07777, 0700);
}

TEST_CASE(LinuxSingleInstanceSocketRejectsRuntimeDirectoryOwnedByOtherUid) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto otherUid = (geteuid() == uid_t(0)) ? uid_t(1) : uid_t(0);
	const auto path = Platform::Linux::SingleInstanceSocketPathInDirectory(
		runtimeDirectory.path(), u"teagram-instance"_q, otherUid);
	CHECK(path.isEmpty());
}

TEST_CASE(LinuxSingleInstanceSocketRejectsSharedRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner | QFileDevice::ReadOther
							 | QFileDevice::ExeOther;
	CHECK(QFile::setPermissions(runtimeDirectory.path(), permissions));

	const auto path = Platform::Linux::SingleInstanceSocketPathInDirectory(
		runtimeDirectory.path(), u"teagram-instance"_q, geteuid());
	CHECK(path.isEmpty());
}

TEST_CASE(LinuxSingleInstanceSocketRejectsRelativeDirectoriesAndPathTraversal) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	CHECK(Platform::Linux::SingleInstanceSocketPathInDirectory(
			  u"relative-runtime"_q, u"teagram-instance"_q, geteuid())
			  .isEmpty());
	CHECK(Platform::Linux::SingleInstanceSocketPathInDirectory(
			  runtimeDirectory.path(), u"../teagram-instance"_q, geteuid())
			  .isEmpty());
}

TEST_CASE(LinuxSingleInstanceSocketFallsBackWhenRuntimeDirectoryIsUnset) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	const auto fallbackDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto rejectedRuntimeDirectory
		= homeDirectory.filePath(u"missing-runtime"_q);
	auto selectionWarning = QString();
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), rejectedRuntimeDirectory, u"teagram-instance"_q, geteuid(),
		homeDirectory.path(), nullptr, false, nullptr, &selectionWarning);
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
	CHECK(selectionWarning.contains(rejectedRuntimeDirectory));
	CHECK(selectionWarning.contains(path));

	const auto encodedDirectory = QFile::encodeName(fallbackDirectory);
	struct stat status = {};
	CHECK_EQ(lstat(encodedDirectory.constData(), &status), 0);
	CHECK(S_ISDIR(status.st_mode));
	CHECK(status.st_uid == geteuid());
	CHECK((status.st_mode & 07777) == 0700);
}

TEST_CASE(LinuxSingleInstanceSocketUsesHomeEnvironmentWithoutPasswdEntry) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	namespace Details = Platform::Linux::SingleInstanceSocketPathDetails;
	const auto fromEnvironment
		= Details::UserHomeDirectory(uid_t(4294967294), homeDirectory.path());
	CHECK_EQ(fromEnvironment, homeDirectory.path());
	const auto fromPasswd
		= Details::ChooseHomeDirectory(u"/home/user"_q, homeDirectory.path());
	CHECK_EQ(fromPasswd, u"/home/user"_q);

	const auto fallbackDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), homeDirectory.filePath(u"missing-runtime"_q),
		u"teagram-instance"_q, geteuid(), fromEnvironment);
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
}

TEST_CASE(LinuxSingleInstanceSocketFallsBackFromAnUntrustedRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!runtimeDirectory.isValid() || !homeDirectory.isValid()) {
		return;
	}
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner | QFileDevice::ReadOther
							 | QFileDevice::ExeOther;
	CHECK(QFile::setPermissions(runtimeDirectory.path(), permissions));

	const auto fallbackDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	auto selectionWarning = QString();
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		runtimeDirectory.path(), runtimeDirectory.path(), u"teagram-instance"_q,
		geteuid(), homeDirectory.path(), nullptr, false, nullptr,
		&selectionWarning);
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
	CHECK(selectionWarning.contains(runtimeDirectory.path()));
	CHECK(selectionWarning.contains(path));
}

TEST_CASE(LinuxSingleInstanceSocketUsesOneEndpointWithMixedXdgRuntimeDirs) {
	auto xdgRuntimeDirectory = QTemporaryDir();
	CHECK(xdgRuntimeDirectory.isValid());
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!xdgRuntimeDirectory.isValid() || !homeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(xdgRuntimeDirectory.path()));
	const auto stableRuntimeDirectory
		= homeDirectory.filePath(u"missing-runtime"_q);
	const auto withXdg = Platform::Linux::SingleInstanceSocketPathForLaunch(
		xdgRuntimeDirectory.path(), stableRuntimeDirectory,
		u"teagram-instance"_q, geteuid(), homeDirectory.path());
	const auto withoutXdg = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), stableRuntimeDirectory, u"teagram-instance"_q, geteuid(),
		homeDirectory.path());
	CHECK(!withXdg.isEmpty());
	CHECK_EQ(withXdg, withoutXdg);
	CHECK(withXdg != xdgRuntimeDirectory.filePath(u"teagram-instance"_q));
	if (withXdg.isEmpty() || (withXdg != withoutXdg)) {
		return;
	}
	auto primary = QLocalServer();
	primary.setSocketOptions(QLocalServer::UserAccessOption);
	const auto result = StartSingleInstanceServerAndWait(primary, withXdg);
	CHECK(result == Platform::Linux::SingleInstanceServerResult::Primary);
	if (result != Platform::Linux::SingleInstanceServerResult::Primary) {
		return;
	}
	auto second = QLocalSocket();
	second.connectToServer(withoutXdg);
	CHECK(second.waitForConnected(1'000));
}

TEST_CASE(LinuxSingleInstanceSocketUsesSnapRuntimeDirectory) {
	auto xdgBase = QTemporaryDir();
	auto hostRuntimeDirectory = QTemporaryDir();
	auto homeDirectory = QTemporaryDir();
	CHECK(xdgBase.isValid());
	CHECK(hostRuntimeDirectory.isValid());
	CHECK(homeDirectory.isValid());
	if (!xdgBase.isValid() || !hostRuntimeDirectory.isValid()
		|| !homeDirectory.isValid()) {
		return;
	}
	const auto xdgRuntimeDirectory
		= QDir(xdgBase.path())
			  .filePath(u"run/user/%1/snap.teagram-desktop"_q.arg(
				  qulonglong(geteuid())));
	CHECK(QDir().mkpath(xdgRuntimeDirectory));
	CHECK(SetPrivatePermissions(xdgRuntimeDirectory));
	CHECK(SetPrivatePermissions(hostRuntimeDirectory.path()));
	const auto socketPath
		= QDir(xdgRuntimeDirectory).filePath(u"teagram-instance"_q);
	const auto selectedPath
		= Platform::Linux::SingleInstanceSocketPathForLaunch(
			xdgRuntimeDirectory, hostRuntimeDirectory.path(),
			u"teagram-instance"_q, geteuid(), homeDirectory.path(), nullptr,
			true);
	CHECK_EQ(selectedPath, socketPath);

	auto primary = QLocalServer();
	primary.setSocketOptions(QLocalServer::UserAccessOption);
	CHECK(StartSingleInstanceServerAndWait(primary, selectedPath)
		  == Platform::Linux::SingleInstanceServerResult::Primary);
	if (!primary.isListening()) {
		return;
	}
	auto loop = QEventLoop();
	auto command = QByteArray();
	auto response = QByteArray();
	QObject::connect(&primary, &QLocalServer::newConnection, &loop, [&] {
		const auto peer = primary.nextPendingConnection();
		QObject::connect(peer, &QLocalSocket::readyRead, &loop, [&, peer] {
			command += peer->readAll();
			if (command.contains(';')) {
				peer->write("RES:1_2;");
				peer->flush();
			}
		});
	});
	auto second = QLocalSocket();
	QObject::connect(&second, &QLocalSocket::connected, &loop, [&] {
		second.write("CMD:show;");
		second.flush();
	});
	QObject::connect(&second, &QLocalSocket::readyRead, &loop, [&] {
		response += second.readAll();
		if (response.contains(';')) {
			loop.quit();
		}
	});
	second.connectToServer(selectedPath);
	QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK_EQ(command, QByteArray("CMD:show;"));
	CHECK_EQ(response, QByteArray("RES:1_2;"));
}

TEST_CASE(LinuxSnapSingleInstanceSocketFitsMaximumParallelInstanceName) {
	auto runtimeRoot = QTemporaryDir(u"/tmp/XXXXXX"_q);
	CHECK(runtimeRoot.isValid());
	if (!runtimeRoot.isValid()) {
		return;
	}
	const auto instanceName = u"telegram-desktop_0123456789"_q;
	const auto runtimeDirectory
		= QDir(runtimeRoot.path())
			  .filePath(u"run/user/%1/snap.%2"_q.arg(qulonglong(geteuid()))
							.arg(instanceName));
	CHECK(QDir().mkpath(runtimeDirectory));
	CHECK(SetPrivatePermissions(runtimeDirectory));

	const auto hash = u"0123456789abcdef0123456789abcdef"_q;
	const auto serverName = Platform::Linux::SingleInstanceLocalServerName(
		hash, u"Teagram"_q, true);
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		runtimeDirectory, QString(), serverName, geteuid(), runtimeRoot.path(),
		nullptr, true);
	auto address = sockaddr_un();
	const auto limit = sizeof(address.sun_path) - 2;
	const auto oldServerName = u"snap.%1.%2"_q.arg(instanceName, hash);
	const auto oldSocketPath = QDir(runtimeDirectory).filePath(oldServerName);
	CHECK(size_t(QFile::encodeName(oldSocketPath).size()) > limit);
	CHECK(!path.isEmpty());
	CHECK_EQ(path, QDir(runtimeDirectory).filePath(hash));
	CHECK(size_t(QFile::encodeName(path).size()) <= limit);
}

TEST_CASE(LinuxSingleInstanceSocketReconnectWaitsForErrorCallback) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto server = QLocalServer();
	auto client = QLocalSocket();
	auto loop = QEventLoop();
	auto inErrorCallback = false;
	auto reconnected = false;
	auto command = QByteArray();
	auto response = QByteArray();
	QObject::connect(&client, &QLocalSocket::errorOccurred, &loop, [&] {
		inErrorCallback = true;
		client.close();
		CHECK(server.listen(socketPath));
		Platform::Linux::QueueSingleInstanceSocketReconnect(&loop, &client,
															socketPath);
		inErrorCallback = false;
	});
	QObject::connect(&client, &QLocalSocket::connected, &loop, [&] {
		CHECK(!inErrorCallback);
		reconnected = true;
		client.write("CMD:show;");
		client.flush();
	});
	QObject::connect(&server, &QLocalServer::newConnection, &loop, [&] {
		const auto peer = server.nextPendingConnection();
		QObject::connect(peer, &QLocalSocket::readyRead, &loop, [&, peer] {
			command += peer->readAll();
			if (command.contains(';')) {
				peer->write("RES:1_2;");
				peer->flush();
			}
		});
	});
	QObject::connect(&client, &QLocalSocket::readyRead, &loop, [&] {
		response += client.readAll();
		if (response.contains(';')) {
			loop.quit();
		}
	});
	client.connectToServer(socketPath);
	QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK(reconnected);
	CHECK_EQ(command, QByteArray("CMD:show;"));
	CHECK_EQ(response, QByteArray("RES:1_2;"));
}

TEST_CASE(LinuxSingleInstanceProbeReturnsSecondaryForExistingListener) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto primary = QLocalServer();
	primary.setSocketOptions(QLocalServer::UserAccessOption);
	CHECK(primary.listen(socketPath));
	if (!primary.isListening()) {
		return;
	}
	auto secondary = QLocalServer();
	secondary.setSocketOptions(QLocalServer::UserAccessOption);
	CHECK(StartSingleInstanceServerAndWait(secondary, socketPath)
		  == Platform::Linux::SingleInstanceServerResult::Secondary);
}

TEST_CASE(LinuxSingleInstanceProbeTimeoutReleasesElectionLock) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto server = QLocalServer();
	auto loop = QEventLoop();
	auto result = Platform::Linux::SingleInstanceServerResult::Primary;
	auto error = QString();
	Platform::Linux::StartSingleInstanceServer(
		server, socketPath, [](const QString &path) { QFile::remove(path); },
		[&](Platform::Linux::SingleInstanceServerResult completed,
			const QString &message) {
			result = completed;
			error = message;
			loop.quit();
		},
		1'000, 5, 50,
		[](QLocalSocket &, const QString &) {
			// Keep the probe pending so the production deadline is deterministic.
		});
	QTimer::singleShot(1'000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK(result == Platform::Linux::SingleInstanceServerResult::Failed);
	CHECK(error.contains(u"probing"_q));
	auto lock = QLockFile(socketPath + u".lock"_q);
	CHECK(lock.tryLock(0));
}

TEST_CASE(
	LinuxSingleInstanceSocketUsesPrivateHomeWhenSharedCandidateIsOccupied) {
	auto sharedTemporaryDirectory = QTemporaryDir();
	CHECK(sharedTemporaryDirectory.isValid());
	auto targetDirectory = QTemporaryDir();
	CHECK(targetDirectory.isValid());
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!sharedTemporaryDirectory.isValid() || !targetDirectory.isValid()
		|| !homeDirectory.isValid()) {
		return;
	}
	const auto sharedCandidate = sharedTemporaryDirectory.filePath(
		u"teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto encodedTarget = QFile::encodeName(targetDirectory.path());
	const auto encodedCandidate = QFile::encodeName(sharedCandidate);
	CHECK_EQ(symlink(encodedTarget.constData(), encodedCandidate.constData()),
			 0);

	const auto fallbackDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		sharedCandidate, sharedCandidate, u"teagram-instance"_q, geteuid(),
		homeDirectory.path());
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
}

TEST_CASE(LinuxSingleInstanceSocketFallsBackWhenRuntimeSocketPathIs107Bytes) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	const auto serverName = u"teagram-instance"_q;
	sockaddr_un socketAddress = {};
	const auto limit = sizeof(socketAddress.sun_path) - 2;
	const auto encodedServerName = QFile::encodeName(serverName);
	const auto runtimeDirectoryPathLength
		= limit + 1 - encodedServerName.size() - 1;
	const auto encodedHomeDirectory = QFile::encodeName(homeDirectory.path());
	const auto runtimePathLength
		= runtimeDirectoryPathLength - encodedHomeDirectory.size() - 1;
	const auto runtimeDirectory
		= homeDirectory.filePath(QString(int(runtimePathLength), QChar('x')));
	CHECK(QDir().mkdir(runtimeDirectory));
	CHECK(SetPrivatePermissions(runtimeDirectory));

	const auto oversizedSocketPath
		= QDir(runtimeDirectory).filePath(serverName);
	CHECK_EQ(quint64(QFile::encodeName(oversizedSocketPath).size()),
			 quint64(limit + 1));
	CHECK(Platform::Linux::SingleInstanceSocketPathInDirectory(
			  runtimeDirectory, serverName, geteuid())
			  .isEmpty());

	const auto fallbackDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		runtimeDirectory, runtimeDirectory, serverName, geteuid(),
		homeDirectory.path());
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(serverName));
}

TEST_CASE(LinuxSingleInstanceSocketReportsUnsafeHomeDirectory) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	const auto unsafePermissions
		= QFileDevice::ReadOwner | QFileDevice::WriteOwner
		  | QFileDevice::ExeOwner | QFileDevice::WriteGroup;
	CHECK(QFile::setPermissions(homeDirectory.path(), unsafePermissions));

	auto failure = Platform::Linux::SingleInstanceSocketPathFailure::None;
	auto detail = QString();
	const auto rejectedRuntimeDirectory
		= homeDirectory.filePath(u"missing-runtime"_q);
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), rejectedRuntimeDirectory, u"teagram-instance"_q, geteuid(),
		homeDirectory.path(), &failure, false, &detail);
	CHECK(path.isEmpty());
	CHECK(failure
		  == Platform::Linux::SingleInstanceSocketPathFailure::
			  HomeDirectoryCheckFailed);
	const auto message
		= Platform::Linux::SingleInstanceSocketPathFailureMessage(failure, detail);
	CHECK(message.contains(rejectedRuntimeDirectory));
	CHECK(message.contains(homeDirectory.path()));
	CHECK(!message.contains(u"XDG_RUNTIME_DIR"_q));
}

TEST_CASE(LinuxSingleInstanceSocketReportsFallbackCreationSystemError) {
	if (geteuid() == uid_t(0)) {
		return;
	}
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
	CHECK(QFile::setPermissions(homeDirectory.path(), permissions));

	auto failure = Platform::Linux::SingleInstanceSocketPathFailure::None;
	auto detail = QString();
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), homeDirectory.filePath(u"missing-runtime"_q),
		u"teagram-instance"_q, geteuid(), homeDirectory.path(), &failure, false,
		&detail);
	CHECK(path.isEmpty());
	CHECK(failure
		  == Platform::Linux::SingleInstanceSocketPathFailure::
			  FallbackDirectoryCheckFailed);
	const auto message
		= Platform::Linux::SingleInstanceSocketPathFailureMessage(failure, detail);
	CHECK(message.contains(QString::fromLocal8Bit(strerror(EACCES))));
}

TEST_CASE(LinuxSingleInstanceSocketReportsOverlongFallbackPath) {
	auto temporaryDirectory = QTemporaryDir();
	CHECK(temporaryDirectory.isValid());
	if (!temporaryDirectory.isValid()) {
		return;
	}
	const auto homeDirectory = QDir(temporaryDirectory.path())
								   .filePath(QString(45, QChar('x')) + u"/"_q
											 + QString(45, QChar('y')));
	CHECK(QDir().mkpath(homeDirectory));
	CHECK(SetPrivatePermissions(homeDirectory));

	auto failure = Platform::Linux::SingleInstanceSocketPathFailure::None;
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), temporaryDirectory.filePath(u"missing-runtime"_q),
		u"teagram-instance"_q, geteuid(), homeDirectory, &failure);
	CHECK(path.isEmpty());
	CHECK(
		failure
		== Platform::Linux::SingleInstanceSocketPathFailure::SocketPathTooLong);
	const auto message
		= Platform::Linux::SingleInstanceSocketPathFailureMessage(failure);
	CHECK(message.contains(u"106-byte"_q));
}

TEST_CASE(LinuxSingleInstanceSocketAcceptsRuntimeSocketPathAt106Bytes) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	const auto serverName = u"teagram-instance"_q;
	sockaddr_un socketAddress = {};
	const auto limit = sizeof(socketAddress.sun_path) - 2;
	const auto encodedServerName = QFile::encodeName(serverName);
	const auto runtimeDirectoryPathLength
		= limit - encodedServerName.size() - 1;
	const auto encodedHomeDirectory = QFile::encodeName(homeDirectory.path());
	const auto runtimePathLength
		= runtimeDirectoryPathLength - encodedHomeDirectory.size() - 1;
	const auto runtimeDirectory
		= homeDirectory.filePath(QString(int(runtimePathLength), QChar('x')));
	CHECK(QDir().mkdir(runtimeDirectory));
	CHECK(SetPrivatePermissions(runtimeDirectory));

	const auto socketPath = QDir(runtimeDirectory).filePath(serverName);
	CHECK_EQ(quint64(QFile::encodeName(socketPath).size()), quint64(limit));
	CHECK_EQ(Platform::Linux::SingleInstanceSocketPathInDirectory(
				 runtimeDirectory, serverName, geteuid()),
			 socketPath);
}

TEST_CASE(LinuxSingleInstanceSocketRejectsASymlinkFallbackDirectory) {
	auto homeDirectory = QTemporaryDir();
	CHECK(homeDirectory.isValid());
	if (!homeDirectory.isValid()) {
		return;
	}
	auto targetDirectory = QTemporaryDir();
	CHECK(targetDirectory.isValid());
	if (!targetDirectory.isValid()) {
		return;
	}
	const auto runtimeDirectory = homeDirectory.filePath(
		u".teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto encodedTarget = QFile::encodeName(targetDirectory.path());
	const auto encodedRuntime = QFile::encodeName(runtimeDirectory);
	CHECK_EQ(symlink(encodedTarget.constData(), encodedRuntime.constData()), 0);

	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), homeDirectory.filePath(u"missing-runtime"_q),
		u"teagram-instance"_q, geteuid(), homeDirectory.path());
	CHECK(path.isEmpty());
}

TEST_CASE(LinuxSingleInstanceServerHandsOffToAnExistingPrimary) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto primary = QLocalServer();
	auto secondary = QLocalServer();
	primary.setSocketOptions(QLocalServer::UserAccessOption);
	secondary.setSocketOptions(QLocalServer::UserAccessOption);
	const auto firstResult
		= StartSingleInstanceServerAndWait(primary, socketPath);
	const auto secondResult
		= StartSingleInstanceServerAndWait(secondary, socketPath);
	CHECK(firstResult == Platform::Linux::SingleInstanceServerResult::Primary);
	CHECK(secondResult
		  == Platform::Linux::SingleInstanceServerResult::Secondary);
}

TEST_CASE(LinuxSingleInstanceServerRecoversAfterPrimaryCrashes) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	const auto encodedPath = QFile::encodeName(socketPath);
	CHECK(size_t(encodedPath.size()) < sizeof(address.sun_path));
	memcpy(address.sun_path, encodedPath.constData(), encodedPath.size() + 1);
	const auto abandonedPrimary = ::socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(abandonedPrimary >= 0);
	if (abandonedPrimary < 0) {
		return;
	}
	const auto bound
		= ::bind(abandonedPrimary,
				reinterpret_cast<const sockaddr *>(&address),
				socklen_t(sizeof(address)));
	CHECK_EQ(bound, 0);
	if (bound != 0) {
		::close(abandonedPrimary);
		return;
	}
	::close(abandonedPrimary);
	CHECK(QFileInfo::exists(socketPath));

	auto recovered = QLocalServer();
	recovered.setSocketOptions(QLocalServer::UserAccessOption);
	CHECK(StartSingleInstanceServerAndWait(recovered, socketPath)
		  == Platform::Linux::SingleInstanceServerResult::Primary);
	CHECK(recovered.isListening());
}

TEST_CASE(LinuxSingleInstanceSocketRetriesContendedElectionWithoutBlocking) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto heldLock = QLockFile(socketPath + u".lock"_q);
	CHECK(heldLock.tryLock(0));
	auto server = QLocalServer();
	auto loop = QEventLoop();
	auto timerFired = false;
	auto result = Platform::Linux::SingleInstanceServerResult::Primary;
	auto error = QString();
	Platform::Linux::StartSingleInstanceServer(
		server, socketPath, [](const QString &path) { QFile::remove(path); },
		[&](Platform::Linux::SingleInstanceServerResult completed,
			const QString &message) {
			result = completed;
			error = message;
			loop.quit();
		},
		50, 5);
	QTimer::singleShot(1, &loop, [&] { timerFired = true; });
	QTimer::singleShot(1'000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK(timerFired);
	CHECK(result == Platform::Linux::SingleInstanceServerResult::Failed);
	CHECK(error.contains(u"election lock"_q));
	CHECK(error != server.errorString());
	CHECK(error.contains(socketPath + u".lock"_q));
	CHECK(error.contains(u"close any stuck Telegram process and retry"_q));
}

TEST_CASE(LinuxSingleInstanceSocketHandsOffCommandAfterConcurrentLaunches) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);
	auto loop = QEventLoop();
	auto command = QByteArray();
	auto response = QByteArray();
	auto failure = QString();
	struct Launch final {
		QLocalServer server;
		QLocalSocket socket;
		bool electionStarted = false;
		Platform::Linux::SingleInstanceServerResult result
			= Platform::Linux::SingleInstanceServerResult::Failed;
	};
	auto first = Launch();
	auto second = Launch();
	const auto prepare = [&](Launch &launch) {
		launch.server.setSocketOptions(QLocalServer::UserAccessOption);
		const auto launchPointer = &launch;
		QObject::connect(
			&launch.server, &QLocalServer::newConnection, &loop,
			[&, launchPointer] {
				const auto peer = launchPointer->server.nextPendingConnection();
				QObject::connect(peer, &QLocalSocket::readyRead, &loop,
								 [&, peer] {
									 command += peer->readAll();
									 if (command.contains(';')) {
										 peer->write("RES:123_456;");
										 peer->flush();
									 }
								 });
			});
		QObject::connect(
			&launch.socket, &QLocalSocket::errorOccurred, &loop,
			[&, launchPointer = &launch](QLocalSocket::LocalSocketError) {
				if (launchPointer->electionStarted) {
					return;
				}
				launchPointer->electionStarted = true;
				Platform::Linux::StartSingleInstanceServer(
					launchPointer->server, socketPath,
					[](const QString &path) { QFile::remove(path); },
					[&, launchPointer](
						Platform::Linux::SingleInstanceServerResult result,
						const QString &error) {
						launchPointer->result = result;
						if (result
							== Platform::Linux::SingleInstanceServerResult::
								Secondary) {
							Platform::Linux::QueueSingleInstanceSocketReconnect(
								&loop, &launchPointer->socket, socketPath);
						} else if (result
								   == Platform::Linux::
									   SingleInstanceServerResult::Failed) {
							failure = error;
							loop.quit();
						}
					});
			});
		QObject::connect(&launch.socket, &QLocalSocket::connected, &loop,
						 [&, launchPointer] {
							 if (!launchPointer->electionStarted) {
								 launchPointer->result = Platform::Linux::
									 SingleInstanceServerResult::Secondary;
							 }
							 launchPointer->socket.write("CMD:show;");
							 launchPointer->socket.flush();
						 });
		QObject::connect(&launch.socket, &QLocalSocket::readyRead, &loop,
						 [&, launchPointer] {
							 response += launchPointer->socket.readAll();
							 if (response.contains(';')) {
								 loop.quit();
							 }
						 });
	};
	prepare(first);
	prepare(second);
	first.socket.connectToServer(socketPath);
	second.socket.connectToServer(socketPath);
	QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK(failure.isEmpty());
	CHECK((first.result == Platform::Linux::SingleInstanceServerResult::Primary)
		  != (second.result
			  == Platform::Linux::SingleInstanceServerResult::Primary));
	CHECK(
		(first.result == Platform::Linux::SingleInstanceServerResult::Secondary)
		!= (second.result
			== Platform::Linux::SingleInstanceServerResult::Secondary));
	CHECK_EQ(command, QByteArray("CMD:show;"));
	CHECK_EQ(response, QByteArray("RES:123_456;"));
}
