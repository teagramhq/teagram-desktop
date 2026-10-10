/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_download.h"

#include <QtCore/QDir>
#include <QtCore/QFileDevice>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

namespace Core {
namespace {

constexpr auto kReleasePageSize = 100;
constexpr auto kMaximumReleasePages = 10;
constexpr auto kMaximumReleasePageBytes = quint64(2 * 1024 * 1024);
constexpr auto kMaximumReleaseCandidates = 128;
constexpr auto kMaximumHttpRequests = 1040;
constexpr auto kMaximumRedirects = 3;
constexpr auto kMaximumSignatureSize = quint64(64);
constexpr auto kTransferTimeoutMilliseconds = 30000;

const auto kReleaseApiHost = u"api.github.com"_q;
const auto kRepositoryHost = u"github.com"_q;
const auto kReleaseAssetsHost = u"release-assets.githubusercontent.com"_q;
const auto kObjectsHost = u"objects.githubusercontent.com"_q;
const auto kRepositoryPath
	= u"/repos/teagramhq/teagram-desktop/releases"_q;
const auto kManifestAsset = u"teagram-update.json"_q;
const auto kSignatureAsset = u"teagram-update.json.sig"_q;
const auto kTagPrefix = u"teagram-build-"_q;

using Reason = TeagramUpdateDownloadReason;
using TransportFailure = TeagramUpdateTransportFailure;

struct RequestResult {
	Reason reason = Reason::InvalidResponse;
	bool success = false;
};

struct Release {
	QString tag;
	quint64 taggedBuild = 0;
	std::map<QString, int> assetNames;
};

struct InternalResult {
	Reason reason = Reason::InvalidResponse;
	std::optional<AuthenticatedTeagramUpdate> metadata;
	std::unique_ptr<QTemporaryFile> archive;
};

[[nodiscard]] InternalResult Failed(Reason reason) {
	return {reason, std::nullopt, nullptr};
}

[[nodiscard]] bool IsRedirectStatus(int status) {
	return status == 301 || status == 302 || status == 303
		|| status == 307 || status == 308;
}

[[nodiscard]] bool IsAllowedAssetRedirect(const QUrl &url) {
	const auto host = url.host().toLower();
	return url.isValid()
		&& url.scheme() == u"https"_q
		&& (host == kReleaseAssetsHost || host == kObjectsHost)
		&& url.userInfo().isEmpty()
		&& (url.port(-1) == -1 || url.port(-1) == 443)
		&& url.fragment().isEmpty();
}

[[nodiscard]] QUrl ReleaseListUrl(int page) {
	auto result = QUrl(u"https://api.github.com"_q);
	result.setPath(kRepositoryPath);
	auto query = QUrlQuery();
	query.addQueryItem(u"per_page"_q, QString::number(kReleasePageSize));
	query.addQueryItem(u"page"_q, QString::number(page));
	result.setQuery(query);
	return result;
}

[[nodiscard]] QUrl ReleaseAssetUrl(const QString &tag,
									   const QString &name) {
	auto result = QUrl(u"https://github.com"_q);
	result.setPath(u"/teagramhq/teagram-desktop/releases/download/"_q
		+ tag + u"/"_q + name);
	return result;
}

[[nodiscard]] std::optional<quint64> ReleaseBuildFromTag(
	const QString &tag) {
	if (!tag.startsWith(kTagPrefix)) {
		return std::nullopt;
	}
	const auto encoded = tag.mid(kTagPrefix.size()).toLatin1();
	if (encoded.isEmpty() || (encoded.size() > 1 && encoded.front() == '0')) {
		return std::nullopt;
	}
	auto build = quint64(0);
	const auto parsed = std::from_chars(
		encoded.constData(), encoded.constData() + encoded.size(), build);
	if (parsed.ec != std::errc()
		|| parsed.ptr != encoded.constData() + encoded.size()
		|| build == 0) {
		return std::nullopt;
	}
	return build;
}

[[nodiscard]] bool HasAsset(const Release &release, const QString &name) {
	const auto i = release.assetNames.find(name);
	return i != release.assetNames.end() && i->second == 1;
}

[[nodiscard]] bool IsApiReleaseRequest(const QUrl &url) {
	return url.scheme() == u"https"_q
		&& url.host() == kReleaseApiHost
		&& url.path() == kRepositoryPath
		&& url.userInfo().isEmpty()
		&& url.port(-1) == -1
		&& url.fragment().isEmpty();
}

class HttpTeagramUpdateTransport final : public TeagramUpdateTransport {
public:
	[[nodiscard]] TeagramUpdateTransportResponse Get(
		const QUrl &url,
		quint64 maximumBytes,
		const ChunkHandler &onChunk,
		const CancellationCheck &isCancelled) override {
		auto request = QNetworkRequest(url);
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::ManualRedirectPolicy);
		request.setAttribute(
			QNetworkRequest::CacheLoadControlAttribute,
			QNetworkRequest::AlwaysNetwork);
		request.setAttribute(
			QNetworkRequest::CacheSaveControlAttribute,
			false);
		request.setAttribute(
			QNetworkRequest::CookieLoadControlAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(
			QNetworkRequest::CookieSaveControlAttribute,
			QNetworkRequest::Manual);
		request.setTransferTimeout(kTransferTimeoutMilliseconds);
		request.setRawHeader("User-Agent", "Teagram-Desktop");
		request.setRawHeader("Accept-Encoding", "identity");
		request.setRawHeader(
			"Accept",
			url.host() == kReleaseApiHost
				? "application/vnd.github+json"
				: "application/octet-stream");

		auto manager = QNetworkAccessManager();
		auto reply = manager.get(request);
		auto loop = QEventLoop();
		auto timer = QTimer();
		timer.setInterval(50);
		auto failure = TransportFailure::None;
		auto received = quint64(0);
		const auto abort = [&](TransportFailure value) {
			if (failure == TransportFailure::None) {
				failure = value;
				reply->abort();
			}
		};
		const auto checkLength = [&] {
			const auto length
				= reply->header(QNetworkRequest::ContentLengthHeader);
			if (!length.isValid()) {
				return;
			}
			bool ok = false;
			const auto value = length.toLongLong(&ok);
			if (ok && (value < 0 || quint64(value) > maximumBytes)) {
				abort(TransportFailure::ResponseTooLarge);
			}
		};
		const auto readAvailable = [&] {
			checkLength();
			if (failure != TransportFailure::None) {
				return;
			}
			while (reply->bytesAvailable() > 0) {
				if (isCancelled()) {
					abort(TransportFailure::Cancelled);
					return;
				}
				const auto size = std::min<qint64>(
					reply->bytesAvailable(), 16 * 1024);
				const auto chunk = reply->read(size);
				if (chunk.isEmpty()) {
					break;
				}
				if (quint64(chunk.size()) > maximumBytes - received) {
					abort(TransportFailure::ResponseTooLarge);
					return;
				}
				received += quint64(chunk.size());
				const auto status = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				if (status != 200) {
					continue;
				}
				if (!onChunk(QByteArrayView(chunk))) {
					abort(TransportFailure::ConsumerStopped);
					return;
				}
			}
		};
		QObject::connect(reply, &QNetworkReply::metaDataChanged,
			&loop, checkLength);
		QObject::connect(reply, &QIODevice::readyRead, &loop, readAvailable);
		QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
			if (isCancelled()) {
				abort(TransportFailure::Cancelled);
			}
		});
		QObject::connect(reply, &QNetworkReply::finished, &loop, [&] {
			readAvailable();
			loop.quit();
		});
		timer.start();
		if (!reply->isFinished()) {
			loop.exec();
		} else {
			readAvailable();
		}
		timer.stop();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto redirect = reply->attribute(
			QNetworkRequest::RedirectionTargetAttribute).toUrl();
		if (isCancelled()) {
			failure = TransportFailure::Cancelled;
		} else if (failure == TransportFailure::None
			&& reply->error() != QNetworkReply::NoError) {
			failure = TransportFailure::Network;
		}
		reply->deleteLater();
		return {failure, status, redirect};
	}
};

