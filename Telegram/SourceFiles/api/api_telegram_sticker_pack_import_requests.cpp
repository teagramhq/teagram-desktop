/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_telegram_sticker_pack_import_requests.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <algorithm>

namespace Api::details {
namespace {

constexpr auto kMaximumBotApiStickers = 200;

[[nodiscard]] std::optional<TelegramStickerInfo>
ParseSticker(const QJsonObject &object, Core::StickerImportType type) {
	const auto fileId = object.value(u"file_id"_q);
	const auto animated = object.value(u"is_animated"_q);
	const auto video = object.value(u"is_video"_q);
	const auto stickerType = object.value(u"type"_q);
	const auto emoji = object.value(u"emoji"_q);
	const auto customEmojiId = object.value(u"custom_emoji_id"_q);
	if (!fileId.isString() || fileId.toString().isEmpty() || !animated.isBool()
		|| !video.isBool() || !stickerType.isString()
		|| (emoji.isUndefined() ? false : !emoji.isString())
		|| (customEmojiId.isUndefined() ? false : !customEmojiId.isString())) {
		return std::nullopt;
	}
	const auto isEmoji = type == Core::StickerImportType::Emoji;
	if (stickerType.toString()
		!= (isEmoji ? u"custom_emoji"_q : u"regular"_q)) {
		return std::nullopt;
	}
	if (animated.toBool() && video.toBool()) {
		return std::nullopt;
	}
	const auto id = customEmojiId.toString();
	if (isEmoji && id.isEmpty()) {
		return std::nullopt;
	}
	return TelegramStickerInfo{
		.fileId = fileId.toString(),
		.customEmojiId = id,
		.emoji = emoji.toString(),
		.format = animated.toBool()
					  ? Core::StickerImportFormat::Tgs
					  : (video.toBool() ? Core::StickerImportFormat::Webm
										: Core::StickerImportFormat::Webp),
	};
}

[[nodiscard]] QJsonObject ParseObject(const QByteArray &response) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(response, &error);
	return (error.error == QJsonParseError::NoError && document.isObject())
			   ? document.object()
			   : QJsonObject();
}

[[nodiscard]] bool IsSuccessfulResponse(const QJsonObject &object) {
	const auto ok = object.value(u"ok"_q);
	return ok.isBool() && ok.toBool();
}

} // namespace

bool IsValidTelegramBotToken(const QString &token) {
	static const auto pattern
		= QRegularExpression(u"^\\d+:[A-Za-z0-9_-]{30,}$"_q);
	return pattern.match(token).hasMatch();
}

bool IsSafeTelegramStickerFilePath(const QString &path) {
	const auto decoded = QUrl::fromPercentEncoding(path.toUtf8());
	if (path.isEmpty() || path.startsWith(QChar('/'))
		|| path.startsWith(u"\\\\"_q) || path.contains(u".."_q)
		|| path.contains(QChar('\\')) || path.contains(QChar('?'))
		|| path.contains(QChar('#')) || path.contains(QChar('%'))
		|| decoded.startsWith(QChar('/')) || decoded.contains(u".."_q)
		|| decoded.contains(QChar('\\')) || decoded.contains(QChar('?'))
		|| decoded.contains(QChar('#'))
		|| std::any_of(decoded.cbegin(), decoded.cend(), [](QChar ch) {
			   return ch.isNull() || ch.category() == QChar::Other_Control;
		   })) {
		return false;
	}
	const auto parsed = QUrl(path, QUrl::StrictMode);
	return parsed.isValid() && parsed.scheme().isEmpty()
		   && parsed.host().isEmpty() && !path.startsWith(u"//"_q);
}

QUrl TelegramBotApiMethodUrl(const QString &token, const QString &method) {
	if (!IsValidTelegramBotToken(token)
		|| (method != u"getStickerSet"_q
			&& method != u"getCustomEmojiStickers"_q
			&& method != u"getFile"_q)) {
		return QUrl();
	}
	auto result = QUrl(u"https://api.telegram.org"_q);
	result.setPath(u"/bot"_q + token + '/' + method, QUrl::DecodedMode);
	return result;
}

