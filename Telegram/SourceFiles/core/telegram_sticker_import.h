/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/sticker_import.h"

#include <QtCore/QUrl>

#include <optional>

namespace Storage {
class Account;
} // namespace Storage

namespace Core {

class TelegramBotToken final {
  public:
	[[nodiscard]] static std::optional<TelegramBotToken> Create(QString value);
	TelegramBotToken(TelegramBotToken &&other) noexcept;
	TelegramBotToken &operator=(TelegramBotToken &&other) noexcept;
	~TelegramBotToken();

	[[nodiscard]] TelegramBotToken clone() const;
	[[nodiscard]] QString redact(QString text) const;
	[[nodiscard]] std::optional<QUrl> apiUrl(const QString &method) const;
	[[nodiscard]] std::optional<QUrl> fileUrl(const QString &path) const;

  private:
	friend class Storage::Account;
	explicit TelegramBotToken(QString value);
	void wipe();
	QString _value;
};

struct TelegramStickerPackLink {
	StickerImportType type = StickerImportType::Stickers;
	QString name;
	[[nodiscard]] QString canonicalUrl() const;
};

[[nodiscard]] std::optional<TelegramStickerPackLink>
ParseTelegramStickerPackLink(const QString &link);
[[nodiscard]] QString TelegramStickerPackCanonicalUrl(StickerImportType type,
													  const QString &name);
[[nodiscard]] bool IsValidTelegramStickerFilePath(const QString &path);

} // namespace Core
