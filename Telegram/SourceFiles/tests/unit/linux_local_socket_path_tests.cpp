/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/local_socket_path.h"
#include "tests/unit/unit_test.h"

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

	const auto path = Platform::Linux::SingleInstanceSocketPath(
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
	const auto path = Platform::Linux::SingleInstanceSocketPath(
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

	const auto path = Platform::Linux::SingleInstanceSocketPath(
		runtimeDirectory.path(), u"teagram-instance"_q, geteuid());
	CHECK(path.isEmpty());
}

TEST_CASE(LinuxSingleInstanceSocketRejectsRelativeDirectoriesAndPathTraversal) {
	CHECK(Platform::Linux::SingleInstanceSocketPath(
			  u"relative-runtime"_q, u"teagram-instance"_q, geteuid())
			  .isEmpty());
	CHECK(Platform::Linux::SingleInstanceSocketPath(
			  u"/run/user/1000"_q, u"../teagram-instance"_q, geteuid())
			  .isEmpty());
}
