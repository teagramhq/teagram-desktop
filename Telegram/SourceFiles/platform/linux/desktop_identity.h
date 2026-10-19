/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <kshell.h>

namespace Platform::Linux {

struct DesktopIntegrationFileNames {
	QString desktopEntry;
	QString dbusService;
	QString applicationIcon;
	QString trayIcon;
	QString trayAttentionIcon;
	QString trayMuteIcon;
};

[[nodiscard]] inline QString
ShortcutExecutablePath(bool updaterDisabled, const QStringList &arguments,
					   const QString &executableDirectory,
					   const QString &executableName) {
#ifdef TDESKTOP_TEAGRAM
	return executableDirectory + executableName;
#else  // !TDESKTOP_TEAGRAM
	if (updaterDisabled) {
		if (!arguments.isEmpty()) {
			const auto result = QFileInfo(arguments.first()).fileName();
			if (!result.isEmpty()) {
				return result;
			}
		}
		return executableName;
	}
	return executableDirectory + executableName;
#endif // TDESKTOP_TEAGRAM
}

[[nodiscard]] inline QString
DesktopEntryCommandLine(const QStringList &arguments) {
	return KShell::joinArgs(arguments).replace('\\', u"\\\\"_q);
}

[[nodiscard]] inline QString
DesktopEntryTryExecValue(const QString &executable) {
	return executable;
}

[[nodiscard]] inline QString
DBusServiceCommandLine(const QStringList &arguments) {
	return KShell::joinArgs(arguments);
}

[[nodiscard]] inline QString DesktopFileId(const QString &instanceHash,
										   bool includeInstanceHash) {
#ifdef TDESKTOP_TEAGRAM
	const auto base = u"org.teagram.desktop"_q;
#else  // !TDESKTOP_TEAGRAM
	const auto base = u"org.telegram.desktop"_q;
#endif // TDESKTOP_TEAGRAM
	return includeInstanceHash ? base + u"._"_q + instanceHash : base;
}

[[nodiscard]] inline QString DesktopIconName(const QString &desktopFileId,
											 const QString &instanceHash) {
	auto result = desktopFileId;
	result.remove(u"._"_q + instanceHash);
	return result;
}

[[nodiscard]] inline DesktopIntegrationFileNames
DesktopIntegrationFileNamesFor(const QString &desktopFileId,
							   const QString &iconName) {
	return {
		desktopFileId + u".desktop"_q,
		desktopFileId + u".service"_q,
		iconName + u".png"_q,
		iconName + u"-symbolic.svg"_q,
		iconName + u"-attention-symbolic.svg"_q,
		iconName + u"-mute-symbolic.svg"_q,
	};
}

[[nodiscard]] inline QString
SingleInstanceLocalServerName(const QString &hash,
							  const QString &applicationName, bool snap,
							  const QString &snapInstanceName) {
	if (snap) {
		const auto result = u"snap.%1.%2"_q.arg(snapInstanceName, hash);
#ifdef TDESKTOP_TEAGRAM
		return result + '-' + applicationName;
#else  // !TDESKTOP_TEAGRAM
		return result;
#endif // TDESKTOP_TEAGRAM
	}
	return hash + '-' + applicationName;
}

} // namespace Platform::Linux
