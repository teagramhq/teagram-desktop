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

#include <optional>
#include <vector>

namespace Api::details {

struct StickerSetImportFlowCallbacks {
	Fn<void(const Core::StickerImportFile &, int, int, Fn<void(MTPInputFile)>,
			Fn<void()>, Fn<void(int)>)>
		upload;
	Fn<void(MTPmessages_UploadMedia, Fn<void(MTPMessageMedia)>,
			Fn<void(QString)>)>
		uploadMedia;
	Fn<void(MTPstickers_CreateStickerSet, Fn<void(MTPmessages_StickerSet)>,
			Fn<void(QString)>)>
		createSet;
};

class StickerSetImportFlow final : public base::has_weak_ptr {
  public:
	StickerSetImportFlow(Core::StickerImportScan scan, QString title,
						 QString shortName, Data::StickersType type,
						 Core::StickerImportSource source,
						 QString telegramPackName,
						 StickerSetImportFlowCallbacks callbacks);

	void start(Fn<void(MTPmessages_StickerSet)> done, Fn<void(QString)> fail,
			   Fn<void(int)> progress = nullptr);
	void cancel();

  private:
	enum class State { Idle, Running, Finished, Cancelled };
	void transition(State state);
	void uploadNext();
	void uploadReady(MTPInputFile file);
	void uploadMediaReady(MTPMessageMedia result);
	void createSet();
	void createSetReady(MTPmessages_StickerSet result);
	void fail(QString error);

	Core::StickerImportScan _scan;
	const QString _title;
	const QString _shortName;
	const Data::StickersType _type;
	const Core::StickerImportSource _source;
	const QString _telegramPackName;
	StickerSetImportFlowCallbacks _callbacks;
	std::vector<MTPInputStickerSetItem> _items;
	int _index = 0;
	int _lastReportedPercent = -1;
	Fn<void(MTPmessages_StickerSet)> _done;
	Fn<void(QString)> _fail;
	Fn<void(int)> _progress;
	State _state = State::Idle;
};

[[nodiscard]] QString StickerImportFileName(const Core::StickerImportFile &file,
											int index);
[[nodiscard]] QVector<MTPDocumentAttribute>
StickerImportFileAttributes(const Core::StickerImportFile &file, int index);
[[nodiscard]] MTPmessages_UploadMedia
StickerImportUploadMediaRequest(const MTPInputFile &inputFile,
								const Core::StickerImportFile &file, int index);
[[nodiscard]] std::optional<MTPInputStickerSetItem>
StickerImportSetItemFromUploadMedia(const MTPMessageMedia &media,
									QString emoji = QStringLiteral("⭐"));
[[nodiscard]] MTPstickers_CreateStickerSet StickerImportCreateSetRequest(
	QString title, QString shortName, Data::StickersType type,
	Core::StickerImportSource source, QVector<MTPInputStickerSetItem> items,
	QString telegramPackName = {});
[[nodiscard]] MTPmessages_InstallStickerSet
StickerImportInstallSetRequest(MTPInputStickerSet set);

} // namespace Api::details
