/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_telegram_sticker_pack.h"

#include "core/sticker_import.h"
#include "logs.h"

#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <memory>
#include <utility>

namespace Api {
namespace {

constexpr auto kMaximumApiResponseBytes = quint64(4) * 1024 * 1024;
constexpr auto kMaximumStickerFiles = 120;
constexpr auto kMaximumEmojiFiles = 200;
constexpr auto kMaximumDownloadedBytes = quint64(110) * 1024 * 1024;
constexpr auto kTransferTimeout = 30'000;

[[nodiscard]] int MaximumItems(Core::StickerImportType type) {
	return (type == Core::StickerImportType::Emoji) ? kMaximumEmojiFiles
													: kMaximumStickerFiles;
}

[[nodiscard]] std::optional<Core::StickerImportFormat>
FormatForPath(const QString &path) {
	const auto suffix = QFileInfo(path).suffix().toLower();
	if (suffix == u"webp"_q) {
		return Core::StickerImportFormat::Webp;
	} else if (suffix == u"tgs"_q) {
		return Core::StickerImportFormat::Tgs;
	} else if (suffix == u"webm"_q) {
		return Core::StickerImportFormat::Webm;
	}
	return std::nullopt;
}

[[nodiscard]] QString JsonString(const QJsonObject &object,
								 const QString &key) {
	const auto value = object.value(key);
	return value.isString() ? value.toString() : QString();
}

} // namespace

TelegramStickerPackDownloader::TelegramStickerPackDownloader(
	Core::TelegramStickerPackLink link, QString token,
	QNetworkAccessManager *network)
	: _link(std::move(link)), _token(std::move(token)),
	  _network(network ? network : new QNetworkAccessManager(this)) {}

TelegramStickerPackDownloader::~TelegramStickerPackDownloader() { cancel(); }

void TelegramStickerPackDownloader::start(Done done, Fail failCallback,
										  Progress progress) {
	Expects(!_started);
	_started = true;
	_done = std::move(done);
	_fail = std::move(failCallback);
	_progress = std::move(progress);
	if (!Core::IsValidTelegramStickerBotToken(_token)) {
		fail(TelegramStickerPackDownloadError::InvalidToken);
		return;
	}
	const auto parsed = Core::ParseTelegramStickerPackLink(_link.canonicalUrl);
	if (!parsed || parsed->name != _link.name || parsed->type != _link.type) {
		fail(TelegramStickerPackDownloadError::InvalidLink);
		return;
	}
	requestJson(
		u"getStickerSet"_q, {{u"name"_q, _link.name}},
		[=](QJsonObject result) { handleStickerSet(std::move(result)); });
}

void TelegramStickerPackDownloader::cancel() {
	if (_finished || _cancelled) {
		return;
	}
	_cancelled = true;
	_finished = true;
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
	_files.clear();
	_stickers.clear();
	_title.clear();
	_token.fill(QChar(0));
	_token.clear();
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
}

void TelegramStickerPackDownloader::requestJson(QString method, Query query,
												Fn<void(QJsonObject)> done) {
	const auto maybeUrl = Core::TelegramStickerBotApiUrl(_token, method);
	if (!maybeUrl) {
		fail(TelegramStickerPackDownloadError::InvalidToken);
		return;
	}
	auto url = *maybeUrl;
	if (!query.isEmpty()) {
		auto encoded = QUrlQuery();
		for (const auto &item : query) {
			encoded.addQueryItem(item.first, item.second);
		}
		url.setQuery(encoded);
	}
	requestBytes(std::move(url), kMaximumApiResponseBytes,
				 TelegramStickerPackDownloadError::InvalidResponse, true,
				 [=](QByteArray bytes) mutable {
					 handleApiResponse(std::move(bytes), std::move(done));
				 });
}

void TelegramStickerPackDownloader::requestBytes(
	QUrl url, quint64 maximumBytes, TelegramStickerPackDownloadError tooLarge,
	bool jsonResponse, Fn<void(QByteArray)> done) {
	if (_cancelled || _finished || _reply) {
		return;
	}
	if (url.scheme() != u"https"_q || url.host() != u"api.telegram.org"_q
		|| url.port(-1) != -1 || !url.userName().isEmpty()
		|| !url.password().isEmpty()) {
		fail(TelegramStickerPackDownloadError::InvalidLink);
		return;
	}
	auto request = QNetworkRequest(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
						 QNetworkRequest::ManualRedirectPolicy);
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
						 QNetworkRequest::AlwaysNetwork);
	request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
	request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
						 QNetworkRequest::Manual);
	request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
						 QNetworkRequest::Manual);
	request.setTransferTimeout(kTransferTimeout);
	request.setRawHeader("Accept-Encoding", "identity");
	request.setRawHeader("Accept", jsonResponse ? "application/json"
												: "application/octet-stream");
	auto response = std::make_shared<QByteArray>();
	auto reply = _network->get(request);
	_reply = reply;
	const auto readAvailable = [=, this] {
		if (_cancelled || _reply != reply) {
			return false;
		}
		const auto chunk = reply->readAll();
		if (quint64(response->size()) + quint64(chunk.size()) > maximumBytes) {
			fail(tooLarge);
			return false;
		}
		response->append(chunk);
		return true;
	};
	connect(reply, &QIODevice::readyRead, this, [=] { readAvailable(); });
	connect(reply, &QNetworkReply::metaDataChanged, this, [=] {
		if (_cancelled || _reply != reply) {
			return;
		}
		const auto length = reply->header(QNetworkRequest::ContentLengthHeader);
		if (length.isValid()) {
			bool ok = false;
			const auto value = length.toLongLong(&ok);
			if (ok && (value < 0 || quint64(value) > maximumBytes)) {
				fail(tooLarge);
			}
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=, this] {
		if (_cancelled || _reply != reply) {
			reply->deleteLater();
			return;
		}
		if (!readAvailable()) {
			return;
		}
		_reply = nullptr;
		const auto status
			= reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
				  .toInt();
		const auto responseUrl = reply->url();
		const auto redirected
			= reply->attribute(QNetworkRequest::RedirectionTargetAttribute)
				  .isValid();
		const auto safeOrigin
			= responseUrl.scheme() == u"https"_q
			  && responseUrl.host() == u"api.telegram.org"_q
			  && responseUrl.port(-1) == -1 && responseUrl.userName().isEmpty()
			  && responseUrl.password().isEmpty() && responseUrl == url;
		const auto networkError = reply->error();
		reply->deleteLater();
		if (!safeOrigin || redirected || (status >= 300 && status < 400)) {
			fail(TelegramStickerPackDownloadError::Network);
			return;
		} else if (jsonResponse && status >= 400) {
			done(std::move(*response));
			return;
		} else if (networkError != QNetworkReply::NoError || status < 200
				   || status >= 300) {
			fail(TelegramStickerPackDownloadError::Network);
			return;
		}
		done(std::move(*response));
	});
}

