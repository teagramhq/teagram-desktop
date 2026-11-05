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
#include <vector>

namespace Api::details {

struct TelegramStickerInfo {
	QString fileId;
	QString customEmojiId;
	QString emoji;
	Core::StickerImportFormat format = Core::StickerImportFormat::Webp;
};

struct TelegramStickerSetInfo {
	QString title;
	std::vector<TelegramStickerInfo> stickers;
};

[[nodiscard]] bool IsValidTelegramBotToken(const QString &token);
[[nodiscard]] bool IsSafeTelegramStickerFilePath(const QString &path);
[[nodiscard]] QUrl TelegramBotApiMethodUrl(const QString &token,
										   const QString &method);
[[nodiscard]] std::optional<QUrl>
TelegramBotApiFileUrl(const QString &token, const QString &filePath);
[[nodiscard]] QString RedactTelegramBotApiError(QString description,
												const QString &token);
[[nodiscard]] std::optional<TelegramStickerSetInfo>
ParseTelegramStickerSetResponse(const QByteArray &response,
								Core::StickerImportType type);
[[nodiscard]] std::optional<std::vector<TelegramStickerInfo>>
ParseTelegramCustomEmojiStickersResponse(
	const QByteArray &response,
	const std::vector<TelegramStickerInfo> &packStickers);

} // namespace Api::details
