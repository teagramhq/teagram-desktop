/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sticker_set_import_requests.h"

#include "core/telegram_sticker_import.h"

#include <utility>

namespace Api::details {

StickerSetImportFlow::StickerSetImportFlow(
	Core::StickerImportScan scan, QString title, QString shortName,
	Data::StickersType type, Core::StickerImportSource source,
	QString telegramPackName, StickerSetImportFlowCallbacks callbacks)
	: _scan(std::move(scan)), _title(std::move(title)),
	  _shortName(std::move(shortName)), _type(type), _source(source),
	  _telegramPackName(std::move(telegramPackName)),
	  _callbacks(std::move(callbacks)) {
	Expects(!!_callbacks.upload);
	Expects(!!_callbacks.uploadMedia);
	Expects(!!_callbacks.createSet);
}

void StickerSetImportFlow::start(Fn<void(MTPmessages_StickerSet)> done,
								 Fn<void(QString)> failCallback,
								 Fn<void(int)> progress) {
	Expects(!_done && !_fail && !_cancelled && !_finished);
	if (_scan.files.empty()) {
		if (failCallback) {
			failCallback(u"Choose a pack with supported sticker files."_q);
		}
		return;
	}
	_done = std::move(done);
	_fail = std::move(failCallback);
	_progress = std::move(progress);
	_items.reserve(_scan.files.size());
	uploadNext();
}

void StickerSetImportFlow::cancel() {
	_cancelled = true;
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
}

void StickerSetImportFlow::uploadNext() {
	if (_cancelled || _finished) {
		return;
	} else if (_index >= int(_scan.files.size())) {
		createSet();
		return;
	}
	const auto weak = base::make_weak(this);
	auto reportProgress = Fn<void(int)>();
	if (_progress) {
		reportProgress = [weak](int percent) {
			if (const auto self = weak.get();
				self && self->_progress
				&& percent != self->_lastReportedPercent) {
				self->_lastReportedPercent = percent;
				self->_progress(percent);
			}
		};
	}
	_callbacks.upload(
		_scan.files[_index], _index, int(_scan.files.size()),
		[weak](MTPInputFile file) {
			if (const auto self = weak.get()) {
				self->uploadReady(std::move(file));
			}
		},
		[weak] {
			if (const auto self = weak.get()) {
				self->fail(QString());
			}
		},
		std::move(reportProgress));
}

void StickerSetImportFlow::uploadReady(MTPInputFile file) {
	if (_cancelled || _finished) {
		return;
	}
	auto request
		= StickerImportUploadMediaRequest(file, _scan.files[_index], _index);
	const auto weak = base::make_weak(this);
	_callbacks.uploadMedia(
		std::move(request),
		[weak](MTPMessageMedia result) {
			if (const auto self = weak.get()) {
				self->uploadMediaReady(std::move(result));
			}
		},
		[weak](QString error) {
			if (const auto self = weak.get()) {
				self->fail(std::move(error));
			}
		});
}

void StickerSetImportFlow::uploadMediaReady(MTPMessageMedia result) {
	if (_cancelled || _finished) {
		return;
	}
	const auto item = StickerImportSetItemFromUploadMedia(
		result, _scan.files[_index].emoji);
	if (!item) {
		fail(u"STICKER_DOCUMENT_INVALID"_q);
		return;
	}
	_items.push_back(*item);
	++_index;
	if (_progress) {
		const auto percent = (_index * 100) / int(_scan.files.size());
		if (percent != _lastReportedPercent) {
			_lastReportedPercent = percent;
			_progress(percent);
		}
	}
	uploadNext();
}

void StickerSetImportFlow::createSet() {
	auto items = QVector<MTPInputStickerSetItem>();
	items.reserve(int(_items.size()));
	for (const auto &item : _items) {
		items.push_back(item);
	}
	auto request
		= StickerImportCreateSetRequest(_title, _shortName, _type, _source,
										std::move(items), _telegramPackName);
	const auto weak = base::make_weak(this);
	_callbacks.createSet(
		std::move(request),
		[weak](MTPmessages_StickerSet result) {
			if (const auto self = weak.get()) {
				self->createSetReady(std::move(result));
			}
		},
		[weak](QString error) {
			if (const auto self = weak.get()) {
				self->fail(std::move(error));
			}
		});
}

void StickerSetImportFlow::createSetReady(MTPmessages_StickerSet result) {
	if (_cancelled || _finished) {
		return;
	}
	_finished = true;
	_fail = nullptr;
	_progress = nullptr;
	const auto done = std::move(_done);
	if (done) {
		done(std::move(result));
	}
}

void StickerSetImportFlow::fail(QString error) {
	if (_cancelled || _finished) {
		return;
	}
	_finished = true;
	_done = nullptr;
	_progress = nullptr;
	const auto failCallback = std::move(_fail);
	if (failCallback) {
		failCallback(std::move(error));
	}
}

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
		MTP_flags(0), MTP_string(file.emoji.isEmpty() ? u"⭐"_q : file.emoji),
		MTP_inputStickerSetEmpty(), MTPMaskCoords()));
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
StickerImportSetItemFromUploadMedia(const MTPMessageMedia &media,
									QString emoji) {
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
	return MTP_inputStickerSetItem(
		MTP_flags(0), std::move(document),
		MTP_string(emoji.isEmpty() ? u"⭐"_q : emoji), MTPMaskCoords(),
		MTPstring());
}

MTPstickers_CreateStickerSet StickerImportCreateSetRequest(
	QString title, QString shortName, Data::StickersType type,
	Core::StickerImportSource source, QVector<MTPInputStickerSetItem> items,
	QString telegramPackName) {
	const auto sourceText = (source == Core::StickerImportSource::Folder)
								? u"folder"_q
							: (source == Core::StickerImportSource::Zip)
								? u"zip"_q
								: Core::TelegramStickerPackCanonicalUrl(
									  (type == Data::StickersType::Emoji)
										  ? Core::StickerImportType::Emoji
										  : Core::StickerImportType::Stickers,
									  telegramPackName);
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