void TelegramStickerPackDownloader::handleApiResponse(
	QByteArray bytes, Fn<void(QJsonObject)> done) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		fail(TelegramStickerPackDownloadError::InvalidResponse);
		return;
	}
	const auto object = document.object();
	if (!object.value(u"ok"_q).toBool()) {
		const auto code = object.value(u"error_code"_q).toInt();
		const auto description = JsonString(object, u"description"_q);
		const auto folded = description.toLower();
		if (code == 401 || code == 404 || folded.contains(u"unauthorized"_q)) {
			fail(TelegramStickerPackDownloadError::InvalidToken);
		} else if (folded.contains(u"stickerset_invalid"_q)
				   || (folded.contains(u"sticker set"_q)
					   && folded.contains(u"not found"_q))) {
			fail(TelegramStickerPackDownloadError::PackNotFound);
		} else {
			fail(TelegramStickerPackDownloadError::BotApi,
				 Core::RedactTelegramStickerBotToken(description, _token));
		}
		return;
	}
	const auto result = object.value(u"result"_q);
	if (result.isObject()) {
		done(result.toObject());
	} else if (result.isArray()) {
		auto wrapped = QJsonObject();
		wrapped.insert(u"stickers"_q, result);
		done(std::move(wrapped));
	} else {
		fail(TelegramStickerPackDownloadError::InvalidResponse);
	}
}