std::optional<QUrl> TelegramBotApiFileUrl(const QString &token,
										  const QString &filePath) {
	if (!IsValidTelegramBotToken(token)
		|| !IsSafeTelegramStickerFilePath(filePath)) {
		return std::nullopt;
	}
	auto result = QUrl(u"https://api.telegram.org"_q);
	result.setPath(u"/file/bot"_q + token + '/' + filePath, QUrl::DecodedMode);
	return result;
}

QString RedactTelegramBotApiError(QString description, const QString &token) {
	if (token.isEmpty()) {
		return description;
	}
	const auto redacted = u"[redacted]"_q;
	description.replace(token, redacted, Qt::CaseInsensitive);
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(token));
	description.replace(encoded, redacted, Qt::CaseInsensitive);
	return description;
}

std::optional<TelegramStickerSetInfo>
ParseTelegramStickerSetResponse(const QByteArray &response,
								Core::StickerImportType type) {
	const auto root = ParseObject(response);
	if (!IsSuccessfulResponse(root)) {
		return std::nullopt;
	}
	const auto result = root.value(u"result"_q);
	if (!result.isObject()) {
		return std::nullopt;
	}
	const auto object = result.toObject();
	const auto title = object.value(u"title"_q);
	const auto stickerType = object.value(u"sticker_type"_q);
	const auto stickers = object.value(u"stickers"_q);
	const auto expectedType = (type == Core::StickerImportType::Emoji)
								  ? u"custom_emoji"_q
								  : u"regular"_q;
	if (!title.isString() || title.toString().trimmed().isEmpty()
		|| !stickerType.isString() || stickerType.toString() != expectedType
		|| !stickers.isArray() || stickers.toArray().isEmpty()
		|| stickers.toArray().size() > kMaximumBotApiStickers) {
		return std::nullopt;
	}
	auto parsed = TelegramStickerSetInfo{.title = title.toString()};
	parsed.stickers.reserve(size_t(stickers.toArray().size()));
	for (const auto &value : stickers.toArray()) {
		if (!value.isObject()) {
			return std::nullopt;
		}
		const auto sticker = ParseSticker(value.toObject(), type);
		if (!sticker) {
			return std::nullopt;
		}
		parsed.stickers.push_back(*sticker);
	}
	return parsed;
}

std::optional<std::vector<TelegramStickerInfo>>
ParseTelegramCustomEmojiStickersResponse(
	const QByteArray &response,
	const std::vector<TelegramStickerInfo> &packStickers) {
	if (packStickers.empty() || packStickers.size() > kMaximumBotApiStickers) {
		return std::nullopt;
	}
	const auto root = ParseObject(response);
	if (!IsSuccessfulResponse(root)) {
		return std::nullopt;
	}
	const auto result = root.value(u"result"_q);
	if (!result.isArray()
		|| result.toArray().size() != int(packStickers.size())) {
		return std::nullopt;
	}
	auto byId = QHash<QString, TelegramStickerInfo>();
	for (const auto &value : result.toArray()) {
		if (!value.isObject()) {
			return std::nullopt;
		}
		const auto sticker
			= ParseSticker(value.toObject(), Core::StickerImportType::Emoji);
		if (!sticker || byId.contains(sticker->customEmojiId)) {
			return std::nullopt;
		}
		byId.insert(sticker->customEmojiId, *sticker);
	}
	auto ordered = std::vector<TelegramStickerInfo>();
	ordered.reserve(packStickers.size());
	for (const auto &sticker : packStickers) {
		const auto i = byId.constFind(sticker.customEmojiId);
		if (i == byId.cend()) {
			return std::nullopt;
		}
		auto current = i.value();
		if (current.emoji.isEmpty()) {
			current.emoji = sticker.emoji;
		}
		ordered.push_back(std::move(current));
	}
	return ordered;
}

} // namespace Api::details
