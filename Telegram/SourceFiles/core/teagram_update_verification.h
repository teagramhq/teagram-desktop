/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QIODevice>
#include <QtCore/QString>

#include <array>
#include <functional>
#include <optional>

namespace Core {

inline constexpr auto kMaximumTeagramUpdateManifestSize
	= qsizetype(16 * 1024);
inline constexpr auto kMaximumTeagramUpdateArchiveSize
	= quint64(512) * 1024 * 1024;

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
	ArchiveTooLarge,
	ArchiveReadFailed,
	Cancelled,
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

struct TeagramUpdateManifestVerificationResult;

#ifdef TDESKTOP_UNIT_TESTS
using TeagramUpdatePublicKey = std::array<unsigned char, 32>;
#endif // TDESKTOP_UNIT_TESTS

class AuthenticatedTeagramUpdate final {
public:
	AuthenticatedTeagramUpdate(const AuthenticatedTeagramUpdate &) = default;
	AuthenticatedTeagramUpdate(AuthenticatedTeagramUpdate &&) = default;
	AuthenticatedTeagramUpdate &operator=(
		const AuthenticatedTeagramUpdate &) = delete;
	AuthenticatedTeagramUpdate &operator=(
		AuthenticatedTeagramUpdate &&) = delete;

	[[nodiscard]] TeagramUpdateChannel channel() const {
		return _channel;
	}
	[[nodiscard]] quint64 build() const {
		return _build;
	}
	[[nodiscard]] const QString &commit() const {
		return _commit;
	}
	[[nodiscard]] const QString &assetName() const {
		return _assetName;
	}
	[[nodiscard]] quint64 assetSize() const {
		return _assetSize;
	}
	[[nodiscard]] const QString &assetSha256() const {
		return _assetSha256;
	}
	[[nodiscard]] const QString &minOs() const {
		return _minOs;
	}

private:
	explicit AuthenticatedTeagramUpdate(VerifiedTeagramUpdate package);

	const TeagramUpdateChannel _channel;
	const quint64 _build;
	const QString _commit;
	const QString _assetName;
	const quint64 _assetSize;
	const QString _assetSha256;
	const QString _minOs;

	friend TeagramUpdateManifestVerificationResult
	AuthenticateTeagramUpdateManifest(
		const QByteArray &manifest,
		const QByteArray &signature,
		quint64 installedBuild,
		TeagramUpdateChannel installedChannel);

#ifdef TDESKTOP_UNIT_TESTS
	friend TeagramUpdateManifestVerificationResult
	AuthenticateTeagramUpdateManifestForTests(
		const QByteArray &manifest,
		const QByteArray &signature,
		quint64 installedBuild,
		TeagramUpdateChannel installedChannel,
		const std::optional<TeagramUpdatePublicKey> &trustedKey);
#endif // TDESKTOP_UNIT_TESTS

};

struct TeagramUpdateVerificationResult {
	TeagramUpdateVerificationReason reason
		= TeagramUpdateVerificationReason::InvalidManifest;
	std::optional<VerifiedTeagramUpdate> package;

	[[nodiscard]] bool eligible() const;
};

struct TeagramUpdateManifestVerificationResult {
	TeagramUpdateVerificationReason reason
		= TeagramUpdateVerificationReason::InvalidManifest;
	std::optional<AuthenticatedTeagramUpdate> package;

	[[nodiscard]] bool authenticated() const;
};

[[nodiscard]] TeagramUpdateVerificationResult
VerifyTeagramUpdatePackage(const QByteArray &manifest,
						   const QByteArray &signature, QByteArrayView archive,
						   quint64 installedBuild,
						   TeagramUpdateChannel installedChannel);

[[nodiscard]] TeagramUpdateManifestVerificationResult
AuthenticateTeagramUpdateManifest(const QByteArray &manifest,
									 const QByteArray &signature,
									 quint64 installedBuild,
									 TeagramUpdateChannel installedChannel);

[[nodiscard]] TeagramUpdateVerificationResult
VerifyAuthenticatedTeagramUpdateArchive(
	const AuthenticatedTeagramUpdate &package,
	QIODevice &archive,
	const std::function<bool()> &isCancelled = {});

#ifdef TDESKTOP_UNIT_TESTS
[[nodiscard]] TeagramUpdateVerificationResult
VerifyTeagramUpdatePackageForTests(
	const QByteArray &manifest, const QByteArray &signature,
	QByteArrayView archive, quint64 installedBuild,
	TeagramUpdateChannel installedChannel,
	const std::optional<TeagramUpdatePublicKey> &trustedKey);

[[nodiscard]] TeagramUpdateManifestVerificationResult
AuthenticateTeagramUpdateManifestForTests(
	const QByteArray &manifest, const QByteArray &signature,
	quint64 installedBuild, TeagramUpdateChannel installedChannel,
	const std::optional<TeagramUpdatePublicKey> &trustedKey);
#endif // TDESKTOP_UNIT_TESTS

} // namespace Core
