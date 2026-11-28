/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/fragment_link_v1.h"

#include "core/sticker_import.h"
#include "mtproto/mtproto_dc_options.h"

#include <QtCore/QUrl>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace Core::FragmentLinkV1 {
namespace {

const auto kDefaultLinkDomain = u"https://web.teagram.io/"_q;
const auto kDefaultServerHost = u"server.teagram.io"_q;
constexpr auto kTextAlphabet = "0123456789abcdefghijklmnopqrstuvwxyz-._:";
constexpr auto kMaxId = std::numeric_limits<qint64>::max();

struct DecodeResult {
	QByteArray bytes;
	bool valid = false;
};

[[nodiscard]] bool IsBase64UrlCharacter(char ch) {
	return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
		   || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
}

[[nodiscard]] DecodeResult DecodeBase64Url(const QString &value) {
	const auto input = value.toLatin1();
	if (input.isEmpty()
		|| !std::all_of(input.cbegin(), input.cend(), IsBase64UrlCharacter)) {
		return {};
	}
	const auto decoded = QByteArray::fromBase64Encoding(
		input, QByteArray::Base64UrlEncoding
				   | QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded) {
		return {};
	}
	const auto canonical = (*decoded).toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	return {*decoded, canonical == input};
}

[[nodiscard]] QByteArray EncodeText(QString value) {
	auto bytes = QByteArray(1, '\0');
	for (auto character : value) {
		if (character.unicode() > 0x7f) {
			return {};
		}
		if (character >= u'A' && character <= u'Z') {
			character = character.toLower();
		}
		const auto digit = std::strchr(kTextAlphabet, character.toLatin1());
		if (!digit) {
			return {};
		}
		auto carry = static_cast<unsigned int>(digit - kTextAlphabet) + 1;
		for (auto i = bytes.size(); i-- > 0;) {
			const auto result = static_cast<unsigned int>(
									static_cast<unsigned char>(bytes[i]))
									* 40
								+ carry;
			bytes[i] = static_cast<char>(result & 0xff);
			carry = result >> 8;
		}
		while (carry) {
			bytes.prepend(static_cast<char>(carry & 0xff));
			carry >>= 8;
		}
	}
	while (bytes.size() > 1 && bytes.front() == '\0') {
		bytes.remove(0, 1);
	}
	return bytes;
}

[[nodiscard]] std::optional<QString> DecodeText(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.front() == '\0') {
		return std::nullopt;
	}
	auto value = bytes;
	auto reversed = QByteArray();
	while (!value.isEmpty()) {
		for (auto i = value.size(); i-- > 0;) {
			if (static_cast<unsigned char>(value[i]) != 0) {
				--value[i];
				break;
			}
			value[i] = static_cast<char>(0xff);
		}
		auto remainder = 0;
		auto quotient = QByteArray();
		quotient.reserve(value.size());
		for (const auto byte : value) {
			const auto current
				= remainder * 256 + static_cast<unsigned char>(byte);
			const auto digit = current / 40;
			remainder = current % 40;
			if (!quotient.isEmpty() || digit != 0) {
				quotient.push_back(static_cast<char>(digit));
			}
		}
		reversed.push_back(kTextAlphabet[remainder]);
		value = std::move(quotient);
	}
	std::reverse(reversed.begin(), reversed.end());
	return QString::fromLatin1(reversed);
}

[[nodiscard]] bool IsAsciiLetter(QChar ch) {
	return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z');
}

[[nodiscard]] bool IsUsername(const QString &username) {
	if (username.size() < 2 || username.size() > 32
		|| !IsAsciiLetter(username.front())) {
		return false;
	}
	return std::all_of(username.cbegin() + 1, username.cend(), [](QChar ch) {
		return IsAsciiLetter(ch) || (ch >= u'0' && ch <= u'9') || ch == u'_';
	});
}

[[nodiscard]] QByteArray EncodeId(quint64 value) {
	if (value == 0 || value > static_cast<quint64>(kMaxId)) {
		return {};
	}
	auto bytes = QByteArray();
	while (value) {
		bytes.prepend(static_cast<char>(value & 0xff));
		value >>= 8;
	}
	return bytes;
}

[[nodiscard]] std::optional<quint64> DecodeId(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > 8 || bytes.front() == '\0') {
		return std::nullopt;
	}
	auto value = quint64(0);
	for (const auto byte : bytes) {
		value = (value << 8) | static_cast<unsigned char>(byte);
	}
	if (value == 0 || value > static_cast<quint64>(kMaxId)) {
		return std::nullopt;
	}
	return value;
}

