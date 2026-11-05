/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_telegram_sticker_pack_import_requests.h"
#include "base/weak_ptr.h"
#include "core/sticker_import.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <memory>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace Api {

class TelegramStickerPackDownloader final : public base::has_weak_ptr {
  public:
	using Done = Fn<void(Core::StickerImportScan, QString, QString)>;
	using Fail = Fn<void(QString)>;

	TelegramStickerPackDownloader();
	~TelegramStickerPackDownloader();

	void start(QString token, Core::TelegramStickerPackLink link, Done done,
			   Fail fail, Fn<void(int)> progress = nullptr);
	void cancel();

  private:
	void requestJson(QString method, QJsonObject parameters,
					 Fn<void(QByteArray)> done);
	void requestFile(QUrl url);
	void readReplyData(qint64 limit);
	void replyFinished();
	void stickerSetReady(const QByteArray &response);
	void customEmojiStickersReady(const QByteArray &response);
	void fileInfoReady(const QByteArray &response);
	void fileReady(const QByteArray &response);
	void downloadNext();
	void finish();
	void failApiResponse(const QByteArray &response);
	void fail(QString message);

	std::unique_ptr<QNetworkAccessManager> _network;
	QPointer<QNetworkReply> _reply;
	QByteArray _response;
	qint64 _responseLimit = 0;
	bool _jsonRequest = false;
	QString _token;
	Core::TelegramStickerPackLink _link;
	QString _title;
	std::vector<details::TelegramStickerInfo> _stickers;
	std::vector<Core::StickerImportFile> _files;
	quint64 _totalBytes = 0;
	int _index = 0;
	Fn<void(QByteArray)> _responseDone;
	Done _done;
	Fail _fail;
	Fn<void(int)> _progress;
};

} // namespace Api
