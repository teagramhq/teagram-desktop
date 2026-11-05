/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/weak_ptr.h"
#include "core/telegram_sticker_import.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtNetwork/QNetworkAccessManager>

class QNetworkReply;

namespace Api {

enum class TelegramStickerPackDownloadError {
	InvalidToken,
	InvalidLink,
	PackNotFound,
	BotApi,
	Network,
	InvalidResponse,
	UnsafeFilePath,
	TooManyFiles,
	FileTooLarge,
	UnsupportedFormat,
};

class TelegramStickerPackDownloader final : public QObject,
											public base::has_weak_ptr {
  public:
	using Done = Fn<void(QString, Core::StickerImportScan)>;
	using Fail = Fn<void(TelegramStickerPackDownloadError, QString)>;
	using Progress = Fn<void(int, int)>;

	TelegramStickerPackDownloader(Core::TelegramStickerPackLink link,
								  QString token,
								  QNetworkAccessManager *network = nullptr);
	~TelegramStickerPackDownloader();

	void start(Done done, Fail fail, Progress progress);
	void cancel();

  private:
	struct Sticker {
		QString fileId;
		QString customEmojiId;
		QString emoji;
	};

	using Query = QVector<QPair<QString, QString>>;

	void requestJson(QString method, Query query, Fn<void(QJsonObject)> done);
	void requestBytes(QUrl url, quint64 maximumBytes,
					  TelegramStickerPackDownloadError tooLarge,
					  bool jsonResponse, Fn<void(QByteArray)> done);
	void handleApiResponse(QByteArray bytes, Fn<void(QJsonObject)> done);
	void handleStickerSet(QJsonObject set);
	void handleCustomEmojiStickers(QJsonObject result);
	void downloadNextSticker();
	void downloadStickerFile(QJsonObject file);
	void fail(TelegramStickerPackDownloadError error, QString detail = {});

	Core::TelegramStickerPackLink _link;
	QString _token;
	QNetworkAccessManager *_network = nullptr;
	QPointer<QNetworkReply> _reply;
	Done _done;
	Fail _fail;
	Progress _progress;
	QString _title;
	std::vector<Sticker> _stickers;
	std::vector<Core::StickerImportFile> _files;
	int _index = 0;
	quint64 _totalDownloaded = 0;
	bool _started = false;
	bool _finished = false;
	bool _cancelled = false;
};

} // namespace Api
