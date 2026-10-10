/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_update_download.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrlQuery>

#include <openssl/evp.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace Core;

namespace {

struct PkeyDeleter {
	void operator()(EVP_PKEY *value) const;
};

struct PkeyContextDeleter {
	void operator()(EVP_PKEY_CTX *value) const;
};

struct DigestContextDeleter {
	void operator()(EVP_MD_CTX *value) const;
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

struct TestSigningKey {
	Pkey privateKey;
	TeagramUpdatePublicKey publicKey = {};
};

[[nodiscard]] std::optional<TestSigningKey> GenerateTestSigningKey() {
	auto context = PkeyContext(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr));
	if (!context || EVP_PKEY_keygen_init(context.get()) != 1) {
		CHECK(false);
		return std::nullopt;
	}
	EVP_PKEY *rawKey = nullptr;
	if (EVP_PKEY_keygen(context.get(), &rawKey) != 1) {
		CHECK(false);
		return std::nullopt;
	}
	auto privateKey = Pkey(rawKey);
	auto publicKey = TeagramUpdatePublicKey();
	auto size = publicKey.size();
	if (EVP_PKEY_get_raw_public_key(privateKey.get(), publicKey.data(), &size)
		!= 1
		|| size != publicKey.size()) {
		CHECK(false);
		return std::nullopt;
	}
	return TestSigningKey{std::move(privateKey), publicKey};
}

[[nodiscard]] QByteArray SignManifest(const QByteArray &manifest,
									  EVP_PKEY *key) {
	auto message = QByteArrayLiteral("teagram-update-v1");
	message.append('\0');
	message.append(manifest);
	auto context = DigestContext(EVP_MD_CTX_new());
	if (!context
		|| EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key)
			   != 1) {
		CHECK(false);
		return {};
	}
	auto size = size_t(0);
	if (EVP_DigestSign(
			context.get(), nullptr, &size,
		reinterpret_cast<const unsigned char *>(message.constData()),
		size_t(message.size()))
		!= 1) {
		CHECK(false);
		return {};
	}
	auto signature = QByteArray(int(size), Qt::Uninitialized);
	if (EVP_DigestSign(
			context.get(), reinterpret_cast<unsigned char *>(signature.data()),
			&size, reinterpret_cast<const unsigned char *>(message.constData()),
		size_t(message.size()))
		!= 1) {
		CHECK(false);
		return {};
	}
	signature.resize(int(size));
	return signature;
}

[[nodiscard]] QByteArray Manifest(const QByteArray &channel,
								  const QByteArray &build,
								  const QByteArray &archive,
								  const QByteArray &size = {}) {
	const auto hash = QCryptographicHash::hash(
		archive, QCryptographicHash::Sha256).toHex();
	const auto assetSize = size.isEmpty()
		? QByteArray::number(archive.size())
		: size;
	return QByteArrayLiteral(
			   "{\"format\":1,\"product\":\"io.teagram.desktop\","
			   "\"repo\":\"teagramhq/teagram-desktop\",\"arch\":\"arm64\","
			   "\"channel\":\"")
		   + channel + QByteArrayLiteral("\",\"build\":\"") + build
		   + QByteArrayLiteral("\",\"commit\":\"")
		   + QByteArrayLiteral("0123456789abcdef0123456789abcdef01234567")
		   + QByteArrayLiteral("\",\"asset_name\":\"Teagram-macOS-arm64-")
		   + build
		   + QByteArrayLiteral(".zip\",\"asset_size\":\"") + assetSize
		   + QByteArrayLiteral("\",\"asset_sha256\":\"") + hash
		   + QByteArrayLiteral("\",\"min_os\":\"13.0\"}");
}

struct FakeResponse {
	int status = 200;
	QUrl redirect;
	QByteArray body;
	TeagramUpdateTransportFailure failure
		= TeagramUpdateTransportFailure::None;
};

[[nodiscard]] FakeResponse ResponseWithBody(QByteArray body) {
	auto result = FakeResponse();
	result.body = std::move(body);
	return result;
}

