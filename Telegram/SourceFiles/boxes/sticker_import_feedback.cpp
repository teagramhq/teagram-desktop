/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_import_feedback.h"

#include "base/assertion.h"

namespace Ui::StickerImport {

TelegramDownloadFailure
TelegramDownloadFailureFeedback(Api::TelegramStickerPackDownloadError error,
								const QString &detail, bool &scanPending) {
	scanPending = false;
	const auto message = [&] {
		switch (error) {
		case Api::TelegramStickerPackDownloadError::InvalidToken:
			return TelegramDownloadFailureMessage::InvalidToken;
		case Api::TelegramStickerPackDownloadError::InvalidLink:
			return TelegramDownloadFailureMessage::InvalidLink;
		case Api::TelegramStickerPackDownloadError::UnsupportedPackType:
			return TelegramDownloadFailureMessage::UnsupportedPackType;
		case Api::TelegramStickerPackDownloadError::PackNotFound:
			return TelegramDownloadFailureMessage::PackNotFound;
		case Api::TelegramStickerPackDownloadError::BotApi:
			return detail.isEmpty()
					   ? TelegramDownloadFailureMessage::InvalidResponse
					   : TelegramDownloadFailureMessage::ApiError;
		case Api::TelegramStickerPackDownloadError::Network:
			return detail.isEmpty()
					   ? TelegramDownloadFailureMessage::NetworkError
					   : TelegramDownloadFailureMessage::ApiError;
		case Api::TelegramStickerPackDownloadError::InvalidResponse:
			return TelegramDownloadFailureMessage::InvalidResponse;
		case Api::TelegramStickerPackDownloadError::UnsafeFilePath:
			return TelegramDownloadFailureMessage::UnsafeFilePath;
		case Api::TelegramStickerPackDownloadError::TooManyFiles:
			return TelegramDownloadFailureMessage::TooManyFiles;
		case Api::TelegramStickerPackDownloadError::FileTooLarge:
			return TelegramDownloadFailureMessage::FileTooLarge;
		case Api::TelegramStickerPackDownloadError::TotalTooLarge:
			return TelegramDownloadFailureMessage::TotalTooLarge;
		case Api::TelegramStickerPackDownloadError::UnsupportedFormat:
			return TelegramDownloadFailureMessage::UnsupportedFormat;
		}
		Unexpected("Telegram sticker download error.");
	}();
	return {
		.message = message,
		.line = TelegramDownloadFailureLine::Status,
		.color = TelegramDownloadFailureColor::Error,
	};
}

} // namespace Ui::StickerImport
