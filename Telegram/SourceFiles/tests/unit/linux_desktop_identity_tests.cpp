/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/version.h"
#include "platform/linux/desktop_identity.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QDir>
#include <QtCore/QSettings>

TEST_CASE(LinuxTeagramDesktopIdentityUsesPrivateNames) {
	const auto instanceHash = u"fixturehash"_q;
	const auto desktopFileId
		= Platform::Linux::DesktopFileId(instanceHash, true);
	CHECK_EQ(desktopFileId, u"org.teagram.desktop._fixturehash"_q);
	const auto staticDesktopFileId
		= Platform::Linux::DesktopFileId(QString(), false);
	CHECK_EQ(staticDesktopFileId, u"org.teagram.desktop"_q);
	CHECK_EQ(Platform::Linux::DesktopIconName(desktopFileId, instanceHash),
			 u"org.teagram.desktop"_q);
	CHECK_EQ(AppName.utf16(), u"Teagram"_q);
	CHECK_EQ(AppNameQt.utf16(), u"Teagram"_q);

	const auto files = Platform::Linux::DesktopIntegrationFileNamesFor(
		desktopFileId,
		Platform::Linux::DesktopIconName(desktopFileId, instanceHash));
	CHECK_EQ(files.desktopEntry, u"org.teagram.desktop._fixturehash.desktop"_q);
	CHECK_EQ(files.dbusService, u"org.teagram.desktop._fixturehash.service"_q);
	CHECK_EQ(files.applicationIcon, u"org.teagram.desktop.png"_q);
	CHECK_EQ(files.trayIcon, u"org.teagram.desktop-symbolic.svg"_q);
	CHECK_EQ(files.trayAttentionIcon,
			 u"org.teagram.desktop-attention-symbolic.svg"_q);
	CHECK_EQ(files.trayMuteIcon, u"org.teagram.desktop-mute-symbolic.svg"_q);
	const auto staticFiles = Platform::Linux::DesktopIntegrationFileNamesFor(
		staticDesktopFileId,
		Platform::Linux::DesktopIconName(staticDesktopFileId, instanceHash));
	CHECK_EQ(staticFiles.desktopEntry, u"org.teagram.desktop.desktop"_q);
	CHECK_EQ(staticFiles.dbusService, u"org.teagram.desktop.service"_q);
	CHECK_EQ(staticFiles.applicationIcon, u"org.teagram.desktop.png"_q);
	for (const auto &name : {
			 files.desktopEntry,
			 files.dbusService,
			 files.applicationIcon,
			 files.trayIcon,
			 files.trayAttentionIcon,
			 files.trayMuteIcon,
		 }) {
		CHECK(!name.startsWith(u"org.telegram.desktop"_q));
		CHECK(name != u"telegram.desktop"_q);
		CHECK(name != u"telegramdesktop.desktop"_q);
	}
	CHECK(Platform::Linux::SingleInstanceLocalServerName(
			  u"same-hash"_q, AppNameQt.utf16(), false, QString())
		  != u"same-hash-TelegramDesktop"_q);
	CHECK(Platform::Linux::SingleInstanceLocalServerName(
			  u"same-hash"_q, AppNameQt.utf16(), true, u"telegram-desktop"_q)
		  != u"snap.telegram-desktop.same-hash"_q);

	const auto xdg = QDir(QString::fromUtf8(TDESKTOP_TEST_SOURCE_DIR))
						 .filePath(u"lib/xdg"_q);
	auto desktop = QSettings(QDir(xdg).filePath(staticFiles.desktopEntry),
							 QSettings::IniFormat);
	desktop.beginGroup(u"Desktop Entry"_q);
	CHECK_EQ(desktop.value(u"Name"_q).toString(), u"Teagram"_q);
	CHECK_EQ(desktop.value(u"Icon"_q).toString(), u"org.teagram.desktop"_q);
	desktop.endGroup();

	auto service = QSettings(QDir(xdg).filePath(staticFiles.dbusService),
							 QSettings::IniFormat);
	service.beginGroup(u"D-BUS Service"_q);
	CHECK_EQ(service.value(u"Name"_q).toString(), u"org.teagram.desktop"_q);
}
