/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sticker_set_import.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "data/stickers/data_stickers.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"

namespace Api {
namespace {

[[nodiscard]] QString SafeUploadName(const Core::StickerImportFile &file,
									 int index) {
	const auto format = Core::StickerImportFormatDetails(file.format);
	return u"sticker_%1.%2"_q.arg(index + 1).arg(format.extension);
}

[[nodiscard]] std::shared_ptr<FilePrepareResult>
PrepareStickerImportFile(MTP::DcId dcId, DocumentId id,
						 const Core::StickerImportFile &file, int index) {
	const auto filename = SafeUploadName(file, index);
	const auto format = Core::StickerImportFormatDetails(file.format);
	auto attributes = QVector<MTPDocumentAttribute>(
		1, MTP_documentAttributeFilename(MTP_string(filename)));
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

	auto result = MakePreparedFile({
		.id = id,
		.type = SendMediaType::File,
	});
	result->filename = filename;
	result->filemime = format.mimeType;
	result->content = file.bytes;
	result->filesize = file.bytes.size();
	result->setFileData(file.bytes);
	result->document = MTP_document(
		MTP_flags(0), MTP_long(id), MTP_long(0), MTP_bytes(),
		MTP_int(base::unixtime::now()), MTP_string(format.mimeType),
		MTP_long(file.bytes.size()), MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(), MTP_int(dcId),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)));
	return result;
}

[[nodiscard]] MTPInputDocument UploadedDocument(const MTPMessageMedia &media) {
	auto result = MTPInputDocument();
	media.match(
		[&](const MTPDmessageMediaDocument &data) {
			if (const auto document = data.vdocument()) {
				document->match(
					[&](const MTPDdocument &d) {
						result = MTP_inputDocument(d.vid(), d.vaccess_hash(),
												   d.vfile_reference());
					},
					[](const auto &) {});
			}
		},
		[](const auto &) {});
	return result;
}

} // namespace

StickerSetImport::StickerSetImport(not_null<Main::Session *> session,
								   Core::StickerImportScan scan, QString title,
								   QString shortName, Data::StickersType type,
								   Core::StickerImportSource source)
	: _session(session), _scan(std::move(scan)), _title(std::move(title)),
	  _shortName(std::move(shortName)), _type(type), _source(source),
	  _api(&session->mtp()) {
	Expects(Core::IsValidStickerSetTitle(_title));
	Expects(Core::IsValidStickerSetShortName(_shortName));
}

StickerSetImport::~StickerSetImport() { cancel(); }

void StickerSetImport::start(Fn<void(MTPmessages_StickerSet)> done,
							 Fn<void(QString)> fail, Fn<void(int)> progress) {
	Expects(!_done && !_fail && !_requestId && !_uploadId);
	if (_scan.files.empty()) {
		if (fail) {
			fail(u"Choose a pack with supported sticker files."_q);
		}
		return;
	}
	_done = std::move(done);
	_fail = std::move(fail);
	_progress = std::move(progress);
	_items.reserve(_scan.files.size());
	uploadNext();
}

void StickerSetImport::cancel() {
	if (_uploadId) {
		_session->uploader().cancel(_uploadId);
		_uploadId = FullMsgId();
	}
	if (_requestId) {
		_api.request(_requestId).cancel();
		_requestId = 0;
	}
	_uploadLifetime.destroy();
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
}

void StickerSetImport::uploadNext() {
	if (_index >= int(_scan.files.size())) {
		createSet();
		return;
	}
	_documentId = base::RandomValue<DocumentId>();
	_uploadLifetime = rpl::lifetime();
	auto prepared = PrepareStickerImportFile(
		_session->mtp().mainDcId(), _documentId, _scan.files[_index], _index);
	_uploadId = FullMsgId(_session->userPeerId(),
						  _session->data().nextLocalMessageId());

	const auto document = _session->data().document(_documentId);
	document->uploadingData
		= std::make_unique<Data::UploadState>(prepared->filesize);

	_session->uploader().documentReady()
		| rpl::filter([=](const Storage::UploadedMedia &data) {
			  return data.fullId == _uploadId;
		  })
		| rpl::on_next(
			[=](const Storage::UploadedMedia &data) {
				uploadReady(data.info.file);
			},
			_uploadLifetime);

	_session->uploader().documentFailed()
		| rpl::filter([=](const FullMsgId &id) { return id == _uploadId; })
		| rpl::on_next([=] { uploadFailed(); }, _uploadLifetime);

	if (_progress) {
		_session->uploader().documentProgress()
			| rpl::filter([=](const FullMsgId &id) { return id == _uploadId; })
			| rpl::on_next([=] { uploadProgressed(); }, _uploadLifetime);
	}
	_session->uploader().upload(_uploadId, prepared);
}

