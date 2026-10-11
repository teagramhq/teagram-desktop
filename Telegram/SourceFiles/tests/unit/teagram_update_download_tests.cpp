/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_update_archive.h"
#include "core/teagram_update_download.h"
#include "core/teagram_update_replacement.h"
#if defined(Q_OS_MAC)
#include "core/teagram_update_install.h"
#endif // Q_OS_MAC

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrlQuery>
#if defined(Q_OS_MAC)
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#endif // Q_OS_MAC

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

struct BioDeleter {
	void operator()(BIO *value) const;
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;
using Bio = std::unique_ptr<BIO, BioDeleter>;

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
	const auto hash
		= QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex();
	const auto assetSize
		= size.isEmpty() ? QByteArray::number(archive.size()) : size;
	return QByteArrayLiteral(
			   "{\"format\":1,\"product\":\"io.teagram.desktop\","
			   "\"repo\":\"teagramhq/teagram-desktop\",\"arch\":\"arm64\","
			   "\"channel\":\"")
		   + channel + QByteArrayLiteral("\",\"build\":\"") + build
		   + QByteArrayLiteral("\",\"commit\":\"")
		   + QByteArrayLiteral("0123456789abcdef0123456789abcdef01234567")
		   + QByteArrayLiteral("\",\"asset_name\":\"Teagram-macOS-arm64-")
		   + build + QByteArrayLiteral(".zip\",\"asset_size\":\"") + assetSize
		   + QByteArrayLiteral("\",\"asset_sha256\":\"") + hash
		   + QByteArrayLiteral("\",\"min_os\":\"13.0\"}");
}

struct FakeResponse {
	int status = 200;
	QUrl redirect;
	QByteArray body;
	TeagramUpdateTransportFailure failure = TeagramUpdateTransportFailure::None;
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

	void addAsset(const QByteArray &tag, const QByteArray &name,
				  FakeResponse response);

	void addRelease(const QByteArray &tag, const QByteArray &channel,
					const QByteArray &build, EVP_PKEY *key,
					const QByteArray &archive, bool prerelease = false,
					const QByteArray &signedSize = {});

	void addReleaseWithArtifacts(const QByteArray &tag,
								 const QByteArray &build,
								 const QByteArray &manifest,
								 const QByteArray &signature,
								 const QByteArray &archive,
								 bool prerelease = false);

	void addOldReleases(int count);

	void addReleaseList();

	TeagramUpdateTransportResponse
	get(const QUrl &url, quint64 maximumBytes, const ChunkHandler &onChunk,
		const CancellationCheck &isCancelled) override;

	AfterChunk _afterChunk;
	std::vector<QUrl> _requests;

  private:
	[[nodiscard]] static QString Key(const QUrl &url);

	std::map<QString, FakeResponse> _responses;
	QJsonArray _releases;

};

void PkeyDeleter::operator()(EVP_PKEY *value) const { EVP_PKEY_free(value); }

void PkeyContextDeleter::operator()(EVP_PKEY_CTX *value) const {
	EVP_PKEY_CTX_free(value);
}

void DigestContextDeleter::operator()(EVP_MD_CTX *value) const {
	EVP_MD_CTX_free(value);
}

void BioDeleter::operator()(BIO *value) const { BIO_free(value); }

void FixtureTransport::addResponse(const QUrl &url, FakeResponse response) {
	_responses.insert_or_assign(Key(url), std::move(response));
}

void FixtureTransport::addAsset(const QByteArray &tag, const QByteArray &name,
								FakeResponse response) {
	auto url = QUrl(
		u"https://github.com/teagramhq/teagram-desktop/releases/download/"_q
		+ QString::fromUtf8(tag) + u"/"_q + QString::fromUtf8(name));
	addResponse(url, std::move(response));
}

void FixtureTransport::addRelease(const QByteArray &tag,
								  const QByteArray &channel,
								  const QByteArray &build, EVP_PKEY *key,
								  const QByteArray &archive, bool prerelease,
								  const QByteArray &signedSize) {
	const auto manifest = Manifest(channel, build, archive, signedSize);
	const auto signature = SignManifest(manifest, key);
	addReleaseWithArtifacts(tag, build, manifest, signature, archive,
							prerelease);
}

