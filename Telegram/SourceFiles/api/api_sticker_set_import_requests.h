/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/sticker_import.h"
#include "data/stickers/data_stickers.h"
#include "mtproto/sender.h"

#include <optional>

namespace Api::details {

[[nodiscard]] QString StickerImportFileName(const Core::StickerImportFile &file,
											int index);
[[nodiscard]] QVector<MTPDocumentAttribute>
StickerImportFileAttributes(const Core::StickerImportFile &file, int index);
[[nodiscard]] MTPmessages_UploadMedia
StickerImportUploadMediaRequest(const MTPInputFile &inputFile,
								const Core::StickerImportFile &file, int index);
[[nodiscard]] std::optional<MTPInputStickerSetItem>
StickerImportSetItemFromUploadMedia(const MTPMessageMedia &media,
									QString emoji = QString());
[[nodiscard]] MTPstickers_CreateStickerSet StickerImportCreateSetRequest(
	QString title, QString shortName, Data::StickersType type,
	Core::StickerImportSource source, QVector<MTPInputStickerSetItem> items,
	QString sourceUrl = QString());
[[nodiscard]] MTPmessages_InstallStickerSet
StickerImportInstallSetRequest(MTPInputStickerSet set);

} // namespace Api::details
