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
								   QString telegramPackName)
	: _session(session), _type(type), _api(&session->mtp()) {
	Expects(Core::IsValidStickerSetTitle(title));
	Expects(Core::IsValidStickerSetShortName(shortName));
	_flow = std::make_unique<details::StickerSetImportFlow>(
		std::move(scan), std::move(title), std::move(shortName), type, source,
		std::move(telegramPackName),
		details::StickerSetImportFlowCallbacks{
			.upload =
				[=](const Core::StickerImportFile &file, int index, int total,
					Fn<void(MTPInputFile)> done, Fn<void()> failed,
					Fn<void(int)> progress) {
					uploadFile(file, index, total, std::move(done),
							   std::move(failed), std::move(progress));
				},
			.uploadMedia =
				[=](MTPmessages_UploadMedia request,
					Fn<void(MTPMessageMedia)> done, Fn<void(QString)> failed) {
					sendUploadMedia(std::move(request), std::move(done),
									std::move(failed));
				},
			.createSet =
				[=](MTPstickers_CreateStickerSet request,
					Fn<void(MTPmessages_StickerSet)> done,
					Fn<void(QString)> failed) {
					sendCreateSet(std::move(request), std::move(done),
								  std::move(failed));
				},
		});
}

StickerSetImport::~StickerSetImport() { cancel(); }

void StickerSetImport::uploadFile(const Core::StickerImportFile &file,
								  int index, int total,
								  Fn<void(MTPInputFile)> done,
								  Fn<void()> failed, Fn<void(int)> progress) {
	_documentId = base::RandomValue<DocumentId>();
	_uploadLifetime = rpl::lifetime();
	auto prepared = PrepareStickerImportFile(_session->mtp().mainDcId(),
											 _documentId, file, index);
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
				_uploadLifetime.destroy();
				_uploadId = FullMsgId();
				done(data.info.file);
			},
			_uploadLifetime);

	_session->uploader().documentFailed()
		| rpl::filter([=](const FullMsgId &id) { return id == _uploadId; })
		| rpl::on_next(
			[=] {
				_uploadLifetime.destroy();
				failed();
			},
			_uploadLifetime);

	if (progress) {
		_session->uploader().documentProgress()
			| rpl::filter([=](const FullMsgId &id) { return id == _uploadId; })
			| rpl::on_next(
				[=] {
					const auto document
						= _session->data().document(_documentId);
					if (!document->uploading() || !document->uploadingData) {
						return;
					}
					const auto size = document->uploadingData->size;
					if (size <= 0) {
						return;
					}
					const auto percent
						= int(((index * 100)
							   + (document->uploadingData->offset * 100) / size)
							  / total);
					progress(percent);
				},
				_uploadLifetime);
	}
	_session->uploader().upload(_uploadId, prepared);
}

void StickerSetImport::sendUploadMedia(MTPmessages_UploadMedia request,
									   Fn<void(MTPMessageMedia)> done,
									   Fn<void(QString)> failed) {
	_requestId = _api.request(std::move(request))
					 .done(crl::guard(this,
									  [=](const MTPMessageMedia &result) {
										  _requestId = 0;
										  done(result);
									  }))
					 .fail(crl::guard(this,
									  [=](const MTP::Error &error) {
										  _requestId = 0;
										  failed(error.type());
									  }))
					 .handleFloodErrors()
					 .send();
}

void StickerSetImport::sendCreateSet(MTPstickers_CreateStickerSet request,
									 Fn<void(MTPmessages_StickerSet)> done,
									 Fn<void(QString)> failed) {
	_requestId
		= _api.request(std::move(request))
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
					  done(result);
				  }))
			  .fail(crl::guard(this,
							   [=](const MTP::Error &error) {
								   _requestId = 0;
								   failed(error.type());
							   }))
			  .handleFloodErrors()
			  .send();
}

void StickerSetImport::start(Fn<void(MTPmessages_StickerSet)> done,
							 Fn<void(QString)> fail, Fn<void(int)> progress) {
	Expects(!_requestId && !_uploadId);
	_flow->start(
		[=](MTPmessages_StickerSet result) {
			cancel();
			if (done) {
				done(std::move(result));
			}
		},
		[=](QString error) {
			cancel();
			if (fail) {
				fail(std::move(error));
			}
		},
		std::move(progress));
}

void StickerSetImport::cancel() {
	_flow->cancel();
	if (_uploadId) {
		_session->uploader().cancel(_uploadId);
		_uploadId = FullMsgId();
	}
	if (_requestId) {
		_api.request(_requestId).cancel();
		_requestId = 0;
	}
	_uploadLifetime.destroy();
}

} // namespace Api
