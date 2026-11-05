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

namespace Main {
class Session;
} // namespace Main

namespace Api {

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
	void uploadNext();
	void uploadReady(const MTPInputFile &file);
	void uploadFailed();
	void uploadProgressed();
	void createSet();
	void fail(QString error);

	const not_null<Main::Session *> _session;
	Core::StickerImportScan _scan;
	const QString _title;
	const QString _shortName;
	const Data::StickersType _type;
	const Core::StickerImportSource _source;
	const QString _telegramPackName;
	MTP::Sender _api;
	rpl::lifetime _uploadLifetime;
	FullMsgId _uploadId;
	DocumentId _documentId = 0;
	mtpRequestId _requestId = 0;
	std::vector<MTPInputStickerSetItem> _items;
	int _index = 0;
	int _lastReportedPercent = -1;
	Fn<void(MTPmessages_StickerSet)> _done;
	Fn<void(QString)> _fail;
	Fn<void(int)> _progress;
};

} // namespace Api
