/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_telegram_sticker_pack.h"

#include "core/sticker_import.h"
#include "logs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QMetaObject>
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

[[nodiscard]] QString ErrorName(TelegramStickerPackDownloadError error) {
	switch (error) {
	case TelegramStickerPackDownloadError::InvalidToken:
		return u"InvalidToken"_q;
	case TelegramStickerPackDownloadError::InvalidLink:
		return u"InvalidLink"_q;
	case TelegramStickerPackDownloadError::UnsupportedPackType:
		return u"UnsupportedPackType"_q;
	case TelegramStickerPackDownloadError::PackNotFound:
		return u"PackNotFound"_q;
	case TelegramStickerPackDownloadError::BotApi:
		return u"BotApi"_q;
	case TelegramStickerPackDownloadError::Network:
		return u"Network"_q;
	case TelegramStickerPackDownloadError::InvalidResponse:
		return u"InvalidResponse"_q;
	case TelegramStickerPackDownloadError::UnsafeFilePath:
		return u"UnsafeFilePath"_q;
	case TelegramStickerPackDownloadError::TooManyFiles:
		return u"TooManyFiles"_q;
	case TelegramStickerPackDownloadError::FileTooLarge:
		return u"FileTooLarge"_q;
	case TelegramStickerPackDownloadError::TotalTooLarge:
		return u"TotalTooLarge"_q;
	case TelegramStickerPackDownloadError::UnsupportedFormat:
		return u"UnsupportedFormat"_q;
	}
	Unexpected("Telegram sticker download error.");
}

} // namespace

TelegramStickerPackDownloader::TelegramStickerPackDownloader(
	Core::TelegramStickerPackLink link, Core::TelegramBotToken token,
	QNetworkAccessManager *network)
	: _link(std::move(link)), _token(std::move(token)),
	  _network(network ? network : new QNetworkAccessManager(this)) {}

TelegramStickerPackDownloader::~TelegramStickerPackDownloader() { cancel(); }

void TelegramStickerPackDownloader::start(Done done, Fail failCallback,
										  Progress progress) {
	Expects(_state == State::Idle);
	_state = State::Running;
	_done = std::move(done);
	_fail = std::move(failCallback);
	_progress = std::move(progress);
	const auto parsed
		= Core::ParseTelegramStickerPackLink(_link.canonicalUrl());
	if (!parsed || parsed->name != _link.name || parsed->type != _link.type) {
		fail(TelegramStickerPackDownloadError::InvalidLink);
		return;
	}
	requestJson(
		u"getStickerSet"_q, {{u"name"_q, _link.name}},
		[=](QJsonObject result) { handleStickerSet(std::move(result)); });
}

void TelegramStickerPackDownloader::cancel() {
	if (_state == State::Finished || _state == State::Cancelled) {
		return;
	}
	transition(State::Cancelled);
	_files.clear();
	_stickers.clear();
	_title.clear();
	_token.reset();
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
}

void TelegramStickerPackDownloader::transition(State state) {
	_state = state;
	_done = nullptr;
	_fail = nullptr;
	_progress = nullptr;
}