[[nodiscard]] FakeResponse RedirectResponse(QUrl url) {
	auto result = FakeResponse();
	result.status = 302;
	result.redirect = std::move(url);
	return result;
}

[[nodiscard]] FakeResponse NetworkFailureResponse() {
	auto result = FakeResponse();
	result.failure = TeagramUpdateTransportFailure::Network;
	return result;
}

class FixtureTransport final : public TeagramUpdateTransport {
public:
	using AfterChunk = std::function<void(const QUrl &)>;

	void addResponse(const QUrl &url, FakeResponse response);

	void addAsset(
		const QByteArray &tag,
		const QByteArray &name,
		FakeResponse response);

	void addRelease(const QByteArray &tag, const QByteArray &channel,
				const QByteArray &build, EVP_PKEY *key,
				const QByteArray &archive, bool prerelease = false,
				const QByteArray &signedSize = {});

	void addOldReleases(int count);

	void addReleaseList();

	TeagramUpdateTransportResponse get(
		const QUrl &url,
		quint64 maximumBytes,
		const ChunkHandler &onChunk,
		const CancellationCheck &isCancelled) override;

	AfterChunk _afterChunk;
	std::vector<QUrl> _requests;

private:
	[[nodiscard]] static QString Key(const QUrl &url);

	std::map<QString, FakeResponse> _responses;
	QJsonArray _releases;

};

void PkeyDeleter::operator()(EVP_PKEY *value) const {
	EVP_PKEY_free(value);
}

void PkeyContextDeleter::operator()(
	EVP_PKEY_CTX *value) const {
	EVP_PKEY_CTX_free(value);
}

void DigestContextDeleter::operator()(
	EVP_MD_CTX *value) const {
	EVP_MD_CTX_free(value);
}

void FixtureTransport::addResponse(const QUrl &url, FakeResponse response) {
	_responses.insert_or_assign(Key(url), std::move(response));
}

void FixtureTransport::addAsset(
	const QByteArray &tag,
	const QByteArray &name,
	FakeResponse response) {
	auto url = QUrl(
		u"https://github.com/teagramhq/teagram-desktop/releases/download/"_q
		+ QString::fromUtf8(tag) + u"/"_q + QString::fromUtf8(name));
	addResponse(url, std::move(response));
}

void FixtureTransport::addRelease(
	const QByteArray &tag,
	const QByteArray &channel,
	const QByteArray &build,
	EVP_PKEY *key,
	const QByteArray &archive,
	bool prerelease,
	const QByteArray &signedSize) {
	const auto manifest = Manifest(channel, build, archive, signedSize);
	const auto signature = SignManifest(manifest, key);
	addAsset(tag, "teagram-update.json", ResponseWithBody(manifest));
	addAsset(tag, "teagram-update.json.sig", ResponseWithBody(signature));
	addAsset(tag, QByteArray("Teagram-macOS-arm64-") + build + ".zip",
			ResponseWithBody(archive));
	auto assets = QJsonArray();
	assets.append(QJsonObject{{u"name"_q, u"teagram-update.json"_q}});
	assets.append(QJsonObject{{u"name"_q, u"teagram-update.json.sig"_q}});
	assets.append(QJsonObject{
		{u"name"_q,
		 u"Teagram-macOS-arm64-%1.zip"_q.arg(QString::fromUtf8(build))},
	});
	_releases.append(QJsonObject{
		{u"tag_name"_q, QString::fromUtf8(tag)},
		{u"prerelease"_q, prerelease},
		{u"assets"_q, assets},
	});
}

void FixtureTransport::addOldReleases(int count) {
	for (auto i = 0; i != count; ++i) {
		_releases.append(QJsonObject{
			{u"tag_name"_q, u"teagram-build-%1"_q.arg(i + 2000)},
			{u"assets"_q, QJsonArray()},
		});
	}
}