class DownloadRunner final {
public:
	using ManifestVerifier = std::function<
		TeagramUpdateManifestVerificationResult(
			const QByteArray &,
			const QByteArray &,
			quint64,
			TeagramUpdateChannel)>;

	DownloadRunner(TeagramUpdateTransport &transport,
					const std::atomic_bool &cancelled,
					QString stagingDirectory,
					ManifestVerifier verify,
					TeagramUpdateProgressHandler progress)
	: _transport(transport)
	, _cancelled(cancelled)
	, _stagingDirectory(std::move(stagingDirectory))
	, _verify(std::move(verify))
	, _progress(std::move(progress)) {
	}

	[[nodiscard]] InternalResult Run(
		quint64 installedBuild,
		TeagramUpdateChannel installedChannel) {
		auto releases = std::vector<Release>();
		auto seenTags = std::map<QString, bool>();
		for (auto page = 1; page <= kMaximumReleasePages; ++page) {
			if (IsCancelled()) {
				return Failed(Reason::Cancelled);
			}
			EmitProgress(TeagramUpdateDownloadStage::Discovering,
				(page - 1) * 100 / kMaximumReleasePages);
			auto body = QByteArray();
			const auto result = FetchBytes(
				ReleaseListUrl(page),
				kMaximumReleasePageBytes,
				false,
				&body);
			if (!result.success) {
				return Failed(result.reason);
			}
			const auto parsed = ParseReleasePage(body, &releases, &seenTags);
			if (!parsed.valid) {
				return Failed(Reason::InvalidResponse);
			}
			if (parsed.count < kReleasePageSize) {
				break;
			} else if (page == kMaximumReleasePages) {
				return Failed(Reason::LimitExceeded);
			}
		}
		std::sort(releases.begin(), releases.end(), [](
								const Release &a,
								const Release &b) {
			return a.taggedBuild > b.taggedBuild;
		});
		auto candidateCount = 0;
		auto rejectedManifest = false;
		auto rejectedIdentity = false;
		for (const auto &release : releases) {
			if (IsCancelled()) {
				return Failed(Reason::Cancelled);
			}
			if (release.taggedBuild <= installedBuild
				|| !HasAsset(release, kManifestAsset)
				|| !HasAsset(release, kSignatureAsset)) {
				continue;
			}
			if (++candidateCount > kMaximumReleaseCandidates) {
				return Failed(Reason::LimitExceeded);
			}
			EmitProgress(
				TeagramUpdateDownloadStage::Authenticating,
				(candidateCount - 1) * 100 / kMaximumReleaseCandidates);
			auto manifest = QByteArray();
			const auto manifestResult = FetchBytes(
				ReleaseAssetUrl(release.tag, kManifestAsset),
				kMaximumTeagramUpdateManifestSize,
				true,
				&manifest);
			if (!manifestResult.success) {
				return Failed(manifestResult.reason);
			}
			auto signature = QByteArray();
			const auto signatureResult = FetchBytes(
				ReleaseAssetUrl(release.tag, kSignatureAsset),
				kMaximumSignatureSize,
				true,
				&signature);
			if (!signatureResult.success) {
				return Failed(signatureResult.reason);
			}
			const auto authenticated = _verify(
				manifest,
				signature,
				installedBuild,
				installedChannel);
			if (!authenticated.authenticated() || !authenticated.package) {
				if (authenticated.reason
						!= TeagramUpdateVerificationReason::NotNewer
					&& authenticated.reason
						!= TeagramUpdateVerificationReason::IneligibleChannel) {
					rejectedManifest = true;
				}
				continue;
			}
			if (authenticated.package->build() != release.taggedBuild) {
				rejectedIdentity = true;
				continue;
			}
			if (!HasAsset(release, authenticated.package->assetName())) {
				return Failed(Reason::ArchiveRejected);
			}
			return DownloadArchive(*authenticated.package);
		}
		return Failed(rejectedIdentity
			? Reason::InvalidResponse
			: (rejectedManifest ? Reason::VerificationFailed : Reason::NoUpdate));
	}

private:
	struct PageResult {
		bool valid = false;
		qsizetype count = 0;
	};

