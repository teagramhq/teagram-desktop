/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/version.h"
#include "platform/linux/specific_linux.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

namespace {

bool WriteSettingsFile(const QString &path) {
	return QDir().mkpath(QFileInfo(path).absolutePath())
		   && QFile(path).open(QIODevice::WriteOnly);
}

class ScopedEnvironmentVariable final {
  public:
	ScopedEnvironmentVariable(const char *name, const QByteArray &value)
		: _name(name), _wasSet(qEnvironmentVariableIsSet(name)),
		  _oldValue(qgetenv(name)) {
		qputenv(_name, value);
	}

	~ScopedEnvironmentVariable() {
		if (_wasSet) {
			qputenv(_name, _oldValue);
		} else {
			qunsetenv(_name);
		}
	}

  private:
	const char *_name = nullptr;
	const bool _wasSet = false;
	const QByteArray _oldValue;
};

class ScopedApplicationName final {
  public:
	ScopedApplicationName() : _oldName(QCoreApplication::applicationName()) {}

	~ScopedApplicationName() { QCoreApplication::setApplicationName(_oldName); }

  private:
	const QString _oldName;
};

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

	const auto oldApplicationName = ScopedApplicationName();
	const auto oldHome = ScopedEnvironmentVariable("HOME", home.toUtf8());
	const auto oldDataHome
		= ScopedEnvironmentVariable("XDG_DATA_HOME", dataHome.toUtf8());
	QCoreApplication::setApplicationName(AppNameQt.utf16());

	const auto resolved = psAppDataPath();

	CHECK_EQ(AppNameQt.utf16(), u"Teagram"_q);
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