void FixtureTransport::addReleaseList() {
	for (auto page = 0;
		 qsizetype(page) * 100 < _releases.size();
		 ++page) {
		const auto start = qsizetype(page) * 100;
		const auto end = std::min(start + 100, _releases.size());
		auto releases = QJsonArray();
		for (auto i = start; i < end; ++i) {
			releases.append(_releases[i]);
		}
		const auto body = QJsonDocument(releases)
			.toJson(QJsonDocument::Compact);
		auto url = QUrl(
			u"https://api.github.com/repos/teagramhq/teagram-desktop/releases"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"per_page"_q, u"100"_q);
		query.addQueryItem(u"page"_q, QString::number(page + 1));
		url.setQuery(query);
		addResponse(url, ResponseWithBody(body));
	}
}

TeagramUpdateTransportResponse FixtureTransport::get(
	const QUrl &url,
	quint64 maximumBytes,
	const TeagramUpdateTransport::ChunkHandler &onChunk,
	const TeagramUpdateTransport::CancellationCheck &isCancelled) {
	_requests.push_back(url);
	const auto i = _responses.find(Key(url));
	if (i == _responses.end()) {
		return {TeagramUpdateTransportFailure::Network, 0, {}};
	}
	const auto &response = i->second;
	if (response.failure != TeagramUpdateTransportFailure::None) {
		return {response.failure, response.status, response.redirect};
	}
	if (!response.redirect.isEmpty()) {
		return {TeagramUpdateTransportFailure::None,
			response.status,
			response.redirect};
	}
	auto received = quint64(0);
	for (auto offset = qsizetype(0); offset < response.body.size();) {
		if (isCancelled()) {
			return {TeagramUpdateTransportFailure::Cancelled,
				response.status,
				{}};
		}
		const auto size = std::min<qsizetype>(
			16384, response.body.size() - offset);
		const auto chunk = QByteArrayView(
			response.body.constData() + offset, size);
		if (quint64(size) > maximumBytes - received) {
			return {TeagramUpdateTransportFailure::ResponseTooLarge,
				response.status,
				{}};
		}
		if (!onChunk(chunk)) {
			return {TeagramUpdateTransportFailure::ConsumerStopped,
				response.status,
				{}};
		}
		if (_afterChunk) {
			_afterChunk(url);
		}
		received += quint64(size);
		offset += size;
	}
	return {TeagramUpdateTransportFailure::None,
		response.status,
		{}};
}

QString FixtureTransport::Key(const QUrl &url) {
	auto result = url.host() + url.path();
	if (url.host() == u"api.github.com"_q) {
		result += u"?page="_q
			+ QUrlQuery(url).queryItemValue(u"page"_q);
	}
	return result;
}