void TelegramStickerPackDownloader::handleStickerSet(QJsonObject set) {
	const auto title = JsonString(set, u"title"_q);
	const auto array = set.value(u"stickers"_q);
	if (title.isEmpty() || !array.isArray()) {
		fail(TelegramStickerPackDownloadError::InvalidResponse);
		return;
	}
	const auto stickers = array.toArray();
	if (stickers.isEmpty()) {
		fail(TelegramStickerPackDownloadError::PackNotFound);
		return;
	} else if (stickers.size() > MaximumItems(_link.type)
			   || stickers.size() > 201) {
		fail(TelegramStickerPackDownloadError::TooManyFiles);
		return;
	}
	_title = Core::RedactTelegramStickerBotToken(title, _token);
	_stickers.reserve(size_t(stickers.size()));
	for (const auto &value : stickers) {
		if (!value.isObject()) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		const auto object = value.toObject();
		const auto fileId = JsonString(object, u"file_id"_q);
		const auto emoji = JsonString(object, u"emoji"_q);
		const auto customEmojiId = JsonString(object, u"custom_emoji_id"_q);
		if (fileId.isEmpty()
			|| (_link.type == Core::StickerImportType::Emoji
				&& customEmojiId.isEmpty())) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		_stickers.push_back({
			.fileId = fileId,
			.customEmojiId = customEmojiId,
			.emoji = Core::RedactTelegramStickerBotToken(emoji, _token),
		});
	}
	if (_link.type == Core::StickerImportType::Emoji) {
		auto ids = QJsonArray();
		for (const auto &sticker : _stickers) {
			ids.push_back(sticker.customEmojiId);
		}
		requestJson(u"getCustomEmojiStickers"_q,
					{{u"custom_emoji_ids"_q,
					  QString::fromUtf8(
						  QJsonDocument(ids).toJson(QJsonDocument::Compact))}},
					[=](QJsonObject result) {
						handleCustomEmojiStickers(std::move(result));
					});
	} else {
		downloadNextSticker();
	}
}

void TelegramStickerPackDownloader::handleCustomEmojiStickers(
	QJsonObject result) {
	const auto array = result.value(u"stickers"_q);
	if (!array.isArray()) {
		fail(TelegramStickerPackDownloadError::InvalidResponse);
		return;
	}
	auto byId = QHash<QString, QJsonObject>();
	for (const auto &value : array.toArray()) {
		if (!value.isObject()) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		const auto object = value.toObject();
		const auto id = JsonString(object, u"custom_emoji_id"_q);
		if (id.isEmpty()) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		byId.insert(id, object);
	}
	for (auto &sticker : _stickers) {
		const auto i = byId.constFind(sticker.customEmojiId);
		if (i == byId.cend()) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		const auto fileId = JsonString(*i, u"file_id"_q);
		if (fileId.isEmpty()) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		sticker.fileId = fileId;
		const auto emoji = JsonString(*i, u"emoji"_q);
		if (!emoji.isEmpty()) {
			sticker.emoji = Core::RedactTelegramStickerBotToken(emoji, _token);
		}
	}
	downloadNextSticker();
}