void FixtureTransport::addReleaseWithArtifacts(
	const QByteArray &tag, const QByteArray &build, const QByteArray &manifest,
	const QByteArray &signature, const QByteArray &archive, bool prerelease) {
	addAsset(tag, "teagram-update.json", ResponseWithBody(manifest));
	addAsset(tag, "teagram-update.sig", ResponseWithBody(signature));
	addAsset(tag, QByteArray("Teagram-macOS-arm64-") + build + ".zip",
			 ResponseWithBody(archive));
	auto assets = QJsonArray();
	assets.append(QJsonObject{{u"name"_q, u"teagram-update.json"_q}});
	assets.append(QJsonObject{{u"name"_q, u"teagram-update.sig"_q}});
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
	for (auto page = 0; qsizetype(page) * 100 < _releases.size(); ++page) {
		const auto start = qsizetype(page) * 100;
		const auto end = std::min(start + 100, _releases.size());
		auto releases = QJsonArray();
		for (auto i = start; i < end; ++i) {
			releases.append(_releases[i]);
		}
		const auto body
			= QJsonDocument(releases).toJson(QJsonDocument::Compact);
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
	const QUrl &url, quint64 maximumBytes,
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
		return {TeagramUpdateTransportFailure::None, response.status,
				response.redirect};
	}
	auto received = quint64(0);
	for (auto offset = qsizetype(0); offset < response.body.size();) {
		if (isCancelled()) {
			return {
				TeagramUpdateTransportFailure::Cancelled, response.status, {}};
		}
		const auto size
			= std::min<qsizetype>(16384, response.body.size() - offset);
		const auto chunk
			= QByteArrayView(response.body.constData() + offset, size);
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
	return {TeagramUpdateTransportFailure::None, response.status, {}};
}

QString FixtureTransport::Key(const QUrl &url) {
	auto result = url.host() + url.path();
	if (url.host() == u"api.github.com"_q) {
		result += u"?page="_q + QUrlQuery(url).queryItemValue(u"page"_q);
	}
	return result;
}

[[nodiscard]] bool HasOnlyFixedSourceHosts(const std::vector<QUrl> &requests) {
	for (const auto &url : requests) {
		const auto host = url.host();
		if (host == u"api.github.com"_q) {
			if (url.path() != u"/repos/teagramhq/teagram-desktop/releases"_q) {
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

struct ZipFixtureEntry {
	QString path;
	QByteArray data;
	bool symlink = false;
	bool special = false;
	quint32 mode = 0644;
};

[[nodiscard]] bool RunProcess(const QString &program,
							  const QStringList &arguments);
[[nodiscard]] QByteArray ReadFile(const QString &path);

[[nodiscard]] QByteArray CreateZip(const QString &directory,
								   const std::vector<ZipFixtureEntry> &entries) {
	const auto path = QDir(directory).filePath(u"fixture.zip"_q);
	auto encoded = QJsonArray();
	for (const auto &entry : entries) {
		encoded.append(QJsonObject{
			{u"path"_q, entry.path},
			{u"data"_q, QString::fromLatin1(entry.data.toBase64())},
			{u"symlink"_q, entry.symlink},
			{u"special"_q, entry.special},
			{u"mode"_q, int(entry.mode)},
		});
	}
	const auto entriesPath = QDir(directory).filePath(u"entries.json"_q);
	if (!WriteFile(entriesPath,
				QJsonDocument(encoded).toJson(QJsonDocument::Compact))) {
		CHECK(false);
		return {};
	}
	const auto source = QString::fromLatin1(
		"import base64, json, stat, sys, zipfile\n"
		"with open(sys.argv[2], 'r', encoding='utf-8') as data:\n"
		"    entries = json.load(data)\n"
		"with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as archive:\n"
		"    for entry in entries:\n"
		"        payload = base64.b64decode(entry['data'])\n"
		"        if entry['symlink']:\n"
		"            info = zipfile.ZipInfo(entry['path'])\n"
		"            info.create_system = 3\n"
		"            info.external_attr = (stat.S_IFLNK | 0o777) << 16\n"
		"            archive.writestr(info, payload)\n"
		"        elif entry['special']:\n"
		"            info = zipfile.ZipInfo(entry['path'])\n"
		"            info.create_system = 3\n"
		"            info.external_attr = (stat.S_IFIFO | 0o600) << 16\n"
		"            archive.writestr(info, payload)\n"
		"        else:\n"
		"            info = zipfile.ZipInfo(entry['path'])\n"
		"            info.create_system = 3\n"
		"            info.external_attr = (stat.S_IFREG | entry['mode']) << 16\n"
		"            archive.writestr(info, payload)\n");
	const auto succeeded = RunProcess(
		QString::fromUtf8(TDESKTOP_TEST_PYTHON_EXECUTABLE),
		{u"-c"_q, source, path, entriesPath});
	if (!succeeded) {
		return {};
	}
	return ReadFile(path);
}

[[nodiscard]] bool RunProcess(const QString &program,
							  const QStringList &arguments) {
	auto process = QProcess();
	process.start(program, arguments);
	const auto started = process.waitForStarted();
	CHECK(started);
	if (!started) {
		return false;
	}
	const auto finished = process.waitForFinished();
	CHECK(finished);
	if (!finished) {
		return false;
	}
	CHECK(process.exitStatus() == QProcess::NormalExit);
	CHECK_EQ(process.exitCode(), 0);
	return process.exitStatus() == QProcess::NormalExit
		   && process.exitCode() == 0;
}

[[nodiscard]] QByteArray ReadFile(const QString &path) {
	auto file = QFile(path);
	const auto opened = file.open(QIODevice::ReadOnly);
	CHECK(opened);
	return opened ? file.readAll() : QByteArray();
}

[[nodiscard]] bool WritePrivateKey(EVP_PKEY *key, const QString &path) {
	const auto localPath = path.toLocal8Bit();
	auto bio = Bio(BIO_new_file(localPath.constData(), "wb"));
	const auto written
		= bio
		  && (PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
										   nullptr, nullptr)
			  == 1);
	CHECK(written);
	return written;
}

struct ProducedPackage {
	QByteArray manifest;
	QByteArray signature;
};

[[nodiscard]] std::optional<ProducedPackage>
ProducePackage(const QByteArray &archive, const QString &keyPath,
				const QString &directory, const QByteArray &channel,
				const QByteArray &build) {
	if (!QDir().mkpath(directory)) {
		CHECK(false);
		return std::nullopt;
	}
	const auto assetName
		= u"Teagram-macOS-arm64-%1.zip"_q.arg(QString::fromLatin1(build));
	const auto archivePath = QDir(directory).filePath(assetName);
	if (!WriteFile(archivePath, archive)) {
		CHECK(false);
		return std::nullopt;
	}
	const auto output = QDir(directory).filePath(u"output"_q);
	const auto succeeded = RunProcess(
		QString::fromUtf8(TDESKTOP_TEST_PYTHON_EXECUTABLE),
		{
			QString::fromUtf8(TDESKTOP_TEST_TEAGRAM_UPDATE_PRODUCER),
			u"--archive"_q,
			archivePath,
			u"--build"_q,
			QString::fromLatin1(build),
			u"--channel"_q,
			QString::fromLatin1(channel),
			u"--commit"_q,
			u"0123456789abcdef0123456789abcdef01234567"_q,
			u"--min-os"_q,
			u"13.0"_q,
			u"--signing-key"_q,
			keyPath,
			u"--output-dir"_q,
			output,
		});
	if (!succeeded) {
		return std::nullopt;
	}
	auto manifest = ReadFile(QDir(output).filePath(u"teagram-update.json"_q));
	auto signature = ReadFile(QDir(output).filePath(u"teagram-update.sig"_q));
	CHECK(!manifest.isEmpty());
	CHECK(!signature.isEmpty());
	if (manifest.isEmpty() || signature.isEmpty()) {
		return std::nullopt;
	}
	return ProducedPackage{std::move(manifest), std::move(signature)};
}

#if defined(Q_OS_MAC)
constexpr auto kProfileInstanceRole
	= "TDESKTOP_TEAGRAM_UPDATE_TEST_INSTANCE";
constexpr auto kProfileInstanceSocket
	= "TDESKTOP_TEAGRAM_UPDATE_TEST_SOCKET";
constexpr auto kProfileInstanceBundle
	= "TDESKTOP_TEAGRAM_UPDATE_TEST_BUNDLE";

[[nodiscard]] int RunProfileInstanceHelper() {
	const auto role = qEnvironmentVariable(kProfileInstanceRole);
	const auto socketName = qEnvironmentVariable(kProfileInstanceSocket);
	const auto bundle = qEnvironmentVariable(kProfileInstanceBundle);
	if (role == "owner") {
		auto server = QLocalServer();
		if (!server.listen(socketName)) {
			return 1;
		}
		QObject::connect(&server, &QLocalServer::newConnection, [&] {
			while (const auto client = server.nextPendingConnection()) {
				QObject::connect(client, &QLocalSocket::readyRead, client, [=] {
					const auto command = client->readAll();
					client->write(command.contains("CMD:quit;")
						? QByteArrayLiteral("RES:0_0;")
						: QByteArrayLiteral("RES:1_0;"));
					client->flush();
					if (command.contains("CMD:quit;")) {
						QCoreApplication::quit();
					}
				});
			}
		});
		std::fputs("READY\n", stdout);
		std::fflush(stdout);
		return QCoreApplication::exec();
	} else if (role == "candidate" || role == "fallback") {
		auto client = QLocalSocket();
		client.connectToServer(socketName);
		if (client.waitForConnected(1'000)) {
			client.write("CMD:show;");
			client.waitForBytesWritten(1'000);
			return 20;
		} else if (client.error() != QLocalSocket::ServerNotFoundError) {
			return 21;
		}
		QLocalServer::removeServer(socketName);
		auto server = QLocalServer();
		if (!server.listen(socketName)) {
			return 22;
		}
		const auto acknowledged
			= CompleteTeagramUpdateStartupForTesting(
				bundle, [] { return false; });
		return role == "fallback" || acknowledged ? 0 : 23;
	}
	return 2;
}

[[nodiscard]] bool StartProfileInstanceHelper(
		QProcess &process,
		const QString &role,
		const QString &socketName,
		const QString &bundle = {}) {
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(kProfileInstanceRole, role);
	environment.insert(kProfileInstanceSocket, socketName);
	environment.insert(kProfileInstanceBundle, bundle);
	process.setProcessEnvironment(environment);
	process.start(
		QCoreApplication::applicationFilePath(),
		{
			"--case",
			"TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback",
		});
	if (!process.waitForStarted(5'000)) {
		return false;
	}
	if (role != "owner") {
		return true;
	}
	return (process.canReadLine() || process.waitForReadyRead(5'000))
		&& process.readAllStandardOutput().contains("READY\n");
}

[[nodiscard]] bool RunProfileCandidate(
		const QString &role,
		const QString &socketName,
		const QString &bundle,
		int expectedExitCode) {
	auto process = QProcess();
	if (!StartProfileInstanceHelper(
			process, role, socketName, bundle)
		|| !process.waitForFinished(5'000)) {
		if (process.state() != QProcess::NotRunning) {
			process.kill();
			process.waitForFinished();
		}
		return false;
	}
	return process.exitStatus() == QProcess::NormalExit
		&& process.exitCode() == expectedExitCode;
}

void StopProfileInstanceHelper(
		QProcess &process,
		const QString &socketName) {
	if (process.state() == QProcess::NotRunning) {
		return;
	}
	auto client = QLocalSocket();
	client.connectToServer(socketName);
	if (client.waitForConnected(1'000)) {
		client.write("CMD:quit;");
		client.waitForBytesWritten(1'000);
	}
	if (!process.waitForFinished(5'000)) {
		process.kill();
		process.waitForFinished();
	}
}
#endif // Q_OS_MAC

} // namespace

TEST_CASE(TeagramUpdateArchiveWritesStayConfined) {
	const auto signingKey = GenerateTestSigningKey();
	if (!signingKey) {
		return;
	}
	const auto trustedKey
		= std::optional<TeagramUpdatePublicKey>(signingKey->publicKey);
	const auto makeCandidate = [&](const QByteArray &archive,
								   const QString &stagingDirectory) {
		auto transport = std::make_unique<FixtureTransport>();
		transport->addRelease("teagram-build-101", "dev", "101",
			signingKey->privateKey.get(), archive);
		transport->addReleaseList();
		auto downloader = TeagramUpdateDownloader(
			std::move(transport), trustedKey, stagingDirectory);
		auto result = downloader.checkForUpdates(100, TeagramUpdateChannel::Dev);
		CHECK(result.available());
		return std::move(result.candidate);
	};
	auto generated = QTemporaryDir();
	CHECK(generated.isValid());
	if (!generated.isValid()) {
		return;
	}
	const auto safeArchive = CreateZip(generated.path(), {
		{u"Teagram.app/Contents/Info.plist"_q, "fixture plist"},
		{u"Teagram.app/Contents/Frameworks/Test.framework/Versions/A/"_q
			+ u"Resources/Info.plist"_q,
		 "fixture resource"},
		{u"Teagram.app/Contents/Frameworks/Test.framework/Versions/Current"_q,
		 "A", true},
		{u"Teagram.app/Contents/Frameworks/Test.framework/Resources"_q,
		 "Versions/Current/Resources", true},
	});
	CHECK(!safeArchive.isEmpty());
	auto downloadStaging = QTemporaryDir();
	auto extracted = QTemporaryDir();
	CHECK(downloadStaging.isValid());
	CHECK(extracted.isValid());
	if (!downloadStaging.isValid() || !extracted.isValid()) {
		return;
	}
	auto candidate = makeCandidate(safeArchive, downloadStaging.path());
	if (!candidate) {
		return;
	}
	CHECK_EQ(candidate->installedBuild(), quint64(100));
	const auto safe = ExtractAuthenticatedTeagramUpdateArchive(
		*candidate, extracted.path());
	CHECK(safe.extracted());
	if (safe.extracted()) {
		const auto framework = QDir(safe.bundlePath).filePath(
			u"Contents/Frameworks/Test.framework"_q);
		CHECK(QFileInfo(QDir(framework).filePath(u"Versions/Current"_q))
			.isSymLink());
		CHECK(QFileInfo(QDir(framework).filePath(
			u"Resources/Info.plist"_q)).isFile());
	}
	auto tamperedDownloadStaging = QTemporaryDir();
	auto tamperedExtraction = QTemporaryDir();
	CHECK(tamperedDownloadStaging.isValid());
	CHECK(tamperedExtraction.isValid());
	if (!tamperedDownloadStaging.isValid() || !tamperedExtraction.isValid()) {
		return;
	}
	auto tamperedCandidate
		= makeCandidate(safeArchive, tamperedDownloadStaging.path());
	if (!tamperedCandidate) {
		return;
	}
	auto tamperedArchive = safeArchive;
	tamperedArchive[tamperedArchive.size() / 2] ^= 0x20;
	CHECK(WriteFile(tamperedCandidate->archivePath(), tamperedArchive));
	const auto tampered = ExtractAuthenticatedTeagramUpdateArchive(
		*tamperedCandidate, tamperedExtraction.path());
	CHECK(!tampered.extracted());
	CHECK(QDir(tamperedExtraction.path()).entryList(
		  QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());

	auto attackRoot = QTemporaryDir();
	CHECK(attackRoot.isValid());
	if (!attackRoot.isValid()) {
		return;
	}
	const auto outside = QDir(attackRoot.path()).filePath(u"outside"_q);
	CHECK(QDir().mkpath(outside));
	const auto canary = QDir(outside).filePath(u"marker"_q);
	const auto canaryBytes = QByteArrayLiteral("outside canary");
	CHECK(WriteFile(canary, canaryBytes));
	auto attackStaging = QTemporaryDir(
		QDir(attackRoot.path()).filePath(u"staging-XXXXXX"_q));
	CHECK(attackStaging.isValid());
	if (!attackStaging.isValid()) {
		return;
	}
	const auto attackArchive = CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/Resources"_q, "../../../outside", true},
	});
	CHECK(!attackArchive.isEmpty());
	auto attackDownloadStaging = QTemporaryDir();
	CHECK(attackDownloadStaging.isValid());
	if (!attackDownloadStaging.isValid()) {
		return;
	}
	auto attackCandidate
		= makeCandidate(attackArchive, attackDownloadStaging.path());
	if (!attackCandidate) {
		return;
	}
	const auto rejected = ExtractAuthenticatedTeagramUpdateArchive(
		*attackCandidate, attackStaging.path());
	CHECK(!rejected.extracted());
	CHECK_EQ(ReadFile(canary), canaryBytes);
	CHECK(!QFileInfo(QDir(attackStaging.path()).filePath(u"Teagram.app"_q))
		.exists());
	CHECK_EQ(ReadFile(canary), canaryBytes);

	const auto orderedAttackArchive = CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/Resources"_q, "../../../outside", true},
		{u"Teagram.app/Contents/Resources/marker"_q,
		 "attacker overwrite"},
	});
	CHECK(!orderedAttackArchive.isEmpty());
	auto orderedCandidate = makeCandidate(
		orderedAttackArchive, attackDownloadStaging.path());
	if (!orderedCandidate) {
		return;
	}
	const auto orderedRejected = ExtractAuthenticatedTeagramUpdateArchive(
		*orderedCandidate, attackStaging.path());
	CHECK(!orderedRejected.extracted());
	CHECK_EQ(ReadFile(canary), canaryBytes);
	CHECK(QDir(attackStaging.path()).entryList(
		  QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());

	const auto chainedSymlinkArchive = CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/Frameworks/A"_q,
		 "../../Contents", true},
		{u"Teagram.app/Contents/Frameworks/B"_q, "A/../../", true},
	});
	CHECK(!chainedSymlinkArchive.isEmpty());
	auto chainedCandidate = makeCandidate(
		chainedSymlinkArchive, attackDownloadStaging.path());
	if (!chainedCandidate) {
		return;
	}
	const auto chainedRejected = ExtractAuthenticatedTeagramUpdateArchive(
		*chainedCandidate, attackStaging.path());
	CHECK(!chainedRejected.extracted());
	CHECK_EQ(ReadFile(canary), canaryBytes);
	CHECK(QDir(attackStaging.path()).entryList(
		  QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
	const auto checkRejected = [&](const QByteArray &archive,
								   TeagramUpdateArchiveReason expected) {
		auto download = QTemporaryDir();
		auto extraction = QTemporaryDir();
		CHECK(download.isValid());
		CHECK(extraction.isValid());
		if (!download.isValid() || !extraction.isValid()) {
			return;
		}
		auto fixture = makeCandidate(archive, download.path());
		if (!fixture) {
			return;
		}
		const auto result = ExtractAuthenticatedTeagramUpdateArchive(
			*fixture, extraction.path());
		CHECK(result.reason == expected);
		CHECK(QDir(extraction.path()).entryList(
			  QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
		CHECK_EQ(ReadFile(canary), canaryBytes);
	};
	checkRejected(CreateZip(attackRoot.path(), {
		{u"Teagram.app/../../outside"_q, "traversal"},
	}), TeagramUpdateArchiveReason::ArchiveRejected);
	checkRejected(CreateZip(attackRoot.path(), {
		{u"/tmp/Teagram.app/Contents/file"_q, "absolute"},
	}), TeagramUpdateArchiveReason::ArchiveRejected);
	checkRejected(CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/duplicate"_q, "first"},
		{u"Teagram.app/Contents/duplicate"_q, "second"},
	}), TeagramUpdateArchiveReason::ArchiveRejected);
	checkRejected(CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents"_q, "file"},
		{u"Teagram.app/Contents/file"_q, "child"},
	}), TeagramUpdateArchiveReason::ArchiveRejected);
	checkRejected(CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/FIFO"_q, "special", false, true},
	}), TeagramUpdateArchiveReason::ArchiveRejected);
	auto oversizedArchive = CreateZip(attackRoot.path(), {
		{u"Teagram.app/Contents/file"_q, "oversized"},
	});
	const auto centralHeader
		= oversizedArchive.indexOf(QByteArray::fromHex("504b0102"));
	CHECK(centralHeader >= 0);
	if (centralHeader >= 0) {
		oversizedArchive[centralHeader + 24] = char(0x01);
		oversizedArchive[centralHeader + 25] = char(0x00);
		oversizedArchive[centralHeader + 26] = char(0x00);
		oversizedArchive[centralHeader + 27] = char(0x80);
		checkRejected(
			oversizedArchive, TeagramUpdateArchiveReason::LimitExceeded);
	}
	auto manyEntries = std::vector<ZipFixtureEntry>();
	manyEntries.reserve(50'001);
	for (auto index = 0; index != 50'001; ++index) {
		manyEntries.push_back({u"Teagram.app/%1"_q.arg(index), {}});
	}
	checkRejected(
		CreateZip(attackRoot.path(), manyEntries),
		TeagramUpdateArchiveReason::LimitExceeded);
}

TEST_CASE(TeagramUpdateInstallFailurePreservesApp) {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	if (!root.isValid()) {
		return;
	}
	const auto installed = QDir(root.path()).filePath(u"Teagram.app"_q);
	const auto candidate = QDir(root.path()).filePath(u"staging/Teagram.app"_q);
	const auto previous
		= QDir(root.path()).filePath(u".Teagram.previous.app"_q);
	const auto installedExecutable
		= QDir(installed).filePath(u"Contents/MacOS/Teagram"_q);
	const auto candidateExecutable
		= QDir(candidate).filePath(u"Contents/MacOS/Teagram"_q);
	const auto profile = QDir(root.path()).filePath(u"profile"_q);
	const auto profileCanary = QDir(profile).filePath(u"account"_q);
	const auto official = QDir(root.path()).filePath(u"Telegram.app"_q);
	const auto officialCanary
		= QDir(official).filePath(u"Contents/MacOS/Telegram"_q);
	CHECK(QDir().mkpath(QFileInfo(installedExecutable).absolutePath()));
	CHECK(QDir().mkpath(QFileInfo(candidateExecutable).absolutePath()));
	CHECK(QDir().mkpath(profile));
	CHECK(QDir().mkpath(QFileInfo(officialCanary).absolutePath()));
	const auto installedBytes = QByteArrayLiteral("fixture build 100");
	const auto candidateBytes = QByteArrayLiteral("fixture build 101");
	const auto profileBytes = QByteArrayLiteral("profile stays unchanged");
	const auto officialBytes = QByteArrayLiteral("official canary");
	CHECK(WriteFile(installedExecutable, installedBytes));
	CHECK(WriteFile(candidateExecutable, candidateBytes));
	CHECK(WriteFile(profileCanary, profileBytes));
	CHECK(WriteFile(officialCanary, officialBytes));
	const auto privatePermissions
		= QFileDevice::ReadOwner | QFileDevice::WriteOwner
		  | QFileDevice::ExeOwner;
	CHECK(QFile::setPermissions(
		QDir(root.path()).filePath(u"staging"_q), privatePermissions));
	CHECK(QFile::setPermissions(
		installedExecutable,
		privatePermissions));
	CHECK(QFile::setPermissions(candidateExecutable, privatePermissions));
	auto lock = TeagramUpdateInstallLock(installed);
	CHECK(lock.acquired());
	const auto competingLock = TeagramUpdateInstallLock(installed);
	CHECK(!competingLock.acquired());
	if (!lock.acquired()) {
		return;
	}
	const auto result = CommitTeagramUpdateReplacement(
		lock,
		installed,
		candidate,
		previous,
		{},
		[](TeagramUpdateReplacementFailurePoint point) {
			return point
				== TeagramUpdateReplacementFailurePoint::BeforePreviousMove;
		});
	CHECK(result.reason == TeagramUpdateReplacementReason::InjectedFailure);
	CHECK_EQ(ReadFile(installedExecutable), installedBytes);
	CHECK_EQ(ReadFile(profileCanary), profileBytes);
	CHECK_EQ(ReadFile(officialCanary), officialBytes);
	CHECK(QFileInfo(candidateExecutable).isExecutable());
	CHECK(!QFileInfo(previous).exists());
	const auto rollback = CommitTeagramUpdateReplacement(
		lock,
		installed,
		candidate,
		previous,
		{},
		[](TeagramUpdateReplacementFailurePoint point) {
			return point
				== TeagramUpdateReplacementFailurePoint::AfterPreviousMove;
		});
	CHECK(rollback.reason == TeagramUpdateReplacementReason::InjectedFailure);
	CHECK_EQ(ReadFile(installedExecutable), installedBytes);
	CHECK_EQ(ReadFile(profileCanary), profileBytes);
	CHECK_EQ(ReadFile(officialCanary), officialBytes);
	CHECK(!QFileInfo(previous).exists());
}

TEST_CASE(TeagramUpdateMarkerRejectsHardLinkWithoutTruncatingCanary) {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	if (!root.isValid()) {
		return;
	}
	const auto canary = root.filePath(u"profile-canary"_q);
	const auto marker = root.filePath(u".teagram-update-pending"_q);
	CHECK(WriteFile(marker, QByteArrayLiteral("100\n")));
	CHECK(QFile::setPermissions(
		marker, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
	struct stat oldMarkerInfo = {};
	const auto markerBytes = QFile::encodeName(marker);
	CHECK_EQ(::lstat(markerBytes.constData(), &oldMarkerInfo), 0);
	CHECK(WriteTeagramUpdateMarker(
		root.path(), ".teagram-update-pending", 101));
	CHECK_EQ(ReadFile(marker), QByteArrayLiteral("101\n"));
	struct stat newMarkerInfo = {};
	CHECK_EQ(::lstat(markerBytes.constData(), &newMarkerInfo), 0);
	CHECK(oldMarkerInfo.st_ino != newMarkerInfo.st_ino);
	CHECK(QFile::remove(marker));
	const auto original = QByteArrayLiteral("profile remains intact");
	CHECK(WriteFile(canary, original));
	CHECK(QFile::setPermissions(
		canary, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
	const auto canaryBytes = QFile::encodeName(canary);
	CHECK_EQ(::link(canaryBytes.constData(), markerBytes.constData()), 0);
	CHECK(!WriteTeagramUpdateMarker(
		root.path(), ".teagram-update-pending", 101));
	CHECK_EQ(ReadFile(canary), original);
	CHECK_EQ(ReadFile(marker), original);
	struct stat canaryInfo = {};
	struct stat markerInfo = {};
	CHECK_EQ(::lstat(canaryBytes.constData(), &canaryInfo), 0);
	CHECK_EQ(::lstat(markerBytes.constData(), &markerInfo), 0);
	CHECK(canaryInfo.st_ino == markerInfo.st_ino);
	CHECK(canaryInfo.st_nlink == 2);
}

#if defined(Q_OS_MAC)
TEST_CASE(TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback) {
	if (qEnvironmentVariableIsSet(kProfileInstanceRole)) {
		std::exit(RunProfileInstanceHelper());
	}
	const auto signingKey = GenerateTestSigningKey();
	if (!signingKey) {
		return;
	}
	auto generated = QTemporaryDir();
	auto downloadStaging = QTemporaryDir();
	CHECK(generated.isValid());
	CHECK(downloadStaging.isValid());
	if (!generated.isValid() || !downloadStaging.isValid()) {
		return;
	}
	const auto bundleInfo = [](quint64 build) {
		return QByteArrayLiteral(
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<plist version=\"1.0\"><dict>"
			"<key>CFBundleIdentifier</key><string>io.teagram.desktop</string>"
			"<key>CFBundleExecutable</key><string>Teagram</string>"
			"<key>TeagramUpdateChannel</key><string>dev</string>"
			"<key>TeagramUpdateBuild</key><string>")
			+ QByteArray::number(build)
			+ QByteArrayLiteral("</string></dict></plist>");
	};
	const auto archive = CreateZip(generated.path(), {
		{u"Teagram.app/Contents/Info.plist"_q, bundleInfo(101)},
		{u"Teagram.app/Contents/MacOS/Teagram"_q,
		 "fixture build 101", false, false, 0755},
	});
	CHECK(!archive.isEmpty());
	auto transport = std::make_unique<FixtureTransport>();
	transport->addRelease(
		"teagram-build-101", "dev", "101",
		signingKey->privateKey.get(), archive);
	transport->addReleaseList();
	auto downloader = TeagramUpdateDownloader(
		std::move(transport),
		std::optional<TeagramUpdatePublicKey>(signingKey->publicKey),
		downloadStaging.path());
	auto downloaded = downloader.checkForUpdates(100, TeagramUpdateChannel::Dev);
	CHECK(downloaded.available());
	if (!downloaded.available()) {
		return;
	}
	CHECK_EQ(downloaded.candidate->installedBuild(), quint64(100));
	CHECK_EQ(downloaded.candidate->metadata().build(), quint64(101));
	const auto installedBytes = QByteArrayLiteral("fixture build 100");
	const auto candidateBytes = QByteArrayLiteral("fixture build 101");
	const auto profileBytes = QByteArrayLiteral("profile stays unchanged");
	const auto officialBytes = QByteArrayLiteral("official canary");
	const auto executablePermissions
		= QFileDevice::ReadOwner | QFileDevice::WriteOwner
		  | QFileDevice::ExeOwner;
	const auto runFixture = [&](bool sealValid,
								bool restartSucceeds,
								bool candidateLaunchSucceeds,
								TeagramUpdateInstallReason expected,
								TeagramUpdateRelaunchResult expectedRelaunch) {
		auto architectureChecks = 0;
		auto codeSealChecks = 0;
		auto restartAttempts = 0;
		auto root = QTemporaryDir();
		CHECK(root.isValid());
		if (!root.isValid()) {
			return;
		}
		const auto installed = root.filePath(u"Teagram.app"_q);
		const auto installedExecutable = QDir(installed).filePath(
			u"Contents/MacOS/Teagram"_q);
		const auto previous = root.filePath(u".Teagram.previous.app"_q);
		const auto previousExecutable = QDir(previous).filePath(
			u"Contents/MacOS/Teagram"_q);
		const auto profileSocket = root.filePath(u"profile-instance.sock"_q);
		const auto profileCanary = root.filePath(u"profile/account"_q);
		const auto officialCanary
			= root.filePath(u"Telegram.app/Contents/MacOS/Telegram"_q);
		CHECK(QDir().mkpath(QFileInfo(installedExecutable).absolutePath()));
		CHECK(QDir().mkpath(QFileInfo(profileCanary).absolutePath()));
		CHECK(QDir().mkpath(QFileInfo(officialCanary).absolutePath()));
		CHECK(WriteFile(
			QDir(installed).filePath(u"Contents/Info.plist"_q),
			bundleInfo(100)));
		CHECK(WriteFile(installedExecutable, installedBytes));
		CHECK(WriteFile(profileCanary, profileBytes));
		CHECK(WriteFile(officialCanary, officialBytes));
		CHECK(QFile::setPermissions(installedExecutable, executablePermissions));
		auto hooks = TeagramUpdateInstallerTestHooks();
		hooks.runningBundleIdentifier = u"io.teagram.desktop"_q;
		hooks.runningBundlePath = installed;
		hooks.hasArchitecture = [&](const QString &path) {
			++architectureChecks;
			return QFileInfo(path).isExecutable();
		};
		hooks.hasCleanCodeSeal = [&](const QString &) {
			++codeSealChecks;
			return sealValid;
		};
		hooks.otherInstanceRunning = [] { return false; };
		auto profileOwner = QProcess();
		if (expected != TeagramUpdateInstallReason::CandidateRejected) {
			CHECK(StartProfileInstanceHelper(
				profileOwner, "owner", profileSocket));
			CHECK(RunProfileCandidate(
				"candidate", profileSocket, installed, 20));
			CHECK(!QFileInfo(root.filePath(
				u".teagram-update-started"_q)).exists());
		}
		hooks.requestRestart = [&] {
			++restartAttempts;
			return restartSucceeds;
		};
		auto installer = TeagramUpdateInstaller(std::move(hooks));
		const auto result = installer.install(*downloaded.candidate);
		CHECK(result.reason == expected);
		CHECK_EQ(
			architectureChecks,
			expected == TeagramUpdateInstallReason::CandidateRejected ? 2 : 3);
		CHECK_EQ(codeSealChecks, 1);
		CHECK_EQ(
			restartAttempts,
			expected == TeagramUpdateInstallReason::CandidateRejected ? 0 : 1);
		CHECK_EQ(ReadFile(profileCanary), profileBytes);
		CHECK_EQ(ReadFile(officialCanary), officialBytes);
		if (expected == TeagramUpdateInstallReason::CandidateRejected) {
			CHECK_EQ(ReadFile(installedExecutable), installedBytes);
			CHECK(!QFileInfo(previous).exists());
			return;
		}
		CHECK_EQ(ReadFile(installedExecutable), candidateBytes);
		CHECK_EQ(ReadFile(previousExecutable), installedBytes);
		CHECK(QFileInfo(previousExecutable).isExecutable());
		CHECK_EQ(result.previousBundlePath, previous);
		if (expected == TeagramUpdateInstallReason::RelaunchRequested) {
			CHECK(QFileInfo(root.filePath(u".teagram-update-pending"_q)).exists());
			CHECK(!QFileInfo(root.filePath(u".teagram-update-started"_q)).exists());
			StopProfileInstanceHelper(profileOwner, profileSocket);
			auto launchAttempts = 0;
			const auto relaunched
				= RelaunchTeagramUpdateAfterShutdownForTesting(
					installed,
					previous,
					[&](const QString &path) {
						++launchAttempts;
						if (path == installed) {
							return candidateLaunchSucceeds
								&& RunProfileCandidate(
									"candidate", profileSocket, installed, 0);
						} else if (path == previous) {
							return RunProfileCandidate(
								"fallback", profileSocket, previous, 0);
						}
						return false;
					},
					[] { return false; });
			CHECK(relaunched == expectedRelaunch);
			CHECK_EQ(launchAttempts, candidateLaunchSucceeds ? 1 : 2);
			if (expectedRelaunch
				== TeagramUpdateRelaunchResult::CandidateStarted) {
				CHECK(!QFileInfo(root.filePath(
					u".teagram-update-pending"_q)).exists());
				CHECK(!QFileInfo(root.filePath(
					u".teagram-update-started"_q)).exists());
				CHECK(!QFileInfo(previous).exists());
			} else {
				CHECK(QFileInfo(root.filePath(
					u".teagram-update-pending"_q)).exists());
				CHECK(!QFileInfo(root.filePath(
					u".teagram-update-started"_q)).exists());
				CHECK(QFileInfo(previous).exists());
			}
		} else {
			CHECK(QFileInfo(root.filePath(u".teagram-update-pending"_q)).exists());
			CHECK(!QFileInfo(root.filePath(u".teagram-update-started"_q)).exists());
			CHECK(QFileInfo(previous).exists());
			StopProfileInstanceHelper(profileOwner, profileSocket);
		}
	};
	runFixture(
		true,
		true,
		true,
		TeagramUpdateInstallReason::RelaunchRequested,
		TeagramUpdateRelaunchResult::CandidateStarted);
	runFixture(
		true,
		true,
		false,
		TeagramUpdateInstallReason::RelaunchRequested,
		TeagramUpdateRelaunchResult::PreviousStarted);
	runFixture(
		false,
		true,
		false,
		TeagramUpdateInstallReason::CandidateRejected,
		TeagramUpdateRelaunchResult::NotPending);
	runFixture(
		true,
		false,
		false,
		TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
		TeagramUpdateRelaunchResult::Failed);
}

TEST_CASE(TeagramUpdateInstallerRemovesInterruptedExtraction) {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	if (!root.isValid()) {
		return;
	}
	const auto installed = root.filePath(u"Teagram.app"_q);
	const auto staging = root.filePath(u".teagram-update-staging"_q);
	const auto previous = root.filePath(u".Teagram.previous.app"_q);
	const auto running = installed;
	const auto installedInfo = QDir(installed).filePath(
		u"Contents/Info.plist"_q);
	const auto installedExecutable = QDir(installed).filePath(
		u"Contents/MacOS/Teagram"_q);
	const auto partialInfo = QDir(staging).filePath(
		u"Teagram.app/Contents/Info.plist"_q);
	CHECK(QDir().mkpath(QFileInfo(installedInfo).absolutePath()));
	CHECK(QDir().mkpath(QFileInfo(installedExecutable).absolutePath()));
	CHECK(QDir().mkpath(QFileInfo(partialInfo).absolutePath()));
	CHECK(WriteFile(
		installedInfo,
		QByteArrayLiteral(
			"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
			"<key>CFBundleIdentifier</key><string>io.teagram.desktop</string>"
			"<key>CFBundleExecutable</key><string>Teagram</string>"
			"<key>TeagramUpdateBuild</key><string>100</string>"
			"</dict></plist>")));
	CHECK(WriteFile(installedExecutable, QByteArrayLiteral("build 100")));
	CHECK(WriteFile(partialInfo, QByteArrayLiteral("<plist><dict>")));
	const auto privatePermissions
		= QFileDevice::ReadOwner | QFileDevice::WriteOwner
		  | QFileDevice::ExeOwner;
	CHECK(QFile::setPermissions(staging, privatePermissions));
	CHECK(!QFileInfo(root.filePath(u".teagram-update-pending"_q)).exists());
	const auto result = RecoverTeagramUpdateInstallerAtStartupForTesting(
		installed, staging, previous, running, [] { return false; });
	CHECK(result.reason
		  == TeagramUpdateStartupRecoveryReason::IncompleteCandidateRemoved);
	CHECK_EQ(ReadFile(installedExecutable), QByteArrayLiteral("build 100"));
	CHECK(!QFileInfo(staging).exists());
	CHECK(!QFileInfo(previous).exists());
	CHECK(!QFileInfo(root.filePath(u".teagram-update-pending"_q)).exists());
}
#endif // Q_OS_MAC

TEST_CASE(TeagramUpdateReplacementRetainsPreviousThroughStartup) {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	if (!root.isValid()) {
		return;
	}
	const auto installed = QDir(root.path()).filePath(u"Teagram.app"_q);
	const auto staging
		= QDir(root.path()).filePath(u".teagram-update-staging"_q);
	const auto stagedPrevious = QDir(staging).filePath(u"Teagram.app"_q);
	const auto previous
		= QDir(root.path()).filePath(u".Teagram.previous.app"_q);
	const auto installedExecutable
		= QDir(installed).filePath(u"Contents/MacOS/Teagram"_q);
	const auto previousExecutable
		= QDir(stagedPrevious).filePath(u"Contents/MacOS/Teagram"_q);
	CHECK(QDir().mkpath(QFileInfo(installedExecutable).absolutePath()));
	CHECK(QDir().mkpath(QFileInfo(previousExecutable).absolutePath()));
	CHECK(QFile::setPermissions(
		staging,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner
			| QFileDevice::ExeOwner));
	CHECK(WriteFile(installedExecutable, QByteArrayLiteral("build 101")));
	CHECK(WriteFile(previousExecutable, QByteArrayLiteral("build 100")));
	const auto executablePermissions
		= QFileDevice::ReadOwner | QFileDevice::WriteOwner
		  | QFileDevice::ExeOwner;
	CHECK(QFile::setPermissions(installedExecutable, executablePermissions));
	CHECK(QFile::setPermissions(previousExecutable, executablePermissions));
	auto lock = TeagramUpdateInstallLock(installed);
	CHECK(lock.acquired());
	if (!lock.acquired()) {
		return;
	}
	const auto recovered = RecoverTeagramUpdateReplacement(
		lock, installed, staging, previous, installed, true, true, true);
	CHECK(recovered.reason
		  == TeagramUpdateStartupRecoveryReason::PreviousRetained);
	CHECK_EQ(ReadFile(installedExecutable), QByteArrayLiteral("build 101"));
	CHECK_EQ(ReadFile(QDir(previous).filePath(u"Contents/MacOS/Teagram"_q)),
			 QByteArrayLiteral("build 100"));
	CHECK(QFileInfo(QDir(previous).filePath(u"Contents/MacOS/Teagram"_q))
		.isExecutable());
	const auto finalized = RecoverTeagramUpdateReplacement(
		lock, installed, staging, previous, installed, false, true, true);
	CHECK(finalized.reason
		  == TeagramUpdateStartupRecoveryReason::PreviousRemoved);
	CHECK_EQ(ReadFile(installedExecutable), QByteArrayLiteral("build 101"));
	CHECK(QFileInfo(installedExecutable).isExecutable());
	CHECK(!QFileInfo(previous).exists());
}

TEST_CASE(TeagramUpdateReplacementRestoresMissingTarget) {
	auto root = QTemporaryDir();
	CHECK(root.isValid());
	if (!root.isValid()) {
		return;
	}
	const auto installed = QDir(root.path()).filePath(u"Teagram.app"_q);
	const auto staging
		= QDir(root.path()).filePath(u".teagram-update-staging"_q);
	const auto previous
		= QDir(root.path()).filePath(u".Teagram.previous.app"_q);
	const auto executable
		= QDir(previous).filePath(u"Contents/MacOS/Teagram"_q);
	CHECK(QDir().mkpath(QFileInfo(executable).absolutePath()));
	CHECK(WriteFile(executable, QByteArrayLiteral("build 100")));
	CHECK(QFile::setPermissions(
		executable,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner
			| QFileDevice::ExeOwner));
	auto lock = TeagramUpdateInstallLock(installed);
	CHECK(lock.acquired());
	if (!lock.acquired()) {
		return;
	}
	const auto recovered = RecoverTeagramUpdateReplacement(
		lock, installed, staging, previous, previous, false, false, false);
	CHECK(recovered.reason
		  == TeagramUpdateStartupRecoveryReason::RestoredPrevious);
	const auto restoredExecutable
		= QDir(installed).filePath(u"Contents/MacOS/Teagram"_q);
	CHECK_EQ(ReadFile(restoredExecutable), QByteArrayLiteral("build 100"));
	CHECK(QFileInfo(restoredExecutable).isExecutable());
	CHECK(!QFileInfo(previous).exists());
}

TEST_CASE(TeagramUpdateDownloadAcceptsProducerArtifacts) {
	const auto signingKey = GenerateTestSigningKey();
	if (!signingKey) {
		return;
	}
	auto generated = QTemporaryDir();
	CHECK(generated.isValid());
	if (!generated.isValid()) {
		return;
	}
	const auto keyPath = generated.filePath(u"fixture-key.pem"_q);
	if (!WritePrivateKey(signingKey->privateKey.get(), keyPath)) {
		return;
	}
	const auto archive = QByteArrayLiteral("producer-bound archive bytes");
	const auto package = ProducePackage(archive, keyPath, generated.path(),
										QByteArrayLiteral("dev"),
										QByteArrayLiteral("101"));
	if (!package) {
		return;
	}
	auto transport = std::make_unique<FixtureTransport>();
	auto *fixture = transport.get();
	fixture->addReleaseWithArtifacts(
		"teagram-build-101", "101", package->manifest, package->signature,
		archive);
	fixture->addReleaseList();
	auto staging = QTemporaryDir();
	CHECK(staging.isValid());
	if (!staging.isValid()) {
		return;
	}
	const auto trustedKey
		= std::optional<TeagramUpdatePublicKey>(signingKey->publicKey);
	TeagramUpdateDownloader downloader(std::move(transport), trustedKey,
										  staging.path());
	const auto result
		= downloader.checkForUpdates(100, TeagramUpdateChannel::Dev);
	CHECK(result.reason == TeagramUpdateDownloadReason::Available);
	CHECK(result.candidate != nullptr);
	if (result.candidate) {
		CHECK_EQ(result.candidate->metadata().build(), quint64(101));
		CHECK(result.candidate->metadata().channel()
			  == TeagramUpdateChannel::Dev);
		QFile downloaded(result.candidate->archivePath());
		CHECK(downloaded.open(QIODevice::ReadOnly));
		CHECK_EQ(downloaded.readAll(), archive);
	}
	CHECK(HasOnlyFixedSourceHosts(fixture->_requests));
}

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
	const auto archiveObjectsRedirect
		= QUrl(u"https://objects.githubusercontent.com/download/verified"_q);
	const auto archiveFinalRedirect = QUrl(
		u"https://release-assets.githubusercontent.com/download/final"_q);
	fixture->addAsset("teagram-build-102", "Teagram-macOS-arm64-102.zip",
					  RedirectResponse(archiveRedirect));
	fixture->addResponse(archiveRedirect,
						 RedirectResponse(archiveObjectsRedirect));
	fixture->addResponse(archiveObjectsRedirect,
						 RedirectResponse(archiveFinalRedirect));
	fixture->addResponse(archiveFinalRedirect, ResponseWithBody(archive));
	QTemporaryDir staging;
	CHECK(staging.isValid());
	TeagramUpdateDownloader dev(std::move(transport), trustedKey,
								staging.path());
	auto progress = std::vector<int>();
	auto devResult
		= dev.checkForUpdates(100, TeagramUpdateChannel::Dev,
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
		const auto permissions
			= QFileInfo(devResult.candidate->archivePath()).permissions();
		const auto ownerPermissions
			= permissions
			  & (QFileDevice::ReadOwner | QFileDevice::WriteOwner
				 | QFileDevice::ExeOwner);
		const auto groupAndOtherPermissions
			= permissions
			  & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
				 | QFileDevice::ExeGroup | QFileDevice::ReadOther
				 | QFileDevice::WriteOther | QFileDevice::ExeOther);
		CHECK(ownerPermissions
			  == (QFileDevice::ReadOwner | QFileDevice::WriteOwner));
		CHECK(groupAndOtherPermissions == QFileDevice::Permissions());
	}
	CHECK(HasOnlyFixedSourceHosts(fixture->_requests));
	CHECK_EQ(QDir(staging.path())
				 .entryList(QDir::Files | QDir::NoDotAndDotDot)
				 .size(),
			 1);
	devResult.candidate.reset();
	CHECK(QDir(staging.path())
			  .entryList(QDir::Files | QDir::NoDotAndDotDot)
			  .isEmpty());

	auto devOnly = std::make_unique<FixtureTransport>();
	devOnly->addRelease("teagram-build-101", "dev", "101",
						signingKey->privateKey.get(), archive, false);
	devOnly->addReleaseList();
	TeagramUpdateDownloader dev101(std::move(devOnly), trustedKey,
								   staging.path());
	auto dev101Result = dev101.checkForUpdates(100, TeagramUpdateChannel::Dev);
	CHECK(dev101Result.reason == TeagramUpdateDownloadReason::Available);
	CHECK(dev101Result.candidate != nullptr);
	if (dev101Result.candidate) {
		CHECK_EQ(dev101Result.candidate->metadata().build(), quint64(101));
		CHECK(dev101Result.candidate->metadata().channel()
			  == TeagramUpdateChannel::Dev);
	}
	dev101Result.candidate.reset();
	CHECK(QDir(staging.path())
			  .entryList(QDir::Files | QDir::NoDotAndDotDot)
			  .isEmpty());

	auto mainDevOnly = std::make_unique<FixtureTransport>();
	auto *devOnlyFixture = mainDevOnly.get();
	devOnlyFixture->addRelease("teagram-build-101", "dev", "101",
							   signingKey->privateKey.get(), archive, false);
	devOnlyFixture->addReleaseList();
	TeagramUpdateDownloader main(std::move(mainDevOnly), trustedKey,
								 staging.path());
	const auto mainResult
		= main.checkForUpdates(100, TeagramUpdateChannel::Main);
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
	const auto appCanaryContents
		= QByteArrayLiteral("installed app stays intact");
	const auto canaryPath = profile.path() + u"/profile-canary"_q;
	const auto canaryContents = QByteArrayLiteral("profile remains untouched");
	CHECK(WriteFile(appCanaryPath, appCanaryContents));
	CHECK(WriteFile(canaryPath, canaryContents));
	const auto runFailure
		= [&](std::unique_ptr<FixtureTransport> fixture,
			  const std::optional<TeagramUpdatePublicKey> &key,
			  TeagramUpdateDownloadReason expected,
			  bool cancelDuringArchive = false,
			  bool expectNoArchiveRequest = false) {
			  fixture->addReleaseList();
			  auto *rawFixture = fixture.get();
			  TeagramUpdateDownloader downloader(std::move(fixture), key,
												 staging.path());
			  if (cancelDuringArchive) {
				  rawFixture->_afterChunk = [&](const QUrl &url) {
					  if (url.path().endsWith(u".zip"_q)) {
						  downloader.cancel();
					  }
				  };
			  }
			  const auto result
				  = downloader.checkForUpdates(100, TeagramUpdateChannel::Dev);
			  CHECK(result.reason == expected);
			  CHECK(result.candidate == nullptr);
			  if (expectNoArchiveRequest) {
				  CHECK(std::none_of(rawFixture->_requests.begin(),
									 rawFixture->_requests.end(),
									 [](const QUrl &url) {
										 return url.path().endsWith(u".zip"_q);
									 }));
			  }
			  CHECK(QDir(staging.path())
						.entryList(QDir::Files | QDir::NoDotAndDotDot)
						.isEmpty());
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
							  "Teagram-macOS-arm64-101.zip",
							  ResponseWithBody(modified));
	runFailure(std::move(modifiedArchive), trustedKey,
			   TeagramUpdateDownloadReason::ArchiveRejected);

	auto unreachable = std::make_unique<FixtureTransport>();
	auto api = QUrl(
		u"https://api.github.com/repos/teagramhq/teagram-desktop/releases?per_page=100&page=1"_q);
	unreachable->addResponse(api, NetworkFailureResponse());
	runFailure(std::move(unreachable), trustedKey,
			   TeagramUpdateDownloadReason::NetworkFailure);

	auto redirect = std::make_unique<FixtureTransport>();
	redirect->addRelease("teagram-build-101", "dev", "101",
						 signingKey->privateKey.get(), archive);
	redirect->addAsset(
		"teagram-build-101", "teagram-update.json",
		RedirectResponse(QUrl(u"https://evil.example/update.json"_q)));
	runFailure(std::move(redirect), trustedKey,
			   TeagramUpdateDownloadReason::DisallowedRedirect);

	auto downgradedRedirect = std::make_unique<FixtureTransport>();
	downgradedRedirect->addRelease("teagram-build-101", "dev", "101",
								   signingKey->privateKey.get(), archive);
	downgradedRedirect->addAsset(
		"teagram-build-101", "teagram-update.json",
		RedirectResponse(QUrl(
			u"http://release-assets.githubusercontent.com/update.json"_q)));
	runFailure(std::move(downgradedRedirect), trustedKey,
			   TeagramUpdateDownloadReason::DisallowedRedirect);

	auto tooManyRedirects = std::make_unique<FixtureTransport>();
	tooManyRedirects->addRelease("teagram-build-101", "dev", "101",
								 signingKey->privateKey.get(), archive);
	const auto firstHop
		= QUrl(u"https://release-assets.githubusercontent.com/first"_q);
	const auto secondHop
		= QUrl(u"https://objects.githubusercontent.com/second"_q);
	const auto thirdHop
		= QUrl(u"https://release-assets.githubusercontent.com/third"_q);
	const auto fourthHop
		= QUrl(u"https://objects.githubusercontent.com/fourth"_q);
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
						  signingKey->privateKey.get(), archive, false,
						  QByteArrayLiteral("536870913"));
	runFailure(std::move(oversized), trustedKey,
			   TeagramUpdateDownloadReason::LimitExceeded);

	auto oversizedManifest = std::make_unique<FixtureTransport>();
	oversizedManifest->addRelease("teagram-build-101", "dev", "101",
								  signingKey->privateKey.get(), archive);
	oversizedManifest->addAsset(
		"teagram-build-101", "teagram-update.json",
		ResponseWithBody(
			QByteArray(kMaximumTeagramUpdateManifestSize + 1, 'x')));
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
	const auto noKeyResult
		= noProductionKey.checkForUpdates(100, TeagramUpdateChannel::Dev);
	CHECK(noKeyResult.reason
		  == TeagramUpdateDownloadReason::VerificationFailed);
	CHECK(noKeyResult.candidate == nullptr);
	CHECK(QDir(staging.path())
			  .entryList(QDir::Files | QDir::NoDotAndDotDot)
			  .isEmpty());
	QFile canary(canaryPath);
	CHECK(canary.open(QIODevice::ReadOnly));
	CHECK_EQ(canary.readAll(), canaryContents);
	QFile appCanary(appCanaryPath);
	CHECK(appCanary.open(QIODevice::ReadOnly));
	CHECK_EQ(appCanary.readAll(), appCanaryContents);
}
