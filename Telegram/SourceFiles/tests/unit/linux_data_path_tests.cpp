/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/version.h"
#include "platform/linux/specific_linux.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

namespace {

bool WriteSettingsFile(const QString &path) {
	return QDir().mkpath(QFileInfo(path).absolutePath())
		   && QFile(path).open(QIODevice::WriteOnly);
}

} // namespace

TEST_CASE(LinuxTeagramDataPathAvoidsTelegramFolders) {
	auto temporary = QTemporaryDir();
	CHECK(temporary.isValid());
	if (!temporary.isValid()) {
		return;
	}

	const auto home = temporary.path();
	const auto dataHome = QDir(home).filePath(u".local/share"_q);
	const auto oldTelegram = QDir(home).filePath(u".TelegramDesktop"_q);
	const auto xdgTelegram = QDir(dataHome).filePath(u"TelegramDesktop"_q);
	const auto teagram = QDir(dataHome).filePath(u"Teagram"_q);
	const auto oldSettings
		= WriteSettingsFile(QDir(oldTelegram).filePath(u"tdata/settings0"_q));
	const auto xdgSettings
		= WriteSettingsFile(QDir(xdgTelegram).filePath(u"tdata/settings1"_q));
	CHECK(oldSettings);
	CHECK(xdgSettings);
	if (!oldSettings || !xdgSettings) {
		return;
	}

	const auto resolved = Platform::LinuxAppDataPath(home, teagram);

	CHECK_EQ(AppName.utf16(), u"Teagram"_q);
	CHECK_EQ(resolved, teagram + '/');
	CHECK(resolved != oldTelegram + '/');
	CHECK(resolved != xdgTelegram + '/');
}

TEST_CASE(LinuxTeagramSkipsLegacyDataMigrationFromTelegramFolders) {
	const auto home = u"/home/teagram-test"_q;
	const auto oldWorkingDirs = {
		QDir(home).filePath(u".TelegramDesktop"_q),
		QDir(home).filePath(u".local/share/TelegramDesktop"_q),
	};
	for (const auto &workingDir : oldWorkingDirs) {
		CHECK(!Platform::LinuxShouldMigrateLegacyDataFiles(workingDir));
	}
}
