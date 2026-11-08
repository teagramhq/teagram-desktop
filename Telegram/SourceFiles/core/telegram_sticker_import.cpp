/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/telegram_sticker_import.h"

#include "base/basic_types.h"

#include <QtCore/QRegularExpression>
#include <openssl/crypto.h>

#include <utility>

namespace Core {
namespace {

[[nodiscard]] bool IsValidTelegramStickerBotToken(const QString &token);
[[nodiscard]] QString RedactTelegramStickerBotToken(QString text,
													const QString &token);
[[nodiscard]] std::optional<QUrl>
TelegramStickerBotApiUrl(const QString &token, const QString &method);
[[nodiscard]] std::optional<QUrl>
TelegramStickerBotFileUrl(const QString &token, const QString &filePath);

[[nodiscard]] bool IsValidPackName(const QString &name) {
	static const auto expression
		= QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_]{1,64}\\z"));
	return expression.match(name).hasMatch();
}

} // namespace

TelegramBotToken::TelegramBotToken(QString value) : _value(std::move(value)) {
	_value.detach();
}

TelegramBotToken::TelegramBotToken(TelegramBotToken &&other) noexcept
	: _value(std::move(other._value)) {}

TelegramBotToken &
TelegramBotToken::operator=(TelegramBotToken &&other) noexcept {
	if (this != &other) {
		wipe();
		_value = std::move(other._value);
	}
	return *this;
}

TelegramBotToken::~TelegramBotToken() { wipe(); }

void TelegramBotToken::wipe() {
	if (!_value.isEmpty()) {
		OPENSSL_cleanse(_value.data(), size_t(_value.size()) * sizeof(QChar));
		_value.clear();
	}
}

std::optional<TelegramBotToken> TelegramBotToken::Create(QString value) {
	if (!IsValidTelegramStickerBotToken(value)) {
		return std::nullopt;
	}
	return TelegramBotToken(std::move(value));
}

TelegramBotToken TelegramBotToken::clone() const {
	return TelegramBotToken(_value);
}

QString TelegramBotToken::redact(QString text) const {
	return RedactTelegramStickerBotToken(std::move(text), _value);
}

std::optional<QUrl> TelegramBotToken::apiUrl(const QString &method) const {
	return TelegramStickerBotApiUrl(_value, method);
}

std::optional<QUrl> TelegramBotToken::fileUrl(const QString &path) const {
	return TelegramStickerBotFileUrl(_value, path);
}

std::optional<TelegramStickerPackLink>
ParseTelegramStickerPackLink(const QString &link) {
	auto value = link.trimmed();
	if (value.isEmpty() || value.startsWith(u"//"_q)) {
		return std::nullopt;
	}
	if (!value.contains(u"://"_q)) {
		value.prepend(u"https://"_q);
	}
	const auto url = QUrl(value, QUrl::StrictMode);
	if (url.scheme() != u"https"_q || url.host() != u"t.me"_q
		|| url.port(-1) != -1 || !url.userName().isEmpty()
		|| !url.password().isEmpty() || url.hasQuery() || url.hasFragment()) {
		return std::nullopt;
	}
	static const auto expression = QRegularExpression(
		QStringLiteral("\\A/(addstickers|addemoji)/([A-Za-z0-9_]{1,64})\\z"));
	const auto match = expression.match(url.path(QUrl::FullyEncoded));
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	const auto type = (match.captured(1) == u"addemoji"_q)
						  ? StickerImportType::Emoji
						  : StickerImportType::Stickers;
	const auto name = match.captured(2);
	return TelegramStickerPackLink{
		.type = type,
		.name = name,
	};
}

QString TelegramStickerPackLink::canonicalUrl() const {
	return TelegramStickerPackCanonicalUrl(type, name);
}

void TelegramStickerPackDownloadStatus::setProgress(QString text) {
	_text = std::move(text);
	_error = false;
}

void TelegramStickerPackDownloadStatus::setError(QString text) {
	_text = std::move(text);
	_error = true;
}

const QString &TelegramStickerPackDownloadStatus::text() const {
	return _text;
}

bool TelegramStickerPackDownloadStatus::isError() const {
	return _error;
}

QString TelegramStickerPackCanonicalUrl(StickerImportType type,
										const QString &name) {
	if (!IsValidPackName(name)) {
		return QString();
	}
	const auto route
		= (type == StickerImportType::Emoji) ? u"addemoji"_q : u"addstickers"_q;
	return u"https://t.me/%1/%2"_q.arg(route, name);
}

namespace {
bool IsValidTelegramStickerBotToken(const QString &token) {
	static const auto expression
		= QRegularExpression(QStringLiteral("\\A\\d+:[A-Za-z0-9_-]{30,}\\z"));
	return expression.match(token).hasMatch();
}
} // namespace

bool IsValidTelegramStickerFilePath(const QString &path) {
	if (path.isEmpty() || path.size() > 1024 || path.contains(u".."_q)) {
		return false;
	}
	static const auto expression = QRegularExpression(QStringLiteral(
		"\\A[A-Za-z0-9_-][A-Za-z0-9_.-]*(/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*\\z"));
	return expression.match(path).hasMatch();
}

namespace {
QString RedactTelegramStickerBotToken(QString text, const QString &token) {
	if (!token.isEmpty()) {
		text.replace(token, u"[redacted]"_q, Qt::CaseSensitive);
		const auto encoded
			= QString::fromLatin1(QUrl::toPercentEncoding(token));
		if (encoded != token) {
			text.replace(encoded, u"[redacted]"_q, Qt::CaseInsensitive);
		}
	}
	static const auto expression
		= QRegularExpression(QStringLiteral("\\b\\d+:[A-Za-z0-9_-]{30,}\\b"));
	return text.replace(expression, u"[redacted]"_q);
}

std::optional<QUrl> TelegramStickerBotApiUrl(const QString &token,
											 const QString &method) {
	if (!IsValidTelegramStickerBotToken(token)) {
		return std::nullopt;
	}
	static const auto methodExpression
		= QRegularExpression(QStringLiteral("\\A[A-Za-z]+\\z"));
	if (!methodExpression.match(method).hasMatch()) {
		return std::nullopt;
	}
	const auto encoded = QByteArray("https://api.telegram.org/bot")
						 + token.toLatin1() + '/' + method.toLatin1();
	auto result = QUrl::fromEncoded(encoded, QUrl::StrictMode);
	if (!result.isValid()) {
		return std::nullopt;
	}
	return result;
}

std::optional<QUrl> TelegramStickerBotFileUrl(const QString &token,
											  const QString &filePath) {
	if (!IsValidTelegramStickerBotToken(token)
		|| !IsValidTelegramStickerFilePath(filePath)) {
		return std::nullopt;
	}
	const auto encoded = QByteArray("https://api.telegram.org/file/bot")
						 + token.toLatin1() + '/' + filePath.toLatin1();
	auto result = QUrl::fromEncoded(encoded, QUrl::StrictMode);
	if (!result.isValid()) {
		return std::nullopt;
	}
	return result;
}

} // namespace
} // namespace Core
