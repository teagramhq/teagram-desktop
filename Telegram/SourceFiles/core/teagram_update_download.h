/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_verification.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QTemporaryFile;

namespace Core {

enum class TeagramUpdateDownloadReason {
	Available,
	NoUpdate,
	Cancelled,
	NetworkFailure,
	InvalidResponse,
	VerificationFailed,
	DisallowedRedirect,
	LimitExceeded,
	StagingFailure,
	ArchiveRejected,
	Busy,
};

enum class TeagramUpdateDownloadStage {
	Discovering,
	Authenticating,
	Downloading,
};

enum class TeagramUpdateTransportFailure {
	None,
	Network,
	Cancelled,
	ConsumerStopped,
	ResponseTooLarge,
};

struct TeagramUpdateTransportResponse {
	TeagramUpdateTransportFailure failure
		= TeagramUpdateTransportFailure::Network;
	int status = 0;
	QUrl redirect;
};

class TeagramUpdateTransport {
  public:
	using ChunkHandler = std::function<bool(QByteArrayView)>;
	using CancellationCheck = std::function<bool()>;

	virtual ~TeagramUpdateTransport() = default;
	[[nodiscard]] virtual TeagramUpdateTransportResponse
	get(const QUrl &url, quint64 maximumBytes, const ChunkHandler &onChunk,
		const CancellationCheck &isCancelled) = 0;
};

class TeagramUpdateCandidate final {
  public:
	~TeagramUpdateCandidate();

	[[nodiscard]] const AuthenticatedTeagramUpdate &metadata() const;
	[[nodiscard]] QString archivePath() const;

  private:
	friend class TeagramUpdateDownloader;
	TeagramUpdateCandidate(AuthenticatedTeagramUpdate metadata,
						   std::unique_ptr<QTemporaryFile> archive);

	AuthenticatedTeagramUpdate _metadata;
	std::unique_ptr<QTemporaryFile> _archive;
};

struct TeagramUpdateDownloadResult {
	TeagramUpdateDownloadReason reason
		= TeagramUpdateDownloadReason::InvalidResponse;
	std::unique_ptr<TeagramUpdateCandidate> candidate;

	[[nodiscard]] bool available() const;
};

using TeagramUpdateProgressHandler
	= std::function<void(TeagramUpdateDownloadStage, int)>;

class TeagramUpdateDownloader final {
  public:
	TeagramUpdateDownloader();
	explicit TeagramUpdateDownloader(
		std::unique_ptr<TeagramUpdateTransport> transport);

#ifdef TDESKTOP_UNIT_TESTS
	TeagramUpdateDownloader(std::unique_ptr<TeagramUpdateTransport> transport,
							std::optional<TeagramUpdatePublicKey> trustedKey,
							QString stagingDirectory);
#endif // TDESKTOP_UNIT_TESTS

	void cancel();
	[[nodiscard]] TeagramUpdateDownloadResult
	checkForUpdates(quint64 installedBuild,
					TeagramUpdateChannel installedChannel,
					TeagramUpdateProgressHandler progress = {});

  private:
	std::unique_ptr<TeagramUpdateTransport> _transport;
	std::atomic_bool _cancelled = false;
	std::atomic_bool _started = false;
#ifdef TDESKTOP_UNIT_TESTS
	std::optional<TeagramUpdatePublicKey> _testTrustedKey;
	QString _testStagingDirectory;
	bool _useTestVerifier = false;
#endif // TDESKTOP_UNIT_TESTS
};

} // namespace Core