void TelegramStickerPackDownloader::requestJson(QString method, Query query,
												Fn<void(QJsonObject)> done) {
	const auto maybeUrl = _token->apiUrl(method);
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
	if (_state != State::Running) {
		return;
	}
	Expects(!_reply);
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
		if (_state != State::Running || _reply != reply) {
			return false;
		}
		const auto chunk
			= reply->read(qint64(maximumBytes - quint64(response->size()) + 1));
		if (quint64(response->size()) + quint64(chunk.size()) > maximumBytes) {
			fail(tooLarge);
			return false;
		}
		response->append(chunk);
		return true;
	};
	connect(reply, &QIODevice::readyRead, this, [=] { readAvailable(); });
	connect(reply, &QNetworkReply::metaDataChanged, this, [=] {
		if (_state != State::Running || _reply != reply) {
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
		if (_state != State::Running || _reply != reply) {
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
		const auto errorString = reply->errorString();
		reply->deleteLater();
		if (!safeOrigin || redirected || (status >= 300 && status < 400)) {
			fail(TelegramStickerPackDownloadError::Network,
				 u"HTTP %1, host %2"_q.arg(status).arg(responseUrl.host()),
				 errorString);
			return;
		} else if (jsonResponse && status >= 400) {
			done(std::move(*response));
			return;
		} else if (networkError != QNetworkReply::NoError || status < 200
				   || status >= 300) {
			fail(TelegramStickerPackDownloadError::Network, QString(),
				 u"HTTP %1: %2"_q.arg(status).arg(errorString));
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
				 description.isEmpty() ? QString::number(code)
									   : _token->redact(description));
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
	const auto stickerType = JsonString(set, u"sticker_type"_q);
	const auto array = set.value(u"stickers"_q);
	if (title.isEmpty() || !array.isArray()
		|| (stickerType != u"regular"_q && stickerType != u"custom_emoji"_q
			&& stickerType != u"mask"_q)) {
		fail(TelegramStickerPackDownloadError::InvalidResponse);
		return;
	}
	const auto expectedType = (_link.type == Core::StickerImportType::Emoji)
								  ? u"custom_emoji"_q
								  : u"regular"_q;
	if (stickerType != expectedType) {
		fail(TelegramStickerPackDownloadError::UnsupportedPackType);
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
	_title = _token->redact(title);
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
		if (fileId.isEmpty() || emoji.isEmpty()
			|| (_link.type == Core::StickerImportType::Emoji
				&& customEmojiId.isEmpty())) {
			fail(TelegramStickerPackDownloadError::InvalidResponse);
			return;
		}
		_stickers.push_back({
			.fileId = fileId,
			.customEmojiId = customEmojiId,
			.emoji = _token->redact(emoji),
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
			sticker.emoji = _token->redact(emoji);
		}
	}
	downloadNextSticker();
}

void TelegramStickerPackDownloader::downloadNextSticker() {
	if (_state != State::Running) {
		return;
	}
	if (_index >= int(_stickers.size())) {
		const auto weak = base::make_weak(this);
		const auto type = _link.type;
		_token.reset();
		crl::async([weak, files = std::move(_files), type]() mutable {
			auto scan = std::make_shared<Core::StickerImportScan>(
				Core::ScanStickerImportFiles(std::move(files), type));
			QMetaObject::invokeMethod(
				QCoreApplication::instance(),
				[weak, scan] {
					const auto self = weak.get();
					if (!self || self->_state != State::Running) {
						return;
					}
					self->_token.reset();
					const auto done = std::move(self->_done);
					self->transition(State::Finished);
					if (done) {
						done(std::move(self->_title), std::move(*scan));
					}
				},
				Qt::QueuedConnection);
		});
		return;
	}
	const auto weak = base::make_weak(this);
	if (const auto progress = _progress) {
		progress(_index, int(_stickers.size()));
		if (!weak.get() || weak.get()->_state != State::Running) {
			return;
		}
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
		fail(TelegramStickerPackDownloadError::TotalTooLarge);
		return;
	}
	const auto remaining = kMaximumDownloadedBytes - _totalDownloaded;
	const auto maximumBytes = std::min(maximumFileBytes, remaining);
	const auto url = _token->fileUrl(path);
	if (!url) {
		fail(TelegramStickerPackDownloadError::UnsafeFilePath);
		return;
	}
	requestBytes(*url, maximumBytes,
				 (remaining < maximumFileBytes)
					 ? TelegramStickerPackDownloadError::TotalTooLarge
					 : TelegramStickerPackDownloadError::FileTooLarge,
				 false, [=](QByteArray bytes) {
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
						 .emoji = emoji,
					 });
					 ++_index;
					 const auto weak = base::make_weak(this);
					 if (const auto progress = _progress) {
						 progress(_index, int(_stickers.size()));
					 }
					 if (const auto self = weak.get()) {
						 self->downloadNextSticker();
					 }
				 });
}

void TelegramStickerPackDownloader::fail(TelegramStickerPackDownloadError error,
										 QString detail, QString diagnostic) {
	if (_state != State::Running) {
		return;
	}
	const auto failCallback = std::move(_fail);
	transition(State::Finished);
	_files.clear();
	_stickers.clear();
	_title.clear();
	if (_token) {
		detail = _token->redact(std::move(detail));
		diagnostic = _token->redact(std::move(diagnostic));
	}
	static const auto urlPattern
		= QRegularExpression(QStringLiteral("\\bhttps?://\\S+"));
	diagnostic.replace(urlPattern, u"[redacted]"_q);
	LOG(("Telegram sticker pack download failed (%1): %2 %3")
			.arg(ErrorName(error))
			.arg(detail)
			.arg(diagnostic));
	_token.reset();
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
	if (failCallback) {
		failCallback(error, std::move(detail));
	}
}

} // namespace Api