[[nodiscard]] int DataFieldCount(Type type) {
	switch (type) {
	case Type::PublicMessage:
	case Type::PrivateChannelMessage:
		return 2;
	default:
		return 1;
	}
}

[[nodiscard]] std::optional<std::vector<QByteArray>>
SplitFields(QByteArray data, int count) {
	auto fields = std::vector<QByteArray>();
	fields.reserve(count);
	for (auto i = 0; i < count - 1; ++i) {
		if (data.isEmpty()) {
			return std::nullopt;
		}
		const auto length = static_cast<unsigned char>(data.front());
		data.remove(0, 1);
		if (length == 0 || length > data.size()) {
			return std::nullopt;
		}
		fields.push_back(data.left(length));
		data.remove(0, length);
	}
	if (data.isEmpty()) {
		return std::nullopt;
	}
	fields.push_back(std::move(data));
	return fields;
}

[[nodiscard]] bool ParsePort(const QString &port, QString *normalized) {
	if (port.isEmpty()
		|| !std::all_of(port.cbegin(), port.cend(),
						[](QChar ch) { return ch >= u'0' && ch <= u'9'; })) {
		return false;
	}
	bool ok = false;
	const auto value = port.toUInt(&ok);
	if (!ok || value == 0 || value > 65535) {
		return false;
	}
	*normalized = QString::number(value);
	return true;
}

struct Ipv4Part {
	bool numeric = false;
	bool valid = false;
	quint64 value = 0;
};

[[nodiscard]] Ipv4Part ParseWhatwgIpv4Part(QString part) {
	if (part.isEmpty()) {
		return {};
	}
	auto radix = quint64(10);
	if (part.size() >= 2 && part[0] == u'0'
		&& (part[1] == u'x' || part[1] == u'X')) {
		radix = 16;
		part = part.mid(2);
	} else if (part.size() >= 2 && part[0] == u'0') {
		radix = 8;
		part = part.mid(1);
	}
	if (part.isEmpty()) {
		return {true, true, 0};
	}
	auto result = quint64(0);
	for (const auto ch : part) {
		auto digit = quint64(0);
		if (ch >= u'0' && ch <= u'9') {
			digit = ch.unicode() - u'0';
		} else if (ch >= u'a' && ch <= u'f') {
			digit = ch.unicode() - u'a' + 10;
		} else if (ch >= u'A' && ch <= u'F') {
			digit = ch.unicode() - u'A' + 10;
		} else {
			return {};
		}
		if (digit >= radix) {
			return {};
		}
		if (result > (std::numeric_limits<quint64>::max() - digit) / radix) {
			return {true, false, 0};
		}
		result = result * radix + digit;
	}
	return {true, true, result};
}

[[nodiscard]] std::optional<QString> NormalizeIpv4(QString host) {
	auto parts = host.split(QChar(u'.'), Qt::KeepEmptyParts);
	if (parts.size() > 1 && parts.back().isEmpty()) {
		parts.pop_back();
	}
	if (parts.isEmpty() || parts.size() > 4) {
		return std::nullopt;
	}
	auto numbers = std::vector<quint64>();
	numbers.reserve(parts.size());
	for (auto i = 0; i < parts.size(); ++i) {
		const auto parsed = ParseWhatwgIpv4Part(parts[i]);
		if (!parsed.numeric || !parsed.valid
			|| (i < parts.size() - 1 && parsed.value > 255)) {
			return std::nullopt;
		}
		numbers.push_back(parsed.value);
	}
	const auto lastLimit = quint64(1) << (8 * (5 - numbers.size()));
	if (numbers.back() >= lastLimit) {
		return std::nullopt;
	}
	auto address = numbers.back();
	for (auto i = 0; i < static_cast<int>(numbers.size()) - 1; ++i) {
		address += numbers[i] << (8 * (3 - i));
	}
	return QStringLiteral("%1.%2.%3.%4")
		.arg((address >> 24) & 0xff)
		.arg((address >> 16) & 0xff)
		.arg((address >> 8) & 0xff)
		.arg(address & 0xff);
}

