/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "core/sticker_import.h"
#include "data/stickers/data_stickers.h"
#include "mtproto/sender.h"

#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Api {

namespace details {
class StickerSetImportFlow;
} // namespace details

class StickerSetImport final : public base::has_weak_ptr {
  public:
	StickerSetImport(not_null<Main::Session *> session,
					 Core::StickerImportScan scan, QString title,
					 QString shortName, Data::StickersType type,
					 Core::StickerImportSource source,
					 QString telegramPackName = {});
	~StickerSetImport();

	void start(Fn<void(MTPmessages_StickerSet)> done, Fn<void(QString)> fail,
			   Fn<void(int)> progress = nullptr);
	void cancel();

  private:
	void uploadFile(const Core::StickerImportFile &file, int index, int total,
					Fn<void(MTPInputFile)> done, Fn<void()> fail,
					Fn<void(int)> progress);
	void sendUploadMedia(MTPmessages_UploadMedia request,
						 Fn<void(MTPMessageMedia)> done,
						 Fn<void(QString)> fail);
	void sendCreateSet(MTPstickers_CreateStickerSet request,
					   Fn<void(MTPmessages_StickerSet)> done,
					   Fn<void(QString)> fail);

	const not_null<Main::Session *> _session;
	const Data::StickersType _type;
	std::unique_ptr<details::StickerSetImportFlow> _flow;
	MTP::Sender _api;
	rpl::lifetime _uploadLifetime;
	FullMsgId _uploadId;
	DocumentId _documentId = 0;
	mtpRequestId _requestId = 0;
};

} // namespace Api