[[nodiscard]] bool HasOnlyFixedSourceHosts(
	const std::vector<QUrl> &requests) {
	for (const auto &url : requests) {
		const auto host = url.host();
		if (host == u"api.github.com"_q) {
			if (url.path()
				!= u"/repos/teagramhq/teagram-desktop/releases"_q) {
				return false;
			}
		} else if (host == u"github.com"_q) {
			if (!url.path().startsWith(
				u"/teagramhq/teagram-desktop/releases/download/"_q)) {
				return false;
			}
		} else if (host != u"release-assets.githubusercontent.com"_q
				&& host != u"objects.githubusercontent.com"_q) {
			return false;
		}
		if (url.scheme() != u"https"_q) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &data) {
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

} // namespace

TEST_CASE(TeagramUpdateDownloadAcceptsFixedSource) {
	const auto signingKey = GenerateTestSigningKey();
	if (!signingKey) {
		return;
	}
	const auto archive = QByteArrayLiteral("verified teagram archive");
	const auto trustedKey
		= std::optional<TeagramUpdatePublicKey>(signingKey->publicKey);
	auto transport = std::make_unique<FixtureTransport>();
	auto *fixture = transport.get();
	fixture->addOldReleases(100);
	fixture->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive, false);
	fixture->addRelease("teagram-build-102", "main", "102",
		signingKey->privateKey.get(), archive, true);
	fixture->addRelease("teagram-build-100", "main", "100",
		signingKey->privateKey.get(), archive, true);
	fixture->addReleaseList();
	const auto archiveRedirect = QUrl(
		u"https://release-assets.githubusercontent.com/download/verified"_q);
	const auto archiveObjectsRedirect = QUrl(
		u"https://objects.githubusercontent.com/download/verified"_q);
	const auto archiveFinalRedirect = QUrl(
		u"https://release-assets.githubusercontent.com/download/final"_q);
	fixture->addAsset("teagram-build-102",
		"Teagram-macOS-arm64-102.zip",
		RedirectResponse(archiveRedirect));
	fixture->addResponse(
		archiveRedirect, RedirectResponse(archiveObjectsRedirect));
	fixture->addResponse(
		archiveObjectsRedirect, RedirectResponse(archiveFinalRedirect));
	fixture->addResponse(archiveFinalRedirect, ResponseWithBody(archive));
	QTemporaryDir staging;
	CHECK(staging.isValid());
	TeagramUpdateDownloader dev(std::move(transport), trustedKey,
		staging.path());
	auto progress = std::vector<int>();
	auto devResult = dev.checkForUpdates(
		100, TeagramUpdateChannel::Dev,
		[&](TeagramUpdateDownloadStage, int value) {
			progress.push_back(value);
		});
	CHECK(devResult.reason == TeagramUpdateDownloadReason::Available);
	CHECK(devResult.candidate != nullptr);
	CHECK(!progress.empty());
	for (const auto value : progress) {
		CHECK(value >= 0 && value <= 100);
	}
	if (devResult.candidate) {
		CHECK_EQ(devResult.candidate->metadata().build(), quint64(102));
		CHECK(devResult.candidate->metadata().channel()
			== TeagramUpdateChannel::Main);
		QFile downloaded(devResult.candidate->archivePath());
		CHECK(downloaded.open(QIODevice::ReadOnly));
		CHECK_EQ(downloaded.readAll(), archive);
		const auto permissions = QFileInfo(
			devResult.candidate->archivePath()).permissions();
		const auto ownerPermissions = permissions
			& (QFileDevice::ReadOwner
				| QFileDevice::WriteOwner
				| QFileDevice::ExeOwner);
		const auto groupAndOtherPermissions = permissions
			& (QFileDevice::ReadGroup
				| QFileDevice::WriteGroup
				| QFileDevice::ExeGroup
				| QFileDevice::ReadOther
				| QFileDevice::WriteOther
				| QFileDevice::ExeOther);
		CHECK(ownerPermissions
			== (QFileDevice::ReadOwner | QFileDevice::WriteOwner));
		CHECK(groupAndOtherPermissions == QFileDevice::Permissions());
	}
	CHECK(HasOnlyFixedSourceHosts(fixture->_requests));
	CHECK_EQ(QDir(staging.path()).entryList(
		QDir::Files | QDir::NoDotAndDotDot).size(), 1);
	devResult.candidate.reset();
	CHECK(QDir(staging.path()).entryList(
		QDir::Files | QDir::NoDotAndDotDot).isEmpty());

	auto devOnly = std::make_unique<FixtureTransport>();
	devOnly->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive, false);
	devOnly->addReleaseList();
	TeagramUpdateDownloader dev101(std::move(devOnly), trustedKey,
		staging.path());
	auto dev101Result = dev101.checkForUpdates(
		100, TeagramUpdateChannel::Dev);
	CHECK(dev101Result.reason == TeagramUpdateDownloadReason::Available);
	CHECK(dev101Result.candidate != nullptr);
	if (dev101Result.candidate) {
		CHECK_EQ(dev101Result.candidate->metadata().build(), quint64(101));
		CHECK(dev101Result.candidate->metadata().channel()
			== TeagramUpdateChannel::Dev);
	}
	dev101Result.candidate.reset();
	CHECK(QDir(staging.path()).entryList(
		QDir::Files | QDir::NoDotAndDotDot).isEmpty());

	auto mainDevOnly = std::make_unique<FixtureTransport>();
	auto *devOnlyFixture = mainDevOnly.get();
	devOnlyFixture->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive, false);
	devOnlyFixture->addReleaseList();
	TeagramUpdateDownloader main(std::move(mainDevOnly), trustedKey,
		staging.path());
	const auto mainResult = main.checkForUpdates(
		100, TeagramUpdateChannel::Main);
	CHECK(mainResult.reason == TeagramUpdateDownloadReason::NoUpdate);
	CHECK(mainResult.candidate == nullptr);
	CHECK(devOnlyFixture->_requests.size() == 3);
}