[[nodiscard]] std::optional<QString> NormalizeHost(QString value) {
	if (value.isEmpty() || value.trimmed() != value) {
		return std::nullopt;
	}
	for (const auto character : value) {
		if (character == u'/' || character == u'?' || character == u'#'
			|| character == u'@' || character == u'\\' || character == u'\t'
			|| character == u'\r' || character == u'\n' || character == u' ') {
			return std::nullopt;
		}
	}
	if (value.count(QChar(u':')) > 1) {
		return std::nullopt;
	}
	auto hostname = value;
	auto port = QString();
	const auto separator = value.lastIndexOf(QChar(u':'));
	if (separator >= 0) {
		hostname = value.left(separator);
		if (!ParsePort(value.mid(separator + 1), &port)) {
			return std::nullopt;
		}
	}
	if (hostname.isEmpty()) {
		return std::nullopt;
	}
	const auto isAscii
		= std::all_of(hostname.cbegin(), hostname.cend(),
					  [](QChar ch) { return ch.unicode() <= 0x7f; });
	const auto encoded
		= isAscii ? hostname.toLower().toLatin1() : QUrl::toAce(hostname);
	auto ascii = QString::fromLatin1(encoded).toLower();
	while (ascii.endsWith('.')) {
		ascii.chop(1);
	}
	if (ascii.isEmpty() || ascii.size() > 253 || ascii.contains("..")) {
		return std::nullopt;
	}
	const auto labels = ascii.split('.');
	for (const auto &label : labels) {
		if (label.isEmpty() || label.size() > 63 || label.front() == '-'
			|| label.back() == '-') {
			return std::nullopt;
		}
		for (const auto character : label) {
			if ((character < 'a' || character > 'z')
				&& (character < '0' || character > '9') && character != '-') {
				return std::nullopt;
			}
		}
	}
	const auto finalPart = ParseWhatwgIpv4Part(labels.back());
	if (finalPart.numeric) {
		const auto normalized = NormalizeIpv4(ascii);
		if (!normalized) {
			return std::nullopt;
		}
		ascii = *normalized;
	}
	if (ascii == u"telegram.org"_q || ascii.endsWith(u".telegram.org"_q)) {
		return std::nullopt;
	}
	return port.isEmpty() ? ascii : (ascii + u":"_q + port);
}

[[nodiscard]] bool HasUnexpectedFields(const Link &link) {
	if (link.type != Type::Profile && link.type != Type::PublicMessage
		&& !link.username.isEmpty()) {
		return true;
	}
	if (link.type != Type::PrivateChannelMessage && link.channelId != 0) {
		return true;
	}
	if (link.type != Type::PublicMessage
		&& link.type != Type::PrivateChannelMessage && link.messageId != 0) {
		return true;
	}
	if (link.type != Type::JoinChat && !link.hash.isNull()) {
		return true;
	}
	if (link.type != Type::SignUpInvite && !link.secret.isNull()) {
		return true;
	}
	return link.type != Type::StickerSet && link.type != Type::EmojiSet
		   && !link.shortName.isEmpty();
}

[[nodiscard]] std::optional<quint64> ParsePositiveId(const QString &text) {
	if (text.isEmpty()
		|| !std::all_of(text.cbegin(), text.cend(),
						[](QChar ch) { return ch >= u'0' && ch <= u'9'; })) {
		return std::nullopt;
	}
	bool ok = false;
	const auto value = text.toULongLong(&ok);
	if (!ok || value == 0 || value > static_cast<quint64>(kMaxId)) {
		return std::nullopt;
	}
	return value;
}

[[nodiscard]] std::optional<QString> EncodeInternalPath(const QString &path) {
	if (path.isEmpty() || path.contains(QChar(u'?'))
		|| path.contains(QChar(u'#'))) {
		return std::nullopt;
	}
	auto parts = path.split(QChar(u'/'), Qt::KeepEmptyParts);
	if (parts.size() > 1 && parts.back().isEmpty()) {
		parts.pop_back();
	}
	if (parts.size() == 1 && IsUsername(parts[0])) {
		return Encode(Link{.type = Type::Profile, .username = parts[0]});
	}
	if (parts.size() == 2 && IsUsername(parts[0])) {
		if (const auto messageId = ParsePositiveId(parts[1])) {
			return Encode(Link{
				.type = Type::PublicMessage,
				.username = parts[0],
				.messageId = *messageId,
			});
		}
	}
	if (parts.size() == 3 && parts[0] == u"c"_q) {
		const auto channelId = ParsePositiveId(parts[1]);
		const auto messageId = ParsePositiveId(parts[2]);
		if (channelId && messageId) {
			return Encode(Link{
				.type = Type::PrivateChannelMessage,
				.channelId = *channelId,
				.messageId = *messageId,
			});
		}
	}
	if (parts.size() == 1 && parts[0].startsWith(QChar(u'+'))) {
		const auto phone
			= std::all_of(parts[0].cbegin() + 1, parts[0].cend(),
						  [](QChar ch) { return ch >= u'0' && ch <= u'9'; });
		if (!phone) {
			const auto decoded = DecodeBase64Url(parts[0].mid(1));
			if (decoded.valid) {
				return Encode(
					Link{.type = Type::JoinChat, .hash = decoded.bytes});
			}
		}
	}
	if (parts.size() == 2 && parts[0] == u"joinchat"_q) {
		const auto decoded = DecodeBase64Url(parts[1]);
		if (decoded.valid) {
			return Encode(Link{.type = Type::JoinChat, .hash = decoded.bytes});
		}
	}
	if (parts.size() == 2 && IsValidStickerSetShortName(parts[1])) {
		if (parts[0] == u"addstickers"_q) {
			return Encode(
				Link{.type = Type::StickerSet, .shortName = parts[1]});
		} else if (parts[0] == u"addemoji"_q) {
			return Encode(Link{.type = Type::EmojiSet, .shortName = parts[1]});
		}
	}
	return std::nullopt;
}

} // namespace

