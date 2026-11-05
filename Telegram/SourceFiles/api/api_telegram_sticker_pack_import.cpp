/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_telegram_sticker_pack_import.h"

#include "lang/lang_keys.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>

namespace Api {
namespace {

constexpr auto kBotApiTimeout = 30'000;
constexpr auto kMaximumBotApiResponseBytes = 1024 * 1024;
constexpr auto kMaximumStickerFileBytes = 512 * 1024;
constexpr auto kMaximumStickerPackBytes = quint64(110) * 1024 * 1024;

[[nodiscard]] bool BotApiResponseSucceeded(const QByteArray &response) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(response, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return false;
	}
	const auto ok = document.object().value(u"ok"_q);
	return ok.isBool() && ok.toBool();
}

} // namespace

TelegramStickerPackDownloader::TelegramStickerPackDownloader()
	: _network(std::make_unique<QNetworkAccessManager>()) {}

TelegramStickerPackDownloader::~TelegramStickerPackDownloader() { cancel(); }

void TelegramStickerPackDownloader::start(QString token,
										  Core::TelegramStickerPackLink link,
										  Done done, Fail fail,
										  Fn<void(int)> progress) {
	Expects(!_done && !_fail && !_reply);
	const auto parsed = Core::ParseTelegramStickerPackLink(link.sourceUrl);
	if (!details::IsValidTelegramBotToken(token)) {
		if (fail) {
			fail(tr::lng_teagram_sticker_import_bot_token_invalid(tr::now));
		}
		return;
	} else if (!parsed || parsed->name != link.name || parsed->type != link.type
			   || parsed->sourceUrl != link.sourceUrl) {
		if (fail) {
			fail(tr::lng_teagram_sticker_import_scan_invalid_source(tr::now));
		}
		return;
	}
	_token = std::move(token);
	_link = std::move(link);
	_done = std::move(done);
	_fail = std::move(fail);
	_progress = std::move(progress);
	requestJson(u"getStickerSet"_q, QJsonObject{{u"name"_q, _link.name}},
				[=](QByteArray response) { stickerSetReady(response); });
}