void TelegramStickerPackDownloader::downloadNextSticker() {
	if (_cancelled || _finished) {
		return;
	}
	if (_index >= int(_stickers.size())) {
		const auto weak = base::make_weak(this);
		const auto type = _link.type;
		_token.fill(QChar(0));
		_token.clear();
		crl::async([weak, files = std::move(_files), type]() mutable {
			auto scan = Core::ScanStickerImportFiles(std::move(files), type);
			crl::on_main(weak, [=, scan = std::move(scan)]() mutable {
				const auto self = weak.get();
				if (!self || self->_cancelled || self->_finished) {
					return;
				}
				self->_finished = true;
				self->_token.fill(QChar(0));
				self->_token.clear();
				self->_fail = nullptr;
				self->_progress = nullptr;
				const auto done = std::move(self->_done);
				if (done) {
					done(std::move(self->_title), std::move(scan));
				}
			});
		});
		return;
	}
	if (_progress) {
		_progress(_index, int(_stickers.size()));
	}
	const auto fileId = _stickers[size_t(_index)].fileId;
	requestJson(u"getFile"_q, {{u"file_id"_q, fileId}}, [=](QJsonObject file) {
		downloadStickerFile(std::move(file));
	});
}

void TelegramStickerPackDownloader::downloadStickerFile(QJsonObject file) {
	const auto path = JsonString(file, u"file_path"_q);
	if (!Core::IsValidTelegramStickerFilePath(path)) {
		fail(TelegramStickerPackDownloadError::UnsafeFilePath);
		return;
	}
	const auto format = FormatForPath(path);
	if (!format) {
		fail(TelegramStickerPackDownloadError::UnsupportedFormat);
		return;
	}
	const auto maximumFileBytes
		= Core::StickerImportFormatDetails(*format).maximumBytes;
	const auto declaredSize = file.value(u"file_size"_q);
	if (declaredSize.isDouble()) {
		const auto size = declaredSize.toDouble();
		if (size < 0) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		} else if (size > double(maximumFileBytes)) {
			fail(TelegramStickerPackDownloadError::FileTooLarge);
			return;
		}
	}
	if (_totalDownloaded >= kMaximumDownloadedBytes) {
		fail(TelegramStickerPackDownloadError::FileTooLarge);
		return;
	}
	const auto remaining = kMaximumDownloadedBytes - _totalDownloaded;
	const auto maximumBytes = std::min(maximumFileBytes, remaining);
	const auto url = Core::TelegramStickerBotFileUrl(_token, path);
	if (!url) {
		fail(TelegramStickerPackDownloadError::UnsafeFilePath);
		return;
	}
	requestBytes(*url, maximumBytes,
				 TelegramStickerPackDownloadError::FileTooLarge, false,
				 [=](QByteArray bytes) {
					 _totalDownloaded += quint64(bytes.size());
					 const auto suffix
						 = Core::StickerImportFormatDetails(*format).extension;
					 const auto name = u"telegram_%1.%2"_q
										   .arg(_index + 1, 3, 10, QChar('0'))
										   .arg(suffix);
					 const auto emoji = _stickers[size_t(_index)].emoji;
					 _files.push_back({
						 .name = name,
						 .bytes = std::move(bytes),
						 .format = *format,
						 .emoji = emoji.isEmpty() ? u"⭐"_q : emoji,
					 });
					 ++_index;
					 if (_progress) {
						 _progress(_index, int(_stickers.size()));
					 }
					 downloadNextSticker();
				 });
}

void TelegramStickerPackDownloader::fail(TelegramStickerPackDownloadError error,
										 QString detail) {
	if (_finished || _cancelled) {
		return;
	}
	_finished = true;
	_done = nullptr;
	_progress = nullptr;
	_files.clear();
	_stickers.clear();
	_title.clear();
	detail = Core::RedactTelegramStickerBotToken(std::move(detail), _token);
	LOG(("Telegram sticker pack download failed (%1): %2")
			.arg(int(error))
			.arg(detail));
	_token.fill(QChar(0));
	_token.clear();
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
	const auto failCallback = std::move(_fail);
	if (failCallback) {
		failCallback(error, std::move(detail));
	}
}

} // namespace Api
