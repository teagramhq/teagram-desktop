/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_import_feedback.h"

#include "base/assertion.h"
#include "lang/lang_keys.h"

namespace Ui::StickerImport {

TelegramDownloadFailure TelegramDownloadFailureFeedback(
		Api::TelegramStickerPackDownloadError error, const QString &detail) {
	const auto message = [&] {
		switch (error) {
		case Api::TelegramStickerPackDownloadError::InvalidToken:
			return tr::lng_teagram_sticker_import_telegram_token_rejected(
				tr::now);
		case Api::TelegramStickerPackDownloadError::InvalidLink:
			return tr::lng_teagram_sticker_import_telegram_link_invalid(tr::now);
		case Api::TelegramStickerPackDownloadError::UnsupportedPackType:
			return tr::lng_teagram_sticker_import_telegram_type_mismatch(tr::now);
		case Api::TelegramStickerPackDownloadError::PackNotFound:
			return tr::lng_teagram_sticker_import_telegram_pack_missing(tr::now);
		case Api::TelegramStickerPackDownloadError::BotApi:
			return detail.isEmpty()
					   ? tr::lng_teagram_sticker_import_telegram_response_invalid(
							 tr::now)
					   : tr::lng_teagram_sticker_import_telegram_api_error(
							 tr::now, lt_error, detail);
		case Api::TelegramStickerPackDownloadError::Network:
			return detail.isEmpty()
					   ? tr::lng_teagram_sticker_import_telegram_network_error(
							 tr::now)
					   : tr::lng_teagram_sticker_import_telegram_api_error(
							 tr::now, lt_error, detail);
		case Api::TelegramStickerPackDownloadError::InvalidResponse:
			return tr::lng_teagram_sticker_import_telegram_response_invalid(
				tr::now);
		case Api::TelegramStickerPackDownloadError::UnsafeFilePath:
			return tr::lng_teagram_sticker_import_telegram_file_path_invalid(
				tr::now);
		case Api::TelegramStickerPackDownloadError::TooManyFiles:
			return tr::lng_teagram_sticker_import_telegram_too_many_files(
				tr::now);
		case Api::TelegramStickerPackDownloadError::FileTooLarge:
			return tr::lng_teagram_sticker_import_telegram_file_too_large(
				tr::now);
		case Api::TelegramStickerPackDownloadError::TotalTooLarge:
			return tr::lng_teagram_sticker_import_scan_total_size(tr::now);
		case Api::TelegramStickerPackDownloadError::UnsupportedFormat:
			return tr::lng_teagram_sticker_import_telegram_format_unsupported(
				tr::now);
		}
		Unexpected("Telegram sticker download error.");
	}();
	return {
		.message = message,
		.pending = false,
		.isError = true,
	};
}

} // namespace Ui::StickerImport