void TelegramStickerPackDownloader::cancel() {
	if (_reply) {
		const auto reply = _reply.data();
		QObject::disconnect(reply, nullptr, _network.get(), nullptr);
		_reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
	_response.clear();
	_responseDone = nullptr;
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
	_token.fill(QChar());
	_token.clear();
	_link = {};
	_title.clear();
	_stickers.clear();
	_files.clear();
	_totalBytes = 0;
	_index = 0;
	_responseLimit = 0;
	_jsonRequest = false;
}

void TelegramStickerPackDownloader::requestJson(QString method,
												QJsonObject parameters,
												Fn<void(QByteArray)> done) {
	const auto url = details::TelegramBotApiMethodUrl(_token, method);
	if (!url.isValid()) {
		fail(tr::lng_teagram_sticker_import_bot_token_invalid(tr::now));
		return;
	}
	auto request = QNetworkRequest(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
						 QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(kBotApiTimeout);
	request.setHeader(QNetworkRequest::ContentTypeHeader,
					  u"application/json"_q);
	_response.clear();
	_responseLimit = kMaximumBotApiResponseBytes;
	_jsonRequest = true;
	_responseDone = std::move(done);
	_reply = _network->post(
		request, QJsonDocument(parameters).toJson(QJsonDocument::Compact));
	const auto weak = base::make_weak(this);
	auto reply = _reply.data();
	QObject::connect(reply, &QNetworkReply::readyRead, _network.get(), [=] {
		if (const auto current = weak.get()) {
			current->readReplyData(kMaximumBotApiResponseBytes);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, _network.get(), [=] {
		if (const auto current = weak.get()) {
			current->replyFinished();
		}
	});
}

void TelegramStickerPackDownloader::requestFile(QUrl url) {
	auto request = QNetworkRequest(std::move(url));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
						 QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(kBotApiTimeout);
	_response.clear();
	_responseLimit = kMaximumStickerFileBytes;
	_jsonRequest = false;
	_responseDone = [=](QByteArray response) { fileReady(response); };
	_reply = _network->get(request);
	const auto weak = base::make_weak(this);
	auto reply = _reply.data();
	QObject::connect(reply, &QNetworkReply::readyRead, _network.get(), [=] {
		if (const auto current = weak.get()) {
			current->readReplyData(kMaximumStickerFileBytes);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, _network.get(), [=] {
		if (const auto current = weak.get()) {
			current->replyFinished();
		}
	});
}

void TelegramStickerPackDownloader::readReplyData(qint64 limit) {
	if (!_reply) {
		return;
	}
	const auto bytes = _reply->readAll();
	if (bytes.size() > limit - _response.size()) {
		fail(tr::lng_teagram_sticker_import_telegram_limit(tr::now));
		return;
	}
	_response.append(bytes);
}

void TelegramStickerPackDownloader::replyFinished() {
	if (!_reply) {
		return;
	}
	const auto reply = _reply.data();
	readReplyData(_responseLimit);
	if (!_reply) {
		return;
	}
	const auto url = reply->url();
	const auto redirect
		= reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
	const auto status
		= reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto error = reply->error();
	const auto json = _jsonRequest;
	const auto response = std::move(_response);
	const auto done = std::move(_responseDone);
	_reply = nullptr;
	_responseLimit = 0;
	QObject::disconnect(reply, nullptr, _network.get(), nullptr);
	reply->deleteLater();
	if (url.scheme() != u"https"_q || url.host() != u"api.telegram.org"_q) {
		fail(tr::lng_teagram_sticker_import_telegram_network_error(tr::now));
		return;
	} else if (redirect.isValid() || (status >= 300 && status < 400)) {
		fail(tr::lng_teagram_sticker_import_telegram_network_error(tr::now));
		return;
	} else if (status >= 400) {
		if (json && !response.isEmpty() && done) {
			done(response);
		} else {
			fail(
				tr::lng_teagram_sticker_import_telegram_network_error(tr::now));
		}
		return;
	} else if (error != QNetworkReply::NoError || status < 200
			   || status >= 300) {
		fail(tr::lng_teagram_sticker_import_telegram_network_error(tr::now));
		return;
	}
	if (done) {
		done(response);
	}
}

void TelegramStickerPackDownloader::stickerSetReady(
	const QByteArray &response) {
	if (!BotApiResponseSucceeded(response)) {
		failApiResponse(response);
		return;
	}
	const auto pack
		= details::ParseTelegramStickerSetResponse(response, _link.type);
	if (!pack) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
		return;
	}
	_title = pack->title;
	_stickers = pack->stickers;
	if (_link.type == Core::StickerImportType::Emoji) {
		auto ids = QJsonArray();
		for (const auto &sticker : _stickers) {
			ids.push_back(sticker.customEmojiId);
		}
		requestJson(
			u"getCustomEmojiStickers"_q,
			QJsonObject{{u"custom_emoji_ids"_q, ids}},
			[=](QByteArray result) { customEmojiStickersReady(result); });
	} else {
		downloadNext();
	}
}

void TelegramStickerPackDownloader::customEmojiStickersReady(
	const QByteArray &response) {
	if (!BotApiResponseSucceeded(response)) {
		failApiResponse(response);
		return;
	}
	const auto stickers = details::ParseTelegramCustomEmojiStickersResponse(
		response, _stickers);
	if (!stickers) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
		return;
	}
	_stickers = *stickers;
	downloadNext();
}

void TelegramStickerPackDownloader::fileInfoReady(const QByteArray &response) {
	if (!BotApiResponseSucceeded(response)) {
		failApiResponse(response);
		return;
	}
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(response, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
		return;
	}
	const auto result = document.object().value(u"result"_q);
	if (!result.isObject()) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
		return;
	}
	const auto file = result.toObject();
	const auto path = file.value(u"file_path"_q);
	const auto size = file.value(u"file_size"_q);
	if (!path.isString()
		|| (size.isDouble()
			&& (size.toDouble() < 0
				|| size.toDouble() > kMaximumStickerFileBytes))) {
		fail(tr::lng_teagram_sticker_import_telegram_limit(tr::now));
		return;
	}
	const auto url = details::TelegramBotApiFileUrl(_token, path.toString());
	if (!url) {
		fail(tr::lng_teagram_sticker_import_telegram_file_error(tr::now));
		return;
	}
	requestFile(*url);
}

void TelegramStickerPackDownloader::fileReady(const QByteArray &response) {
	if (_index < 0 || _index >= int(_stickers.size()) || response.isEmpty()) {
		fail(tr::lng_teagram_sticker_import_telegram_file_error(tr::now));
		return;
	} else if (quint64(response.size())
			   > kMaximumStickerPackBytes - _totalBytes) {
		fail(tr::lng_teagram_sticker_import_telegram_limit(tr::now));
		return;
	}
	const auto &sticker = _stickers[_index];
	const auto format = Core::StickerImportFormatDetails(sticker.format);
	_files.push_back({
		.name = u"telegram_%1.%2"_q.arg(_index + 1).arg(format.extension),
		.bytes = response,
		.format = sticker.format,
		.emoji = sticker.emoji,
	});
	_totalBytes += quint64(response.size());
	++_index;
	if (_progress) {
		_progress((_index * 100) / int(_stickers.size()));
	}
	downloadNext();
}

void TelegramStickerPackDownloader::downloadNext() {
	if (_index >= int(_stickers.size())) {
		finish();
		return;
	}
	requestJson(u"getFile"_q,
				QJsonObject{{u"file_id"_q, _stickers[_index].fileId}},
				[=](QByteArray response) { fileInfoReady(response); });
}

void TelegramStickerPackDownloader::finish() {
	const auto scan
		= Core::ScanTelegramStickerImportFiles(std::move(_files), _link.type);
	const auto done = std::move(_done);
	const auto title = _title;
	const auto sourceUrl = _link.sourceUrl;
	cancel();
	if (done) {
		done(scan, title, sourceUrl);
	}
}

void TelegramStickerPackDownloader::failApiResponse(
	const QByteArray &response) {
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(response, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
		return;
	}
	const auto object = document.object();
	const auto code = object.value(u"error_code"_q).toInt();
	const auto description = object.value(u"description"_q).toString();
	if (code == 401) {
		fail(tr::lng_teagram_sticker_import_bot_token_wrong(tr::now));
	} else if (description.contains(u"STICKERSET_INVALID"_q,
									Qt::CaseInsensitive)) {
		fail(tr::lng_teagram_sticker_import_telegram_pack_missing(tr::now));
	} else if (!description.isEmpty()) {
		fail(tr::lng_teagram_sticker_import_telegram_api_error(
			tr::now, lt_error,
			details::RedactTelegramBotApiError(description, _token)));
	} else {
		fail(tr::lng_teagram_sticker_import_telegram_pack_error(tr::now));
	}
}

void TelegramStickerPackDownloader::fail(QString message) {
	const auto failed = std::move(_fail);
	cancel();
	if (failed) {
		failed(std::move(message));
	}
}

} // namespace Api