	[[nodiscard]] bool IsCancelled() const {
		return _cancelled.load(std::memory_order_relaxed);
	}

	void EmitProgress(TeagramUpdateDownloadStage stage, int value) const {
		if (_progress) {
			_progress(stage, std::clamp(value, 0, 100));
		}
	}

	[[nodiscard]] RequestResult Request(
		QUrl url,
		quint64 maximumBytes,
		bool allowAssetRedirects,
		const TeagramUpdateTransport::ChunkHandler &onChunk) {
		if ((!allowAssetRedirects && !IsApiReleaseRequest(url))
			|| (allowAssetRedirects
				&& (url.scheme() != u"https"_q
					|| url.host() != kRepositoryHost
					|| !url.path().startsWith(
						u"/teagramhq/teagram-desktop/releases/download/"_q)
					|| !url.userInfo().isEmpty()
					|| url.port(-1) != -1
					|| !url.fragment().isEmpty()))) {
			return {Reason::InvalidResponse, false};
		}
		auto redirects = 0;
		while (true) {
			if (IsCancelled()) {
				return {Reason::Cancelled, false};
			}
			if (_requests >= kMaximumHttpRequests) {
				return {Reason::LimitExceeded, false};
			}
			++_requests;
			const auto response = _transport.Get(
				url,
				maximumBytes,
				onChunk,
				[&] { return IsCancelled(); });
			if (IsCancelled()
				|| response.failure == TransportFailure::Cancelled) {
				return {Reason::Cancelled, false};
			}
			if (response.failure == TransportFailure::ResponseTooLarge) {
				return {Reason::LimitExceeded, false};
			}
			if (response.failure == TransportFailure::Network) {
				return {Reason::NetworkFailure, false};
			}
			if (response.failure == TransportFailure::ConsumerStopped) {
				return {Reason::InvalidResponse, false};
			}
			if (IsRedirectStatus(response.status)) {
				if (!allowAssetRedirects) {
					return {Reason::DisallowedRedirect, false};
				}
				if (redirects == kMaximumRedirects) {
					return {Reason::LimitExceeded, false};
				}
				const auto target = url.resolved(response.redirect);
				if (!IsAllowedAssetRedirect(target)) {
					return {Reason::DisallowedRedirect, false};
				}
				url = target;
				++redirects;
				continue;
			}
			if (response.status != 200 || !response.redirect.isEmpty()) {
				return {Reason::InvalidResponse, false};
			}
			return {Reason::Available, true};
		}
	}

