/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_import_feedback.h"

#include "base/assertion.h"

#include <utility>

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

Api::TelegramStickerPackDownloader::Fail MakeTelegramDownloadFailureCallback(
	Core::StickerImportScan &scan, bool &scanPending,
	TelegramDownloadFailureIsAlive isAlive,
	TelegramDownloadFailureSetPending setPending,
	TelegramDownloadFailureFormatText formatText,
	TelegramDownloadFailureSetText setStatusText,
	TelegramDownloadFailureSetText setErrorText) {
	const auto callback =
		[&scan, &scanPending, isAlive = std::move(isAlive),
		 setPending = std::move(setPending), formatText = std::move(formatText),
		 setStatusText = std::move(setStatusText),
		 setErrorText = std::move(setErrorText)](
			Api::TelegramStickerPackDownloadError error, QString detail) {
			if (!isAlive()) {
				return;
			}
			const auto feedback
				= TelegramDownloadFailureFeedback(error, detail, scanPending);
			scan = {};
			setPending(scanPending);
			const auto &setText
				= (feedback.line == TelegramDownloadFailureLine::Status)
					  ? setStatusText
					  : setErrorText;
			setText(feedback.color, formatText(feedback.message, detail));
		};
	return callback;
}

} // namespace Ui::StickerImport
