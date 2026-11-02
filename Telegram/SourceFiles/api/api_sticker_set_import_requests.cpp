/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sticker_set_import_requests.h"

#include <utility>

namespace Api::details {

QString StickerImportFileName(const Core::StickerImportFile &file, int index) {
	const auto format = Core::StickerImportFormatDetails(file.format);
	return u"sticker_%1.%2"_q.arg(index + 1).arg(format.extension);
}

QVector<MTPDocumentAttribute>
StickerImportFileAttributes(const Core::StickerImportFile &file, int index) {
	auto attributes = QVector<MTPDocumentAttribute>(
		1, MTP_documentAttributeFilename(
			   MTP_string(StickerImportFileName(file, index))));
	if (!file.dimensions.isEmpty()) {
		attributes.push_back(
			MTP_documentAttributeImageSize(MTP_int(file.dimensions.width()),
										   MTP_int(file.dimensions.height())));
	}
	if (file.format == Core::StickerImportFormat::Tgs) {
		attributes.push_back(MTP_documentAttributeAnimated());
	} else if (file.format == Core::StickerImportFormat::Webm) {
		attributes.push_back(MTP_documentAttributeVideo(
			MTP_flags(0), MTP_double(0.), MTP_int(0), MTP_int(0), MTPint(),
			MTPdouble(), MTPstring()));
	}
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0), MTP_string(u"⭐"_q), MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));
	return attributes;
}

MTPmessages_UploadMedia
StickerImportUploadMediaRequest(const MTPInputFile &inputFile,
								const Core::StickerImportFile &file,
								int index) {
	const auto format = Core::StickerImportFormatDetails(file.format);
	const auto media = MTP_inputMediaUploadedDocument(
		MTP_flags(0), inputFile, MTPInputFile(), MTP_string(format.mimeType),
		MTP_vector<MTPDocumentAttribute>(
			StickerImportFileAttributes(file, index)),
		MTP_vector<MTPInputDocument>(), MTPInputPhoto(), MTP_int(0),
		MTP_int(0));
	return MTPmessages_UploadMedia(MTP_flags(0), MTPstring(),
								   MTP_inputPeerSelf(), media);
}

std::optional<MTPInputStickerSetItem>
StickerImportSetItemFromUploadMedia(const MTPMessageMedia &media) {
	auto document = MTPInputDocument();
	media.match(
		[&](const MTPDmessageMediaDocument &data) {
			if (const auto value = data.vdocument()) {
				value->match(
					[&](const MTPDdocument &d) {
						document = MTP_inputDocument(d.vid(), d.vaccess_hash(),
													 d.vfile_reference());
					},
					[](const auto &) {});
			}
		},
		[](const auto &) {});
	if (document.type() != mtpc_inputDocument) {
		return std::nullopt;
	}
	return MTP_inputStickerSetItem(MTP_flags(0), std::move(document),
								   MTP_string(u"⭐"_q), MTPMaskCoords(),
								   MTPstring());
}

MTPstickers_CreateStickerSet StickerImportCreateSetRequest(
	QString title, QString shortName, Data::StickersType type,
	Core::StickerImportSource source, QVector<MTPInputStickerSetItem> items) {
	const auto sourceText = (source == Core::StickerImportSource::Folder)
								? u"folder"_q
								: u"zip"_q;
	auto flags = MTPstickers_CreateStickerSet::Flags(
		MTPstickers_CreateStickerSet::Flag::f_software);
	if (type == Data::StickersType::Emoji) {
		flags |= MTPstickers_CreateStickerSet::Flag::f_emojis;
	}
	return MTPstickers_CreateStickerSet(
		MTP_flags(flags), MTP_inputUserSelf(), MTP_string(title),
		MTP_string(shortName), MTPInputDocument(),
		MTP_vector<MTPInputStickerSetItem>(std::move(items)),
		MTP_string(sourceText));
}

MTPmessages_InstallStickerSet
StickerImportInstallSetRequest(MTPInputStickerSet set) {
	return MTPmessages_InstallStickerSet(std::move(set), MTP_bool(false));
}

} // namespace Api::details
