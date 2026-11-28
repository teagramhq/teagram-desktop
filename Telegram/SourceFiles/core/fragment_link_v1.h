/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace Core::FragmentLinkV1 {

enum class Type : uchar {
	Profile = 1,
	PublicMessage = 2,
	PrivateChannelMessage = 3,
	JoinChat = 4,
	SignUpInvite = 5,
	StickerSet = 6,
	EmojiSet = 7,
};

struct Link {
	Type type = Type::Profile;
	QString host;
	QString username;
	quint64 channelId = 0;
	quint64 messageId = 0;
	QByteArray hash;
	QByteArray secret;
	QString shortName;

	friend bool operator==(const Link &, const Link &) = default;
};

enum class UrlStatus {
	NotV1,
	Invalid,
	Valid,
};

struct UrlResult {
	UrlStatus status = UrlStatus::NotV1;
	Link link;
};

[[nodiscard]] std::optional<Link> Decode(const QString &fragment);
[[nodiscard]] UrlResult DecodeUrl(const QString &url);
[[nodiscard]] std::optional<QString> Encode(const Link &link);
[[nodiscard]] bool CanOpenOnDefaultServer(const Link &link);
[[nodiscard]] QString LinkForPath(const QString &path,
								  const QString &internalLinksDomain);

} // namespace Core::FragmentLinkV1