std::optional<QString> Encode(const Link &link) {
	const auto type = static_cast<int>(link.type);
	if (type < static_cast<int>(Type::Profile)
		|| type > static_cast<int>(Type::EmojiSet)
		|| HasUnexpectedFields(link)) {
		return std::nullopt;
	}
	auto host = QString();
	if (!link.host.isEmpty()) {
		const auto normalized = NormalizeHost(link.host);
		if (!normalized || *normalized == kDefaultServerHost) {
			return std::nullopt;
		}
		host = *normalized;
	}
	auto fields = std::vector<QByteArray>();
	if (!host.isEmpty()) {
		const auto encoded = EncodeText(host);
		if (encoded.isEmpty()) {
			return std::nullopt;
		}
		fields.push_back(encoded);
	}
	switch (link.type) {
	case Type::Profile:
		if (!IsUsername(link.username)) {
			return std::nullopt;
		}
		fields.push_back(EncodeText(link.username));
		break;
	case Type::PublicMessage: {
		if (!IsUsername(link.username)) {
			return std::nullopt;
		}
		fields.push_back(EncodeText(link.username));
		const auto messageId = EncodeId(link.messageId);
		if (messageId.isEmpty()) {
			return std::nullopt;
		}
		fields.push_back(messageId);
	} break;
	case Type::PrivateChannelMessage: {
		const auto channelId = EncodeId(link.channelId);
		if (channelId.isEmpty()) {
			return std::nullopt;
		}
		fields.push_back(channelId);
		const auto messageId = EncodeId(link.messageId);
		if (messageId.isEmpty()) {
			return std::nullopt;
		}
		fields.push_back(messageId);
	} break;
	case Type::JoinChat:
		if (link.hash.isEmpty()) {
			return std::nullopt;
		}
		fields.push_back(link.hash);
		break;
	case Type::SignUpInvite:
		if (link.secret.size() != 32) {
			return std::nullopt;
		}
		fields.push_back(link.secret);
		break;
	case Type::StickerSet:
	case Type::EmojiSet: {
		if (link.shortName.isEmpty() || link.shortName.size() > 64) {
			return std::nullopt;
		}
		const auto encoded = EncodeText(link.shortName);
		if (encoded.isEmpty() || encoded.size() > 43) {
			return std::nullopt;
		}
		fields.push_back(encoded);
	} break;
	}
	auto bytes = QByteArray(
		1, static_cast<char>(0x40 | type | (host.isEmpty() ? 0 : 0x20)));
	for (auto i = 0; i < static_cast<int>(fields.size()); ++i) {
		const auto &field = fields[i];
		if (i < static_cast<int>(fields.size()) - 1) {
			if (field.isEmpty() || field.size() > 255) {
				return std::nullopt;
			}
			bytes.push_back(static_cast<char>(field.size()));
		}
		bytes.append(field);
	}
	return u"#~"_q
		   + QString::fromLatin1(bytes.toBase64(
			   QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::optional<Link> Decode(const QString &fragment) {
	if (!fragment.startsWith(u"#~"_q)) {
		return std::nullopt;
	}
	const auto encoded = fragment.mid(2);
	const auto decoded = DecodeBase64Url(encoded);
	if (!decoded.valid || decoded.bytes.isEmpty()) {
		return std::nullopt;
	}
	const auto header = static_cast<unsigned char>(decoded.bytes.front());
	if ((header & 0xc0) != 0x40) {
		return std::nullopt;
	}
	const auto type = static_cast<Type>(header & 0x1f);
	if (type < Type::Profile || type > Type::EmojiSet) {
		return std::nullopt;
	}
	const auto hasHost = (header & 0x20) != 0;
	const auto count = DataFieldCount(type) + (hasHost ? 1 : 0);
	const auto fields = SplitFields(decoded.bytes.mid(1), count);
	if (!fields) {
		return std::nullopt;
	}
	auto result = Link{.type = type};
	auto index = 0;
	if (hasHost) {
		const auto hostText = DecodeText((*fields)[index++]);
		if (!hostText) {
			return std::nullopt;
		}
		const auto normalized = NormalizeHost(*hostText);
		if (!normalized) {
			return std::nullopt;
		}
		result.host = *normalized;
	}
	switch (type) {
	case Type::Profile:
	case Type::PublicMessage: {
		if ((*fields)[index].size() > 22) {
			return std::nullopt;
		}
		const auto username = DecodeText((*fields)[index++]);
		if (!username || !IsUsername(*username)) {
			return std::nullopt;
		}
		result.username = *username;
		if (type == Type::PublicMessage) {
			const auto messageId = DecodeId((*fields)[index]);
			if (!messageId) {
				return std::nullopt;
			}
			result.messageId = *messageId;
		}
	} break;
	case Type::PrivateChannelMessage: {
		const auto channelId = DecodeId((*fields)[index++]);
		const auto messageId = DecodeId((*fields)[index]);
		if (!channelId || !messageId) {
			return std::nullopt;
		}
		result.channelId = *channelId;
		result.messageId = *messageId;
	} break;
	case Type::JoinChat:
		result.hash = (*fields)[index];
		break;
	case Type::SignUpInvite:
		if ((*fields)[index].size() != 32) {
			return std::nullopt;
		}
		result.secret = (*fields)[index];
		break;
	case Type::StickerSet:
	case Type::EmojiSet: {
		if ((*fields)[index].size() > 43) {
			return std::nullopt;
		}
		const auto shortName = DecodeText((*fields)[index]);
		if (!shortName || shortName->size() > 64) {
			return std::nullopt;
		}
		result.shortName = *shortName;
	} break;
	}
	const auto canonical = Encode(result);
	if (!canonical || *canonical != fragment) {
		return std::nullopt;
	}
	return result;
}

UrlResult DecodeUrl(const QString &url) {
	const auto parsed = QUrl(url, QUrl::TolerantMode);
	if (parsed.host().compare(u"web.teagram.io"_q, Qt::CaseInsensitive) != 0) {
		return {};
	}
	const auto fragmentStart = url.indexOf(QChar(u'#'));
	if (fragmentStart < 0 || !url.mid(fragmentStart).startsWith(u"#~"_q)) {
		return {};
	}
	if (!parsed.isValid()
		|| parsed.scheme().compare(u"https"_q, Qt::CaseInsensitive) != 0
		|| (parsed.port(-1) != -1 && parsed.port(-1) != 443)
		|| !parsed.userName().isEmpty() || !parsed.password().isEmpty()
		|| (parsed.path() != QString() && parsed.path() != u"/"_q)
		|| parsed.hasQuery()) {
		return {UrlStatus::Invalid, {}};
	}
	const auto decoded = Decode(url.mid(fragmentStart));
	return decoded ? UrlResult{UrlStatus::Valid, *decoded}
				   : UrlResult{UrlStatus::Invalid, {}};
}

bool IsV1UrlCandidate(const QString &url) {
	return DecodeUrl(url).status != UrlStatus::NotV1;
}

bool CanOpenOnDefaultServer(const Link &link) { return link.host.isEmpty(); }

std::optional<Type> AccountDispatchForClick(const Link &link,
											const MTP::CustomServer &server) {
	if (!MTP::IsDefaultTeagramServer(server) || !CanOpenOnDefaultServer(link)) {
		return std::nullopt;
	}
	switch (link.type) {
	case Type::Profile:
	case Type::PublicMessage:
	case Type::PrivateChannelMessage:
	case Type::JoinChat:
	case Type::StickerSet:
	case Type::EmojiSet:
		return link.type;
	case Type::SignUpInvite:
		return std::nullopt;
	}
	return std::nullopt;
}

QString LinkForPath(const QString &path, const QString &internalLinksDomain) {
	if (internalLinksDomain != kDefaultLinkDomain) {
		return internalLinksDomain + path;
	}
	if (const auto fragment = EncodeInternalPath(path)) {
		return internalLinksDomain + *fragment;
	}
	return internalLinksDomain + path;
}

} // namespace Core::FragmentLinkV1
