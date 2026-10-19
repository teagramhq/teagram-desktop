/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Platform::Linux {

struct DesktopIntegrationFileNames {
	QString desktopEntry;
	QString dbusService;
	QString applicationIcon;
	QString trayIcon;
	QString trayAttentionIcon;
	QString trayMuteIcon;
};

[[nodiscard]] inline QString DesktopFileId(
		const QString &instanceHash,
		bool includeInstanceHash) {
#ifdef TDESKTOP_TEAGRAM
	const auto base = u"org.teagram.desktop"_q;
#else // !TDESKTOP_TEAGRAM
	const auto base = u"org.telegram.desktop"_q;
#endif // TDESKTOP_TEAGRAM
	return includeInstanceHash
		? base + u"._"_q + instanceHash
		: base;
}

[[nodiscard]] inline QString DesktopIconName(
		const QString &desktopFileId,
		const QString &instanceHash) {
	auto result = desktopFileId;
	result.remove(u"._"_q + instanceHash);
	return result;
}

[[nodiscard]] inline DesktopIntegrationFileNames
DesktopIntegrationFileNamesFor(
		const QString &desktopFileId,
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

[[nodiscard]] inline QString SingleInstanceLocalServerName(
		const QString &hash,
		const QString &applicationName,
		bool snap,
		const QString &snapInstanceName) {
	if (snap) {
		const auto result = u"snap.%1.%2"_q.arg(snapInstanceName, hash);
#ifdef TDESKTOP_TEAGRAM
		return result + '-' + applicationName;
#else // !TDESKTOP_TEAGRAM
		return result;
#endif // TDESKTOP_TEAGRAM
	}
	return hash + '-' + applicationName;
}

} // namespace Platform::Linux
