/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/local_socket_path.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool SetPrivatePermissions(const QString &path) {
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner;
	return QFile::setPermissions(path, permissions);
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
	CHECK(Platform::Linux::SingleInstanceSocketPathInDirectory(
			  u"relative-runtime"_q, u"teagram-instance"_q, geteuid())
			  .isEmpty());
	CHECK(Platform::Linux::SingleInstanceSocketPathInDirectory(
			  u"/run/user/1000"_q, u"../teagram-instance"_q, geteuid())
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
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		QString(), homeDirectory.filePath(u"missing-runtime"_q),
		u"teagram-instance"_q, geteuid(), homeDirectory.path());
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));

	const auto encodedDirectory = QFile::encodeName(fallbackDirectory);
	struct stat status = {};
	CHECK_EQ(lstat(encodedDirectory.constData(), &status), 0);
	CHECK(S_ISDIR(status.st_mode));
	CHECK(status.st_uid == geteuid());
	CHECK((status.st_mode & 07777) == 0700);
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
	const auto path = Platform::Linux::SingleInstanceSocketPathForLaunch(
		runtimeDirectory.path(), runtimeDirectory.path(), u"teagram-instance"_q,
		geteuid(), homeDirectory.path());
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
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
	const auto result = Platform::Linux::StartSingleInstanceServer(
		primary, withXdg, [](const QString &path) { QFile::remove(path); });
	CHECK(result == Platform::Linux::SingleInstanceServerResult::Primary);
	if (result != Platform::Linux::SingleInstanceServerResult::Primary) {
		return;
	}
	auto second = QLocalSocket();
	second.connectToServer(withoutXdg);
	CHECK(second.waitForConnected(1'000));
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
	const auto runtimePathLength = limit + 1 - encodedServerName.size() - 1;
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
	const auto runtimePathLength = limit - encodedServerName.size() - 1;
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
	const auto cleanup = [](const QString &path) { QFile::remove(path); };
	const auto firstResult = Platform::Linux::StartSingleInstanceServer(
		primary, socketPath, cleanup);
	const auto secondResult = Platform::Linux::StartSingleInstanceServer(
		secondary, socketPath, cleanup);
	CHECK(firstResult == Platform::Linux::SingleInstanceServerResult::Primary);
	CHECK(secondResult
		  == Platform::Linux::SingleInstanceServerResult::Secondary);
}

TEST_CASE(LinuxSingleInstanceSocketSerializesConcurrentPrimaryElection) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	CHECK(SetPrivatePermissions(runtimeDirectory.path()));
	const auto socketPath
		= QDir(runtimeDirectory.path()).filePath(u"teagram-instance"_q);

	std::condition_variable condition;
	std::mutex mutex;
	auto active = std::atomic_bool(false);
	auto primaryExists = std::atomic_bool(false);
	auto overlapped = std::atomic_bool(false);
	auto lockFailures = std::atomic_int(0);
	auto ready = 0;
	auto primary = std::atomic_int(0);
	auto secondary = std::atomic_int(0);
	auto start = false;
	const auto launch = [&] {
		{
			std::unique_lock lock(mutex);
			++ready;
			condition.notify_all();
			condition.wait(lock, [&] { return start; });
		}
		const auto locked
			= Platform::Linux::WithSingleInstanceServerLock(socketPath, [&] {
				  if (active.exchange(true)) {
					  overlapped = true;
				  }
				  std::this_thread::sleep_for(std::chrono::milliseconds(100));
				  if (primaryExists.exchange(true)) {
					  ++secondary;
				  } else {
					  ++primary;
				  }
				  active = false;
			  });
		if (!locked) {
			++lockFailures;
		}
	};
	std::thread first(launch);
	std::thread second(launch);
	{
		std::unique_lock lock(mutex);
		condition.wait(lock, [&] { return ready == 2; });
		start = true;
		condition.notify_all();
	}
	first.join();
	second.join();

	CHECK(!overlapped.load());
	CHECK_EQ(primary.load(), 1);
	CHECK_EQ(secondary.load(), 1);
	CHECK_EQ(lockFailures.load(), 0);
}
