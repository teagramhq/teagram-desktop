/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_verification.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QSet>

#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace Core {
namespace {

using Reason = TeagramUpdateVerificationReason;
using Ed25519PublicKey = std::array<unsigned char, 32>;

constexpr auto kMaximumManifestSize = 16 * 1024;
constexpr auto kExpectedFieldCount = 11;
constexpr auto kMaximumSignatureSize = 64;
constexpr auto kMaximumHashSize = 32;

constexpr auto kProductionUpdatePublicKey = std::optional<Ed25519PublicKey>();

struct PkeyDeleter {
	void operator()(EVP_PKEY *value) const { EVP_PKEY_free(value); }
};

struct DigestContextDeleter {
	void operator()(EVP_MD_CTX *value) const { EVP_MD_CTX_free(value); }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

struct ManifestParseResult {
	Reason reason = Reason::InvalidManifest;
	std::optional<VerifiedTeagramUpdate> package;
};

[[nodiscard]] TeagramUpdateVerificationResult Rejected(Reason reason) {
	return {reason, std::nullopt};
}

[[nodiscard]] bool IsJsonWhitespace(char ch) {
	return (ch == ' ') || (ch == '\t') || (ch == '\n') || (ch == '\r');
}

void SkipJsonWhitespace(const QByteArray &json, qsizetype *position) {
	while (*position < json.size() && IsJsonWhitespace(json[*position])) {
		++*position;
	}
}

[[nodiscard]] qsizetype JsonStringEnd(const QByteArray &json,
									  qsizetype position) {
	if (position >= json.size() || json[position] != '"') {
		return -1;
	}
	for (++position; position < json.size(); ++position) {
		if (json[position] == '\\') {
			++position;
		} else if (json[position] == '"') {
			return position + 1;
		}
	}
	return -1;
}

[[nodiscard]] bool HasDuplicateOrNestedManifestFields(const QByteArray &json) {
	auto position = qsizetype(0);
	SkipJsonWhitespace(json, &position);
	if (position >= json.size() || json[position++] != '{') {
		return true;
	}
	auto keys = QSet<QString>();
	for (auto count = 0; count != kExpectedFieldCount; ++count) {
		SkipJsonWhitespace(json, &position);
		if (position < json.size() && json[position] == '}') {
			return false;
		}
		const auto end = JsonStringEnd(json, position);
		if (end < 0) {
			return true;
		}
		const auto token = json.mid(position, end - position);
		const auto wrapped = QByteArrayLiteral("[") + token + "]";
		auto error = QJsonParseError{0, QJsonParseError::NoError};
		const auto document = QJsonDocument::fromJson(wrapped, &error);
		if (error.error != QJsonParseError::NoError || !document.isArray()) {
			return true;
		}
		const auto key = document.array().first().toString();
		if (keys.contains(key)) {
			return true;
		}
		keys.insert(key);
		position = end;
		SkipJsonWhitespace(json, &position);
		if (position >= json.size() || json[position++] != ':') {
			return true;
		}
		SkipJsonWhitespace(json, &position);
		if (position >= json.size() || json[position] == '{'
			|| json[position] == '[') {
			return true;
		}
		if (json[position] == '"') {
			position = JsonStringEnd(json, position);
			if (position < 0) {
				return true;
			}
		} else {
			const auto start = position;
			while (position < json.size() && json[position] != ','
				   && json[position] != '}'
				   && !IsJsonWhitespace(json[position])) {
				++position;
			}
			if (position == start) {
				return true;
			}
		}
		SkipJsonWhitespace(json, &position);
		if (position < json.size() && json[position] == '}') {
			return false;
		}
		if (position >= json.size() || json[position++] != ',') {
			return true;
		}
	}
	SkipJsonWhitespace(json, &position);
	return position < json.size() && json[position] != '}';
}

[[nodiscard]] bool IsLowerHex(const QString &value, qsizetype length) {
	if (value.size() != length) {
		return false;
	}
	for (const auto ch : value) {
		const auto code = ch.unicode();
		if (!((code >= u'0' && code <= u'9')
			  || (code >= u'a' && code <= u'f'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::optional<quint64>
ParseCanonicalUnsigned(const QString &value) {
	if (value.isEmpty() || (value.size() > 1 && value.front() == u'0')) {
		return std::nullopt;
	}
	auto encoded = value.toLatin1();
	if (encoded.size() != value.size()) {
		return std::nullopt;
	}
	auto result = quint64(0);
	const auto parsed = std::from_chars(
		encoded.constData(), encoded.constData() + encoded.size(), result);
	if (parsed.ec != std::errc()
		|| parsed.ptr != encoded.constData() + encoded.size()) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] ManifestParseResult ParseManifest(const QByteArray &manifest) {
	if (manifest.isEmpty()) {
		return {Reason::InvalidManifest, std::nullopt};
	} else if (manifest.size() > kMaximumManifestSize) {
		return {Reason::ManifestTooLarge, std::nullopt};
	}
	auto error = QJsonParseError{0, QJsonParseError::NoError};
	const auto document = QJsonDocument::fromJson(manifest, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()
		|| HasDuplicateOrNestedManifestFields(manifest)) {
		return {Reason::InvalidManifest, std::nullopt};
	}
	const auto fields = document.object();
	if (fields.size() != kExpectedFieldCount) {
		return {Reason::InvalidManifest, std::nullopt};
	}
	const auto fieldNames = std::array{
		u"format"_q,	 u"product"_q,		u"repo"_q,	 u"arch"_q,
		u"channel"_q,	 u"build"_q,		u"commit"_q, u"asset_name"_q,
		u"asset_size"_q, u"asset_sha256"_q, u"min_os"_q,
	};
	for (const auto &name : fieldNames) {
		if (!fields.contains(name)) {
			return {Reason::InvalidManifest, std::nullopt};
		}
	}
	const auto format = fields.value(u"format"_q);
	if (!format.isDouble()) {
		return {Reason::InvalidManifest, std::nullopt};
	} else if (format.toDouble() != 1.) {
		return {Reason::UnsupportedFormat, std::nullopt};
	}
	const auto stringFields = std::array{
		u"product"_q,	   u"repo"_q,	u"arch"_q,		 u"channel"_q,
		u"build"_q,		   u"commit"_q, u"asset_name"_q, u"asset_size"_q,
		u"asset_sha256"_q, u"min_os"_q,
	};
	for (const auto &name : stringFields) {
		if (!fields.value(name).isString()) {
			return {Reason::InvalidManifest, std::nullopt};
		}
	}
	if (fields.value(u"product"_q).toString() != u"io.teagram.desktop"_q) {
		return {Reason::WrongProduct, std::nullopt};
	}
	if (fields.value(u"repo"_q).toString() != u"teagramhq/teagram-desktop"_q) {
		return {Reason::WrongRepository, std::nullopt};
	}
	if (fields.value(u"arch"_q).toString() != u"arm64"_q) {
		return {Reason::WrongArchitecture, std::nullopt};
	}
	const auto channelName = fields.value(u"channel"_q).toString();
	const auto channel = [&]() -> std::optional<TeagramUpdateChannel> {
		if (channelName == u"dev"_q) {
			return TeagramUpdateChannel::Dev;
		} else if (channelName == u"main"_q) {
			return TeagramUpdateChannel::Main;
		}
		return std::nullopt;
	}();
	if (!channel) {
		return {Reason::InvalidChannel, std::nullopt};
	}
	const auto build
		= ParseCanonicalUnsigned(fields.value(u"build"_q).toString());
	if (!build || *build == 0) {
		return {Reason::InvalidBuild, std::nullopt};
	}
	const auto assetSize
		= ParseCanonicalUnsigned(fields.value(u"asset_size"_q).toString());
	if (!assetSize || *assetSize == 0) {
		return {Reason::InvalidAssetSize, std::nullopt};
	}
	const auto commit = fields.value(u"commit"_q).toString();
	if (!IsLowerHex(commit, 40)) {
		return {Reason::InvalidManifest, std::nullopt};
	}
	const auto assetName = fields.value(u"asset_name"_q).toString();
	const auto expectedAssetName = u"Teagram-macOS-arm64-%1.zip"_q.arg(*build);
	if (assetName != expectedAssetName) {
		return {Reason::InvalidAssetName, std::nullopt};
	}
	const auto assetSha256 = fields.value(u"asset_sha256"_q).toString();
	if (!IsLowerHex(assetSha256, 64)) {
		return {Reason::InvalidHash, std::nullopt};
	}
	return {
		Reason::Eligible,
		VerifiedTeagramUpdate{
			*channel,
			*build,
			commit,
			assetName,
			*assetSize,
			assetSha256,
			fields.value(u"min_os"_q).toString(),
		},
	};
}

[[nodiscard]] QByteArray SignatureMessage(const QByteArray &manifest) {
	auto result = QByteArrayLiteral("teagram-update-v1");
	result.append('\0');
	result.append(manifest);
	return result;
}

[[nodiscard]] TeagramUpdateVerificationResult
VerifyPackage(const QByteArray &manifest, const QByteArray &signature,
			  QByteArrayView archive, quint64 installedBuild,
			  TeagramUpdateChannel installedChannel,
			  const std::optional<Ed25519PublicKey> &trustedKey) {
	if (!trustedKey) {
		return Rejected(Reason::MissingTrustedKey);
	}
	if (manifest.size() > kMaximumManifestSize) {
		return Rejected(Reason::ManifestTooLarge);
	}
	if (signature.size() != kMaximumSignatureSize) {
		return Rejected(Reason::InvalidSignature);
	}
	auto key = Pkey(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519, nullptr, trustedKey->data(), trustedKey->size()));
	auto context = DigestContext(EVP_MD_CTX_new());
	if (!key || !context
		|| EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
								key.get())
			   != 1) {
		return Rejected(Reason::VerifierUnavailable);
	}
	const auto message = SignatureMessage(manifest);
	const auto verified = EVP_DigestVerify(
		context.get(),
		reinterpret_cast<const unsigned char *>(signature.constData()),
		size_t(signature.size()),
		reinterpret_cast<const unsigned char *>(message.constData()),
		size_t(message.size()));
	if (verified == 0) {
		return Rejected(Reason::InvalidSignature);
	} else if (verified != 1) {
		return Rejected(Reason::VerifierUnavailable);
	}
	const auto parsed = ParseManifest(manifest);
	if (parsed.reason != Reason::Eligible || !parsed.package) {
		return Rejected(parsed.reason);
	}
	auto package = *parsed.package;
	if (package.build <= installedBuild) {
		return Rejected(Reason::NotNewer);
	}
	switch (installedChannel) {
	case TeagramUpdateChannel::Dev:
		break;
	case TeagramUpdateChannel::Main:
		if (package.channel != TeagramUpdateChannel::Main) {
			return Rejected(Reason::IneligibleChannel);
		}
		break;
	default:
		return Rejected(Reason::InvalidChannel);
	}
	if (archive.size() < 0 || quint64(archive.size()) != package.assetSize
		|| package.assetSize > std::numeric_limits<size_t>::max()) {
		return Rejected(Reason::ArchiveSizeMismatch);
	}
	auto digest = std::array<unsigned char, kMaximumHashSize>();
	auto digestSize = 0U;
	if (EVP_Digest(archive.data(), size_t(archive.size()), digest.data(),
				   &digestSize, EVP_sha256(), nullptr)
			!= 1
		|| digestSize != digest.size()) {
		return Rejected(Reason::VerifierUnavailable);
	}
	const auto actualHash
		= QByteArray(reinterpret_cast<const char *>(digest.data()),
					 int(digest.size()))
			  .toHex();
	if (actualHash != package.assetSha256.toLatin1()) {
		return Rejected(Reason::ArchiveHashMismatch);
	}
	return {Reason::Eligible, std::move(package)};
}

} // namespace

bool TeagramUpdateVerificationResult::eligible() const {
	return reason == TeagramUpdateVerificationReason::Eligible
		   && package.has_value();
}

TeagramUpdateVerificationResult
VerifyTeagramUpdatePackage(const QByteArray &manifest,
						   const QByteArray &signature, QByteArrayView archive,
						   quint64 installedBuild,
						   TeagramUpdateChannel installedChannel) {
	return VerifyPackage(manifest, signature, archive, installedBuild,
						 installedChannel, kProductionUpdatePublicKey);
}

#ifdef TDESKTOP_UNIT_TESTS
TeagramUpdateVerificationResult VerifyTeagramUpdatePackageForTests(
	const QByteArray &manifest, const QByteArray &signature,
	QByteArrayView archive, quint64 installedBuild,
	TeagramUpdateChannel installedChannel,
	const std::optional<TeagramUpdatePublicKey> &trustedKey) {
	return VerifyPackage(manifest, signature, archive, installedBuild,
						 installedChannel, trustedKey);
}
#endif // TDESKTOP_UNIT_TESTS

} // namespace Core