void StickerSetImport::uploadReady(const MTPInputFile &file) {
	_uploadLifetime.destroy();
	_uploadId = FullMsgId();

	auto attributes = QVector<MTPDocumentAttribute>();
	const auto &source = _scan.files[_index];
	const auto filename = SafeUploadName(source, _index);
	const auto format = Core::StickerImportFormatDetails(source.format);
	attributes.push_back(MTP_documentAttributeFilename(MTP_string(filename)));
	if (!source.dimensions.isEmpty()) {
		attributes.push_back(MTP_documentAttributeImageSize(
			MTP_int(source.dimensions.width()),
			MTP_int(source.dimensions.height())));
	}
	if (source.format == Core::StickerImportFormat::Tgs) {
		attributes.push_back(MTP_documentAttributeAnimated());
	} else if (source.format == Core::StickerImportFormat::Webm) {
		attributes.push_back(MTP_documentAttributeVideo(
			MTP_flags(0), MTP_double(0.), MTP_int(0), MTP_int(0), MTPint(),
			MTPdouble(), MTPstring()));
	}
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0), MTP_string(u"⭐"_q), MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));

	const auto media = MTP_inputMediaUploadedDocument(
		MTP_flags(0), file, MTPInputFile(), MTP_string(format.mimeType),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)),
		MTP_vector<MTPInputDocument>(), MTPInputPhoto(), MTP_int(0),
		MTP_int(0));

	_requestId
		= _api.request(MTPmessages_UploadMedia(MTP_flags(0), MTPstring(),
											   MTP_inputPeerSelf(), media))
			  .done(crl::guard(
				  this,
				  [=](const MTPMessageMedia &result) {
					  _requestId = 0;
					  const auto document = UploadedDocument(result);
					  if (document.type() != mtpc_inputDocument) {
						  fail(u"STICKER_DOCUMENT_INVALID"_q);
						  return;
					  }
					  _items.push_back(MTP_inputStickerSetItem(
						  MTP_flags(0), document, MTP_string(u"⭐"_q),
						  MTPMaskCoords(), MTPstring()));
					  ++_index;
					  if (_progress) {
						  _progress((_index * 100) / int(_scan.files.size()));
					  }
					  uploadNext();
				  }))
			  .fail(crl::guard(this,
							   [=](const MTP::Error &error) {
								   _requestId = 0;
								   fail(error.type());
							   }))
			  .handleFloodErrors()
			  .send();
}

void StickerSetImport::uploadFailed() { fail(QString()); }

void StickerSetImport::uploadProgressed() {
	if (!_progress) {
		return;
	}
	const auto document = _session->data().document(_documentId);
	if (!document->uploading() || !document->uploadingData) {
		return;
	}
	const auto size = document->uploadingData->size;
	if (size <= 0) {
		return;
	}
	const auto percent
		= int(((_index * 100) + (document->uploadingData->offset * 100) / size)
			  / int(_scan.files.size()));
	if (percent != _lastReportedPercent) {
		_lastReportedPercent = percent;
		_progress(percent);
	}
}

void StickerSetImport::createSet() {
	const auto source = (_source == Core::StickerImportSource::Folder)
							? u"folder"_q
							: u"zip"_q;
	auto flags = MTPstickers_CreateStickerSet::Flags(
		MTPstickers_CreateStickerSet::Flag::f_software);
	if (_type == Data::StickersType::Emoji) {
		flags |= MTPstickers_CreateStickerSet::Flag::f_emojis;
	}
	auto items = QVector<MTPInputStickerSetItem>();
	items.reserve(int(_items.size()));
	for (const auto &item : _items) {
		items.push_back(item);
	}
	_requestId
		= _api
			  .request(MTPstickers_CreateStickerSet(
				  MTP_flags(flags), MTP_inputUserSelf(), MTP_string(_title),
				  MTP_string(_shortName), MTPInputDocument(),
				  MTP_vector<MTPInputStickerSetItem>(std::move(items)),
				  MTP_string(source)))
			  .done(crl::guard(
				  this,
				  [=](const MTPmessages_StickerSet &result) {
					  _requestId = 0;
					  result.match(
						  [&](const MTPDmessages_stickerSet &data) {
							  _session->data().stickers().feedSetFull(data);
							  _session->data().stickers().notifyUpdated(_type);
						  },
						  [](const auto &) {});
					  const auto done = std::move(_done);
					  cancel();
					  if (done) {
						  done(result);
					  }
				  }))
			  .fail(crl::guard(this,
							   [=](const MTP::Error &error) {
								   _requestId = 0;
								   fail(error.type());
							   }))
			  .handleFloodErrors()
			  .send();
}

void StickerSetImport::fail(QString error) {
	const auto failed = std::move(_fail);
	cancel();
	if (failed) {
		failed(std::move(error));
	}
}

} // namespace Api
