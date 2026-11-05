/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/sticker_import.h"

#include <QtCore/QString>

#include <optional>

namespace Core {

struct StickerSetInstallTarget {
	QString shortName;
	StickerImportType type = StickerImportType::Stickers;
};

[[nodiscard]] QString TryConvertUrlToLocal(QString url);
[[nodiscard]] QString TryConvertUrlToLocal(QString url,
										   const QString &internalLinksDomain,
										   bool hasPinnedServer);
[[nodiscard]] std::optional<StickerSetInstallTarget>
StickerSetInstallTargetFromLocalUrl(const QString &url);

} // namespace Core
