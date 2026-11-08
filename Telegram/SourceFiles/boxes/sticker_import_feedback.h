/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_telegram_sticker_pack.h"

#include <QtCore/QString>

namespace Ui::StickerImport {

enum class TelegramDownloadFailureMessage {
	InvalidToken,
	InvalidLink,
	UnsupportedPackType,
	PackNotFound,
	ApiError,
	NetworkError,
	InvalidResponse,
	UnsafeFilePath,
	TooManyFiles,
	FileTooLarge,
	TotalTooLarge,
	UnsupportedFormat,
};

struct TelegramDownloadFailure final {
	TelegramDownloadFailureMessage message;
	bool pending = false;
	bool isError = true;
};

[[nodiscard]] TelegramDownloadFailure
TelegramDownloadFailureFeedback(Api::TelegramStickerPackDownloadError error,
								const QString &detail);

} // namespace Ui::StickerImport
