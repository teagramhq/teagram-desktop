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

#include <unistd.h>

TEST_CASE(LinuxSingleInstanceSocketUsesPrivateRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner;
	const auto permissionsSet
		= QFile::setPermissions(runtimeDirectory.path(), permissions);
	CHECK(permissionsSet);
	if (!permissionsSet) {
		return;
	}

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
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner;
	const auto permissionsSet
		= QFile::setPermissions(runtimeDirectory.path(), permissions);
	CHECK(permissionsSet);
	if (!permissionsSet) {
		return;
	}
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
	const auto permissionsSet
		= QFile::setPermissions(runtimeDirectory.path(), permissions);
	CHECK(permissionsSet);
	if (!permissionsSet) {
		return;
	}

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
	auto temporaryDirectory = QTemporaryDir();
	CHECK(temporaryDirectory.isValid());
	if (!temporaryDirectory.isValid()) {
		return;
	}
	const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
							 | QFileDevice::ExeOwner;
	const auto permissionsSet
		= QFile::setPermissions(temporaryDirectory.path(), permissions);
	CHECK(permissionsSet);
	if (!permissionsSet) {
		return;
	}

	const auto runtimeDirectory = temporaryDirectory.filePath(
		u"teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto path = Platform::Linux::SingleInstanceSocketPath(
		QString(), u"teagram-instance"_q, geteuid(), temporaryDirectory.path());
	CHECK_EQ(path, QDir(runtimeDirectory).filePath(u"teagram-instance"_q));

	const auto encodedDirectory = QFile::encodeName(runtimeDirectory);
	struct stat status = {};
	CHECK_EQ(lstat(encodedDirectory.constData(), &status), 0);
	CHECK(S_ISDIR(status.st_mode));
	CHECK_EQ(status.st_uid, geteuid());
	CHECK_EQ(status.st_mode & 07777, 0700);
}

TEST_CASE(LinuxSingleInstanceSocketFallsBackFromAnUntrustedRuntimeDirectory) {
	auto runtimeDirectory = QTemporaryDir();
	CHECK(runtimeDirectory.isValid());
	if (!runtimeDirectory.isValid()) {
		return;
	}
	auto temporaryDirectory = QTemporaryDir();
	CHECK(temporaryDirectory.isValid());
	if (!temporaryDirectory.isValid()) {
		return;
	}
	const auto sharedPermissions = QFileDevice::ReadOwner
		| QFileDevice::WriteOwner | QFileDevice::ExeOwner
		| QFileDevice::ReadOther | QFileDevice::ExeOther;
	const auto permissionsSet
		= QFile::setPermissions(runtimeDirectory.path(), sharedPermissions);
	CHECK(permissionsSet);
	if (!permissionsSet) {
		return;
	}

	const auto fallbackDirectory = temporaryDirectory.filePath(
		u"teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto path = Platform::Linux::SingleInstanceSocketPath(
		runtimeDirectory.path(), u"teagram-instance"_q, geteuid(),
		temporaryDirectory.path());
	CHECK_EQ(path, QDir(fallbackDirectory).filePath(u"teagram-instance"_q));
}

TEST_CASE(LinuxSingleInstanceSocketRejectsASymlinkFallbackDirectory) {
	auto temporaryDirectory = QTemporaryDir();
	CHECK(temporaryDirectory.isValid());
	if (!temporaryDirectory.isValid()) {
		return;
	}
	auto targetDirectory = QTemporaryDir();
	CHECK(targetDirectory.isValid());
	if (!targetDirectory.isValid()) {
		return;
	}
	const auto runtimeDirectory = temporaryDirectory.filePath(
		u"teagram-runtime-%1"_q.arg(qulonglong(geteuid())));
	const auto encodedTarget = QFile::encodeName(targetDirectory.path());
	const auto encodedRuntime = QFile::encodeName(runtimeDirectory);
	CHECK_EQ(symlink(encodedTarget.constData(), encodedRuntime.constData()), 0);

	const auto path = Platform::Linux::SingleInstanceSocketPath(
		QString(), u"teagram-instance"_q, geteuid(), temporaryDirectory.path());
	CHECK(path.isEmpty());
}