TEST_CASE(TeagramUpdateDownloadFailsWithoutMutation) {
	const auto signingKey = GenerateTestSigningKey();
	const auto wrongKey = GenerateTestSigningKey();
	if (!signingKey || !wrongKey) {
		return;
	}
	const auto archive = QByteArrayLiteral("verified teagram archive");
	auto modified = archive;
	modified[0] = 'X';
	const auto trustedKey
		= std::optional<TeagramUpdatePublicKey>(signingKey->publicKey);
	const auto wrongPublicKey
		= std::optional<TeagramUpdatePublicKey>(wrongKey->publicKey);
	QTemporaryDir app;
	QTemporaryDir profile;
	QTemporaryDir staging;
	CHECK(app.isValid());
	CHECK(profile.isValid());
	CHECK(staging.isValid());
	const auto appCanaryPath = app.path() + u"/bundle-canary"_q;
	const auto appCanaryContents = QByteArrayLiteral("installed app stays intact");
	const auto canaryPath = profile.path() + u"/profile-canary"_q;
	const auto canaryContents = QByteArrayLiteral("profile remains untouched");
	CHECK(WriteFile(appCanaryPath, appCanaryContents));
	CHECK(WriteFile(canaryPath, canaryContents));
	const auto runFailure = [&](std::unique_ptr<FixtureTransport> fixture,
								const std::optional<TeagramUpdatePublicKey> &key,
								TeagramUpdateDownloadReason expected,
								bool cancelDuringArchive = false,
								bool expectNoArchiveRequest = false) {
		fixture->addReleaseList();
		auto *rawFixture = fixture.get();
		TeagramUpdateDownloader downloader(
			std::move(fixture), key, staging.path());
		if (cancelDuringArchive) {
			rawFixture->_afterChunk = [&](const QUrl &url) {
				if (url.path().endsWith(u".zip"_q)) {
					downloader.cancel();
				}
			};
		}
		const auto result = downloader.checkForUpdates(
			100, TeagramUpdateChannel::Dev);
		CHECK(result.reason == expected);
		CHECK(result.candidate == nullptr);
		if (expectNoArchiveRequest) {
			CHECK(std::none_of(
				rawFixture->_requests.begin(),
				rawFixture->_requests.end(),
				[](const QUrl &url) { return url.path().endsWith(u".zip"_q); }));
		}
		CHECK(QDir(staging.path()).entryList(
			QDir::Files | QDir::NoDotAndDotDot).isEmpty());
		QFile canary(canaryPath);
		CHECK(canary.open(QIODevice::ReadOnly));
		CHECK_EQ(canary.readAll(), canaryContents);
		QFile appCanary(appCanaryPath);
		CHECK(appCanary.open(QIODevice::ReadOnly));
		CHECK_EQ(appCanary.readAll(), appCanaryContents);
	};

	auto modifiedArchive = std::make_unique<FixtureTransport>();
	modifiedArchive->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	modifiedArchive->addAsset("teagram-build-101",
		"Teagram-macOS-arm64-101.zip", ResponseWithBody(modified));
	runFailure(std::move(modifiedArchive), trustedKey,
		TeagramUpdateDownloadReason::ArchiveRejected);

	auto unreachable = std::make_unique<FixtureTransport>();
	auto api = QUrl(u"https://api.github.com/repos/teagramhq/teagram-desktop/releases?per_page=100&page=1"_q);
	unreachable->addResponse(api, NetworkFailureResponse());
	runFailure(std::move(unreachable), trustedKey,
		TeagramUpdateDownloadReason::NetworkFailure);

	auto redirect = std::make_unique<FixtureTransport>();
	redirect->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	redirect->addAsset("teagram-build-101", "teagram-update.json",
		RedirectResponse(QUrl(u"https://evil.example/update.json"_q)));
	runFailure(std::move(redirect), trustedKey,
		TeagramUpdateDownloadReason::DisallowedRedirect);

	auto downgradedRedirect = std::make_unique<FixtureTransport>();
	downgradedRedirect->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	downgradedRedirect->addAsset("teagram-build-101", "teagram-update.json",
		RedirectResponse(QUrl(
			u"http://release-assets.githubusercontent.com/update.json"_q)));
	runFailure(std::move(downgradedRedirect), trustedKey,
		TeagramUpdateDownloadReason::DisallowedRedirect);

	auto tooManyRedirects = std::make_unique<FixtureTransport>();
	tooManyRedirects->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	const auto firstHop = QUrl(
		u"https://release-assets.githubusercontent.com/first"_q);
	const auto secondHop = QUrl(
		u"https://objects.githubusercontent.com/second"_q);
	const auto thirdHop = QUrl(
		u"https://release-assets.githubusercontent.com/third"_q);
	const auto fourthHop = QUrl(
		u"https://objects.githubusercontent.com/fourth"_q);
	tooManyRedirects->addAsset("teagram-build-101", "teagram-update.json",
		RedirectResponse(firstHop));
	tooManyRedirects->addResponse(firstHop, RedirectResponse(secondHop));
	tooManyRedirects->addResponse(secondHop, RedirectResponse(thirdHop));
	tooManyRedirects->addResponse(thirdHop, RedirectResponse(fourthHop));
	runFailure(std::move(tooManyRedirects), trustedKey,
		TeagramUpdateDownloadReason::LimitExceeded);

	auto invalidKey = std::make_unique<FixtureTransport>();
	invalidKey->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	runFailure(std::move(invalidKey), wrongPublicKey,
		TeagramUpdateDownloadReason::VerificationFailed, false, true);

	auto oversized = std::make_unique<FixtureTransport>();
	oversized->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive,
		false, QByteArrayLiteral("536870913"));
	runFailure(std::move(oversized), trustedKey,
		TeagramUpdateDownloadReason::LimitExceeded);

	auto oversizedManifest = std::make_unique<FixtureTransport>();
	oversizedManifest->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	oversizedManifest->addAsset("teagram-build-101",
		"teagram-update.json",
		ResponseWithBody(QByteArray(
			kMaximumTeagramUpdateManifestSize + 1, 'x')));
	runFailure(std::move(oversizedManifest), trustedKey,
		TeagramUpdateDownloadReason::LimitExceeded);

	auto wrongTag = std::make_unique<FixtureTransport>();
	wrongTag->addRelease("teagram-build-999", "dev", "101",
		signingKey->privateKey.get(), archive);
	runFailure(std::move(wrongTag), trustedKey,
		TeagramUpdateDownloadReason::InvalidResponse);

	auto cancelled = std::make_unique<FixtureTransport>();
	cancelled->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	runFailure(std::move(cancelled), trustedKey,
		TeagramUpdateDownloadReason::Cancelled, true);

	auto production = std::make_unique<FixtureTransport>();
	production->addRelease("teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	production->addReleaseList();
	TeagramUpdateDownloader noProductionKey(std::move(production));
	const auto noKeyResult = noProductionKey.checkForUpdates(
		100, TeagramUpdateChannel::Dev);
	CHECK(noKeyResult.reason
		== TeagramUpdateDownloadReason::VerificationFailed);
	CHECK(noKeyResult.candidate == nullptr);
	CHECK(QDir(staging.path()).entryList(
		QDir::Files | QDir::NoDotAndDotDot).isEmpty());
	QFile canary(canaryPath);
	CHECK(canary.open(QIODevice::ReadOnly));
	CHECK_EQ(canary.readAll(), canaryContents);
	QFile appCanary(appCanaryPath);
	CHECK(appCanary.open(QIODevice::ReadOnly));
	CHECK_EQ(appCanary.readAll(), appCanaryContents);
}
