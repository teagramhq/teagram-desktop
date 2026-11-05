/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sticker_set_import.h"
#include "api/api_sticker_set_import_requests.h"

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

[[nodiscard]] std::shared_ptr<FilePrepareResult>
PrepareStickerImportFile(MTP::DcId dcId, DocumentId id,
						 const Core::StickerImportFile &file, int index) {
	const auto filename = details::StickerImportFileName(file, index);
	const auto format = Core::StickerImportFormatDetails(file.format);
	auto attributes = details::StickerImportFileAttributes(file, index);

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

} // namespace

StickerSetImport::StickerSetImport(not_null<Main::Session *> session,
								   Core::StickerImportScan scan, QString title,
								   QString shortName, Data::StickersType type,
								   Core::StickerImportSource source,
								   QString sourceUrl)
	: _session(session), _scan(std::move(scan)), _title(std::move(title)),
	  _shortName(std::move(shortName)), _type(type), _source(source),
	  _sourceUrl(std::move(sourceUrl)), _api(&session->mtp()) {
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

	const auto &source = _scan.files[_index];
	_requestId
		= _api.request(details::StickerImportUploadMediaRequest(file, source,
																_index))
			  .done(crl::guard(
				  this,
				  [=](const MTPMessageMedia &result) {
					  _requestId = 0;
					  const auto item
						  = details::StickerImportSetItemFromUploadMedia(
							  result, source.emoji);
					  if (!item) {
						  fail(u"STICKER_DOCUMENT_INVALID"_q);
						  return;
					  }
					  _items.push_back(*item);
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
	auto items = QVector<MTPInputStickerSetItem>();
	items.reserve(int(_items.size()));
	for (const auto &item : _items) {
		items.push_back(item);
	}
	_requestId
		= _api.request(details::StickerImportCreateSetRequest(
						   _title, _shortName, _type, _source, std::move(items),
						   _sourceUrl))
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