	[[nodiscard]] RequestResult FetchBytes(
		const QUrl &url,
		quint64 maximumBytes,
		bool allowAssetRedirects,
		QByteArray *result) {
		auto tooLarge = false;
		const auto fetched = Request(
			url,
			maximumBytes,
			allowAssetRedirects,
			[&](QByteArrayView chunk) {
				if (quint64(result->size()) > maximumBytes
					|| quint64(chunk.size())
						> maximumBytes - quint64(result->size())
					|| chunk.size() > std::numeric_limits<int>::max()
						- result->size()) {
					tooLarge = true;
					return false;
				}
				result->append(chunk.data(), int(chunk.size()));
				return true;
			});
		if (!fetched.success && tooLarge) {
			return {Reason::LimitExceeded, false};
		}
		return fetched;
	}

	[[nodiscard]] PageResult ParseReleasePage(
		const QByteArray &body,
		std::vector<Release> *releases,
		std::map<QString, bool> *seenTags) const {
		auto error = QJsonParseError{0, QJsonParseError::NoError};
		const auto document = QJsonDocument::fromJson(body, &error);
		if (error.error != QJsonParseError::NoError || !document.isArray()) {
			return {};
		}
		const auto array = document.array();
		if (array.size() > kReleasePageSize) {
			return {};
		}
		for (const auto &value : array) {
			if (!value.isObject()) {
				return {};
			}
			const auto fields = value.toObject();
			const auto tag = fields.value(u"tag_name"_q);
			if (!tag.isString()) {
				continue;
			}
			const auto tagName = tag.toString();
			const auto build = ReleaseBuildFromTag(tagName);
			if (!build) {
				continue;
			}
			if (!seenTags->emplace(tagName, true).second) {
				return {};
			}
			const auto assets = fields.value(u"assets"_q);
			if (!assets.isArray()) {
				continue;
			}
			auto release = Release{tagName, *build, {}};
			for (const auto &asset : assets.toArray()) {
				if (!asset.isObject()) {
					continue;
				}
				const auto name = asset.toObject().value(u"name"_q);
				if (name.isString()) {
					++release.assetNames[name.toString()];
				}
			}
			if (HasAsset(release, kManifestAsset)
				&& HasAsset(release, kSignatureAsset)) {
				releases->push_back(std::move(release));
			}
		}
		return {true, array.size()};
	}

