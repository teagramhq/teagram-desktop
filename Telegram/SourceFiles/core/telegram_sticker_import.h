/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/sticker_import.h"

#include <QtCore/QUrl>

#include <optional>

namespace Core {

struct TelegramStickerPackLink {
	StickerImportType type = StickerImportType::Stickers;
	QString name;
	QString canonicalUrl;
};

[[nodiscard]] std::optional<TelegramStickerPackLink>
ParseTelegramStickerPackLink(const QString &link);
[[nodiscard]] QString TelegramStickerPackCanonicalUrl(StickerImportType type,
													  const QString &name);
[[nodiscard]] bool IsValidTelegramStickerBotToken(const QString &token);
[[nodiscard]] bool IsValidTelegramStickerFilePath(const QString &path);
[[nodiscard]] QString RedactTelegramStickerBotToken(QString text,
													const QString &token);
[[nodiscard]] std::optional<QUrl>
TelegramStickerBotApiUrl(const QString &token, const QString &method);
[[nodiscard]] std::optional<QUrl>
TelegramStickerBotFileUrl(const QString &token, const QString &filePath);

} // namespace Core
