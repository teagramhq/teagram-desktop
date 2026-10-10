/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QString>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Core {

#ifdef TDESKTOP_TEAGRAM
inline constexpr auto kTeagramUpdateBuild
	= std::uint64_t(TDESKTOP_TEAGRAM_UPDATE_BUILD);
inline constexpr std::string_view kTeagramUpdateChannel
	= TDESKTOP_TEAGRAM_UPDATE_CHANNEL;
#endif // TDESKTOP_TEAGRAM

enum class TeagramUpdateChannel {
	Dev,
	Main,
};

enum class TeagramUpdateVerificationReason {
	Eligible,
	MissingTrustedKey,
	InvalidSignature,
	InvalidManifest,
	ManifestTooLarge,
	UnsupportedFormat,
	WrongProduct,
	WrongRepository,
	WrongArchitecture,
	InvalidChannel,
	InvalidBuild,
	InvalidAssetSize,
	InvalidAssetName,
	InvalidHash,
	NotNewer,
	IneligibleChannel,
	ArchiveSizeMismatch,
	ArchiveHashMismatch,
	VerifierUnavailable,
};

struct VerifiedTeagramUpdate {
	TeagramUpdateChannel channel = TeagramUpdateChannel::Main;
	quint64 build = 0;
	QString commit;
	QString assetName;
	quint64 assetSize = 0;
	QString assetSha256;
	QString minOs;
};

struct TeagramUpdateVerificationResult {
	TeagramUpdateVerificationReason reason
		= TeagramUpdateVerificationReason::InvalidManifest;
	std::optional<VerifiedTeagramUpdate> package;

	[[nodiscard]] bool eligible() const;
};

[[nodiscard]] TeagramUpdateVerificationResult
VerifyTeagramUpdatePackage(const QByteArray &manifest,
						   const QByteArray &signature, QByteArrayView archive,
						   quint64 installedBuild,
						   TeagramUpdateChannel installedChannel);

#ifdef TDESKTOP_UNIT_TESTS
using TeagramUpdatePublicKey = std::array<unsigned char, 32>;

[[nodiscard]] TeagramUpdateVerificationResult
VerifyTeagramUpdatePackageForTests(
	const QByteArray &manifest, const QByteArray &signature,
	QByteArrayView archive, quint64 installedBuild,
	TeagramUpdateChannel installedChannel,
	const std::optional<TeagramUpdatePublicKey> &trustedKey);
#endif // TDESKTOP_UNIT_TESTS

} // namespace Core