	[[nodiscard]] InternalResult DownloadArchive(
		const AuthenticatedTeagramUpdate &metadata) {
		if (metadata.assetSize() > kMaximumTeagramUpdateArchiveSize) {
			return Failed(Reason::LimitExceeded);
		}
		const auto pattern = _stagingDirectory.isEmpty()
			? QDir::tempPath() + u"/teagram-update-XXXXXX"_q
			: QDir(_stagingDirectory).filePath(u"teagram-update-XXXXXX"_q);
		auto archive = std::make_unique<QTemporaryFile>(pattern);
		archive->setAutoRemove(true);
		if (!archive->open()
			|| !archive->setPermissions(
				QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
			return Failed(Reason::StagingFailure);
		}
		auto received = quint64(0);
		auto oversized = false;
		auto writeFailed = false;
		EmitProgress(TeagramUpdateDownloadStage::Downloading, 0);
		const auto fetched = Request(
			ReleaseAssetUrl(
				u"teagram-build-%1"_q.arg(metadata.build()),
				metadata.assetName()),
			metadata.assetSize(),
			true,
			[&](QByteArrayView chunk) {
				if (quint64(chunk.size()) > metadata.assetSize() - received) {
					oversized = true;
					return false;
				}
				const auto size = qint64(chunk.size());
				if (archive->write(chunk.data(), size) != size) {
					writeFailed = true;
					return false;
				}
				received += quint64(chunk.size());
				const auto percent = int(
					(received * 100) / metadata.assetSize());
				EmitProgress(
					TeagramUpdateDownloadStage::Downloading,
					std::min(percent, 99));
				return true;
			});
		if (IsCancelled() || fetched.reason == Reason::Cancelled) {
			return Failed(Reason::Cancelled);
		} else if (writeFailed) {
			return Failed(Reason::StagingFailure);
		} else if (oversized) {
			return Failed(Reason::ArchiveRejected);
		} else if (!fetched.success) {
			return Failed(fetched.reason);
		} else if (received != metadata.assetSize() || !archive->flush()) {
			return Failed(Reason::ArchiveRejected);
		}
		const auto verification = VerifyAuthenticatedTeagramUpdateArchive(
			metadata, *archive, [&] { return IsCancelled(); });
		if (verification.reason
			== TeagramUpdateVerificationReason::Cancelled) {
			return Failed(Reason::Cancelled);
		}
		if (!verification.eligible()) {
			return Failed(
				verification.reason
					== TeagramUpdateVerificationReason::ArchiveTooLarge
					? Reason::LimitExceeded
					: Reason::ArchiveRejected);
		}
		archive->close();
		EmitProgress(TeagramUpdateDownloadStage::Downloading, 100);
		return {
			Reason::Available,
			metadata,
			std::move(archive),
		};
	}

	TeagramUpdateTransport &_transport;
	const std::atomic_bool &_cancelled;
	QString _stagingDirectory;
	ManifestVerifier _verify;
	TeagramUpdateProgressHandler _progress;
	int _requests = 0;
};

} // namespace

TeagramUpdateCandidate::TeagramUpdateCandidate(
	AuthenticatedTeagramUpdate metadata,
	std::unique_ptr<QTemporaryFile> archive)
: _metadata(std::move(metadata))
, _archive(std::move(archive)) {
}

TeagramUpdateCandidate::~TeagramUpdateCandidate() = default;

const AuthenticatedTeagramUpdate &TeagramUpdateCandidate::metadata() const {
	return _metadata;
}

QString TeagramUpdateCandidate::archivePath() const {
	return _archive ? _archive->fileName() : QString();
}

bool TeagramUpdateDownloadResult::available() const {
	return reason == TeagramUpdateDownloadReason::Available
		&& candidate != nullptr;
}

TeagramUpdateDownloader::TeagramUpdateDownloader()
: TeagramUpdateDownloader(std::make_unique<HttpTeagramUpdateTransport>()) {
}

TeagramUpdateDownloader::TeagramUpdateDownloader(
	std::unique_ptr<TeagramUpdateTransport> transport)
: _transport(std::move(transport)) {
}

#ifdef TDESKTOP_UNIT_TESTS
TeagramUpdateDownloader::TeagramUpdateDownloader(
	std::unique_ptr<TeagramUpdateTransport> transport,
	std::optional<TeagramUpdatePublicKey> trustedKey,
	QString stagingDirectory)
: _transport(std::move(transport))
, _testTrustedKey(std::move(trustedKey))
, _testStagingDirectory(std::move(stagingDirectory))
, _useTestVerifier(true) {
}
#endif // TDESKTOP_UNIT_TESTS

void TeagramUpdateDownloader::Cancel() {
	_cancelled.store(true, std::memory_order_relaxed);
}

TeagramUpdateDownloadResult TeagramUpdateDownloader::CheckForUpdates(
	quint64 installedBuild,
	TeagramUpdateChannel installedChannel,
	TeagramUpdateProgressHandler progress) {
	if (_started.exchange(true, std::memory_order_relaxed)) {
		return {Reason::Busy, nullptr};
	}
	if (!_transport) {
		return {Reason::InvalidResponse, nullptr};
	}
	const auto verify = [&](
						const QByteArray &manifest,
						const QByteArray &signature,
						quint64 build,
						TeagramUpdateChannel channel) {
#ifdef TDESKTOP_UNIT_TESTS
		if (_useTestVerifier) {
			return AuthenticateTeagramUpdateManifestForTests(
				manifest, signature, build, channel, _testTrustedKey);
		}
#endif // TDESKTOP_UNIT_TESTS
		return AuthenticateTeagramUpdateManifest(
			manifest, signature, build, channel);
	};
#ifdef TDESKTOP_UNIT_TESTS
	const auto stagingDirectory = _testStagingDirectory;
#else
	const auto stagingDirectory = QString();
#endif // TDESKTOP_UNIT_TESTS
	auto runner = DownloadRunner(
		*_transport,
		_cancelled,
		stagingDirectory,
		verify,
		std::move(progress));
	auto result = runner.Run(installedBuild, installedChannel);
	if (_cancelled.load(std::memory_order_relaxed)) {
		return {Reason::Cancelled, nullptr};
	}
	if (result.reason != Reason::Available
		|| !result.metadata
		|| !result.archive) {
		return {result.reason, nullptr};
	}
	return {
		Reason::Available,
		std::unique_ptr<TeagramUpdateCandidate>(
			new TeagramUpdateCandidate(
				std::move(*result.metadata),
				std::move(result.archive))),
	};
}

} // namespace Core
