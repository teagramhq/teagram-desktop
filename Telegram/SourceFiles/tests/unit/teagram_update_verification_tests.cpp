/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_update_verification.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace Core;

namespace {

const auto kArchive = QByteArrayView("archive", 7);
constexpr auto kArchiveHash
	= "0eb3e36bfb24dcd9bb1d1bece1531216b59539a8fde17ee80224af0653c92aa3";

struct PkeyDeleter {
	void operator()(EVP_PKEY *value) const { EVP_PKEY_free(value); }
};

struct PkeyContextDeleter {
	void operator()(EVP_PKEY_CTX *value) const { EVP_PKEY_CTX_free(value); }
};

struct DigestContextDeleter {
	void operator()(EVP_MD_CTX *value) const { EVP_MD_CTX_free(value); }
};

struct BioDeleter {
	void operator()(BIO *value) const { BIO_free(value); }
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
	auto key = Pkey(rawKey);
	auto result = TestSigningKey{std::move(key), {}};
	auto size = result.publicKey.size();
	if (EVP_PKEY_get_raw_public_key(result.privateKey.get(),
									result.publicKey.data(), &size)
			!= 1
		|| size != result.publicKey.size()) {
		CHECK(false);
		return std::nullopt;
	}
	return result;
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

[[nodiscard]] QByteArray CreateArchive(const QString &directory) {
	const auto path = QDir(directory).filePath(u"fixture.zip"_q);
	const auto source = QString::fromLatin1(
		"import sys, zipfile\n"
		"with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as "
		"archive:\n"
		"    archive.writestr('Teagram.app/Contents/Info.plist', b'fixture')\n"
		"    archive.writestr("
		"'Teagram.app/Contents/MacOS/Teagram', b'executable fixture')\n");
	const auto succeeded
		= RunProcess(QString::fromUtf8(TDESKTOP_TEST_PYTHON_EXECUTABLE),
					 {u"-c"_q, source, path});
	if (!succeeded) {
		return {};
	}
	return ReadFile(path);
}

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	const auto opened = file.open(QIODevice::WriteOnly);
	CHECK(opened);
	if (!opened) {
		return false;
	}
	const auto written = file.write(bytes);
	CHECK_EQ(written, qint64(bytes.size()));
	return written == bytes.size();
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
	QByteArray archive;
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
	const auto archiveWritten = WriteFile(archivePath, archive);
	if (!archiveWritten) {
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
	return ProducedPackage{
		archive,
		ReadFile(QDir(output).filePath(u"teagram-update.json"_q)),
		ReadFile(QDir(output).filePath(u"teagram-update.sig"_q)),
	};
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

[[nodiscard]] QByteArray
Manifest(const QByteArray &channel = QByteArrayLiteral("main"),
		 const QByteArray &build = QByteArrayLiteral("101")) {
	return QByteArrayLiteral(
			   "{\"format\":1,\"product\":\"io.teagram.desktop\","
			   "\"repo\":\"teagramhq/teagram-desktop\",\"arch\":\"arm64\","
			   "\"channel\":\"")
		   + channel + QByteArrayLiteral("\",\"build\":\"") + build
		   + QByteArrayLiteral("\",\"commit\":\"")
		   + QByteArrayLiteral("0123456789abcdef0123456789abcdef01234567")
		   + QByteArrayLiteral("\",\"asset_name\":\"Teagram-macOS-arm64-")
		   + build
		   + QByteArrayLiteral(
			   ".zip\",\"asset_size\":\"7\",\"asset_sha256\":\"")
		   + kArchiveHash + QByteArrayLiteral("\",\"min_os\":\"13.0\"}");
}

[[nodiscard]] QByteArray ReplaceOnce(QByteArray source,
									 const QByteArray &before,
									 const QByteArray &after) {
	const auto index = source.indexOf(before);
	if (index < 0) {
		CHECK(index >= 0);
		return {};
	}
	source.replace(index, before.size(), after);
	return source;
}

[[nodiscard]] TeagramUpdateVerificationResult
Verify(const QByteArray &manifest, const QByteArray &signature,
	   QByteArrayView archive, quint64 installedBuild,
	   TeagramUpdateChannel installedChannel,
	   const std::optional<TeagramUpdatePublicKey> &publicKey) {
	return VerifyTeagramUpdatePackageForTests(manifest, signature, archive,
											  installedBuild, installedChannel,
											  publicKey);
}

[[nodiscard]] TeagramUpdateVerificationResult
VerifySigned(const QByteArray &manifest, EVP_PKEY *privateKey,
			 quint64 installedBuild, TeagramUpdateChannel installedChannel,
			 const std::optional<TeagramUpdatePublicKey> &publicKey) {
	return Verify(manifest, SignManifest(manifest, privateKey), kArchive,
				  installedBuild, installedChannel, publicKey);
}

} // namespace

TEST_CASE(TeagramUpdateAcceptsEligibleSignedPackage) {
	const auto key = GenerateTestSigningKey();
	if (!key) {
		return;
	}
	const auto publicKey
		= std::optional<TeagramUpdatePublicKey>(key->publicKey);
	const auto main = VerifySigned(Manifest(), key->privateKey.get(), 100,
								   TeagramUpdateChannel::Main, publicKey);
	CHECK(main.eligible());
	CHECK(main.reason == TeagramUpdateVerificationReason::Eligible);
	CHECK(main.package.has_value());
	if (main.package) {
		CHECK_EQ(main.package->build, quint64(101));
		CHECK(main.package->channel == TeagramUpdateChannel::Main);
		CHECK_EQ(main.package->commit,
				 u"0123456789abcdef0123456789abcdef01234567"_q);
		CHECK_EQ(main.package->assetName, u"Teagram-macOS-arm64-101.zip"_q);
		CHECK_EQ(main.package->assetSize, quint64(7));
		CHECK_EQ(
			main.package->assetSha256,
			u"0eb3e36bfb24dcd9bb1d1bece1531216b59539a8fde17ee80224af0653c92aa3"_q);
		CHECK_EQ(main.package->minOs, u"13.0"_q);
	}
	for (const auto &channel :
		 {QByteArrayLiteral("dev"), QByteArrayLiteral("main")}) {
		const auto result = VerifySigned(
			Manifest(channel, QByteArrayLiteral("101")), key->privateKey.get(),
			100, TeagramUpdateChannel::Dev, publicKey);
		CHECK(result.eligible());
	}
	const auto maximumBuild = QByteArrayLiteral("18446744073709551615");
	const auto maximum = VerifySigned(
		Manifest(QByteArrayLiteral("main"), maximumBuild),
		key->privateKey.get(), 100, TeagramUpdateChannel::Main, publicKey);
	CHECK(maximum.eligible());
	if (maximum.package) {
		CHECK_EQ(maximum.package->build, std::numeric_limits<quint64>::max());
	}
}

TEST_CASE(TeagramUpdateRejectsUntrustedOrIneligiblePackage) {
	const auto key = GenerateTestSigningKey();
	const auto otherKey = GenerateTestSigningKey();
	if (!key || !otherKey) {
		return;
	}
	const auto publicKey
		= std::optional<TeagramUpdatePublicKey>(key->publicKey);
	const auto valid = Manifest();
	const auto signature = SignManifest(valid, key->privateKey.get());
	const auto rejected = [&](const QByteArray &manifest) {
		const auto result = VerifySigned(manifest, key->privateKey.get(), 100,
										 TeagramUpdateChannel::Main, publicKey);
		CHECK(!result.eligible());
		return result.reason;
	};
	const auto rejects = [&](const QByteArray &manifest) {
		CHECK(rejected(manifest) != TeagramUpdateVerificationReason::Eligible);
	};

	const auto tamperedManifest
		= ReplaceOnce(valid, QByteArrayLiteral("\"format\":1"),
					  QByteArrayLiteral("\"format\":2"));
	const auto tampered = Verify(tamperedManifest, signature, kArchive, 100,
								 TeagramUpdateChannel::Main, publicKey);
	CHECK(tampered.reason == TeagramUpdateVerificationReason::InvalidSignature);
	auto tamperedArchive = QByteArray(kArchive.data(), int(kArchive.size()));
	tamperedArchive[6] = 'f';
	const auto hashMismatch = Verify(valid, signature, tamperedArchive, 100,
									 TeagramUpdateChannel::Main, publicKey);
	CHECK(hashMismatch.reason
		  == TeagramUpdateVerificationReason::ArchiveHashMismatch);
	const auto unsignedPackage = Verify(valid, {}, kArchive, 100,
										TeagramUpdateChannel::Main, publicKey);
	CHECK(unsignedPackage.reason
		  == TeagramUpdateVerificationReason::InvalidSignature);
	const auto wrongKey
		= Verify(valid, signature, kArchive, 100, TeagramUpdateChannel::Main,
				 std::optional<TeagramUpdatePublicKey>(otherKey->publicKey));
	CHECK(wrongKey.reason == TeagramUpdateVerificationReason::InvalidSignature);
	const auto noKey = Verify(valid, signature, kArchive, 100,
							  TeagramUpdateChannel::Main, std::nullopt);
	CHECK(noKey.reason == TeagramUpdateVerificationReason::MissingTrustedKey);
	const auto production = VerifyTeagramUpdatePackage(
		valid, signature, kArchive, 100, TeagramUpdateChannel::Main);
	CHECK(!production.eligible());

	const auto dev102 = VerifySigned(
		Manifest(QByteArrayLiteral("dev"), QByteArrayLiteral("102")),
		key->privateKey.get(), 100, TeagramUpdateChannel::Main, publicKey);
	CHECK(dev102.reason == TeagramUpdateVerificationReason::IneligibleChannel);
	for (const auto &build :
		 {QByteArrayLiteral("100"), QByteArrayLiteral("99")}) {
		const auto result = VerifySigned(
			Manifest(QByteArrayLiteral("main"), build), key->privateKey.get(),
			100, TeagramUpdateChannel::Main, publicKey);
		CHECK(result.reason == TeagramUpdateVerificationReason::NotNewer);
	}
	const auto stillOn102 = VerifySigned(
		Manifest(QByteArrayLiteral("main"), QByteArrayLiteral("102")),
		key->privateKey.get(), 102, TeagramUpdateChannel::Main, publicKey);
	CHECK(stillOn102.reason == TeagramUpdateVerificationReason::NotNewer);
	const auto advancesFrom102 = VerifySigned(
		Manifest(QByteArrayLiteral("main"), QByteArrayLiteral("103")),
		key->privateKey.get(), 102, TeagramUpdateChannel::Main, publicKey);
	CHECK(advancesFrom102.eligible());

	const auto duplicate
		= ReplaceOnce(valid, QByteArrayLiteral("\"format\":1"),
					  QByteArrayLiteral("\"format\":1,\"format\":1"));
	CHECK(rejected(duplicate)
		  == TeagramUpdateVerificationReason::InvalidManifest);
	const auto escapedDuplicate
		= ReplaceOnce(valid, QByteArrayLiteral("\"format\":1"),
					  QByteArrayLiteral("\"format\":1,\"\\u0066ormat\":1"));
	CHECK(rejected(escapedDuplicate)
		  == TeagramUpdateVerificationReason::InvalidManifest);
	const auto oversized = valid + QByteArray(16385 - valid.size(), ' ');
	CHECK(rejected(oversized)
		  == TeagramUpdateVerificationReason::ManifestTooLarge);
	const auto overflowBuild
		= ReplaceOnce(valid, QByteArrayLiteral("\"build\":\"101\""),
					  QByteArrayLiteral("\"build\":\"18446744073709551616\""));
	CHECK(rejected(overflowBuild)
		  == TeagramUpdateVerificationReason::InvalidBuild);
	const auto overflowSize = ReplaceOnce(
		valid, QByteArrayLiteral("\"asset_size\":\"7\""),
		QByteArrayLiteral("\"asset_size\":\"18446744073709551616\""));
	CHECK(rejected(overflowSize)
		  == TeagramUpdateVerificationReason::InvalidAssetSize);
	const auto maximumSize = ReplaceOnce(
		valid, QByteArrayLiteral("\"asset_size\":\"7\""),
		QByteArrayLiteral("\"asset_size\":\"18446744073709551615\""));
	CHECK(rejected(maximumSize)
		  == TeagramUpdateVerificationReason::ArchiveSizeMismatch);
	const auto nonCanonicalBuild
		= ReplaceOnce(valid, QByteArrayLiteral("\"build\":\"101\""),
					  QByteArrayLiteral("\"build\":\"0101\""));
	CHECK(rejected(nonCanonicalBuild)
		  == TeagramUpdateVerificationReason::InvalidBuild);
	const auto sizeMismatch
		= ReplaceOnce(valid, QByteArrayLiteral("\"asset_size\":\"7\""),
					  QByteArrayLiteral("\"asset_size\":\"6\""));
	CHECK(rejected(sizeMismatch)
		  == TeagramUpdateVerificationReason::ArchiveSizeMismatch);

	const auto wrongIdentity = std::vector<std::pair<QByteArray, QByteArray>>{
		{QByteArrayLiteral("\"product\":\"io.teagram.desktop\""),
		 QByteArrayLiteral("\"product\":\"other.product\"")},
		{QByteArrayLiteral("\"repo\":\"teagramhq/teagram-desktop\""),
		 QByteArrayLiteral("\"repo\":\"other/repository\"")},
		{QByteArrayLiteral("\"arch\":\"arm64\""),
		 QByteArrayLiteral("\"arch\":\"x86_64\"")},
		{QByteArrayLiteral("\"asset_name\":\"Teagram-macOS-arm64-101.zip\""),
		 QByteArrayLiteral("\"asset_name\":\"../../outside.zip\"")},
		{QByteArrayLiteral("\"format\":1"), QByteArrayLiteral("\"format\":2")},
		{QByteArrayLiteral("\"channel\":\"main\""),
		 QByteArrayLiteral("\"channel\":\"beta\"")},
	};
	for (const auto &[before, after] : wrongIdentity) {
		rejects(ReplaceOnce(valid, before, after));
	}

	const auto wrongTypes = std::vector<std::pair<QByteArray, QByteArray>>{
		{QByteArrayLiteral("\"format\":1"),
		 QByteArrayLiteral("\"format\":\"1\"")},
		{QByteArrayLiteral("\"product\":\"io.teagram.desktop\""),
		 QByteArrayLiteral("\"product\":1")},
		{QByteArrayLiteral("\"repo\":\"teagramhq/teagram-desktop\""),
		 QByteArrayLiteral("\"repo\":1")},
		{QByteArrayLiteral("\"arch\":\"arm64\""),
		 QByteArrayLiteral("\"arch\":1")},
		{QByteArrayLiteral("\"channel\":\"main\""),
		 QByteArrayLiteral("\"channel\":1")},
		{QByteArrayLiteral("\"build\":\"101\""),
		 QByteArrayLiteral("\"build\":101")},
		{QByteArrayLiteral(
			 "\"commit\":\"0123456789abcdef0123456789abcdef01234567\""),
		 QByteArrayLiteral("\"commit\":1")},
		{QByteArrayLiteral("\"asset_name\":\"Teagram-macOS-arm64-101.zip\""),
		 QByteArrayLiteral("\"asset_name\":1")},
		{QByteArrayLiteral("\"asset_size\":\"7\""),
		 QByteArrayLiteral("\"asset_size\":7")},
		{QByteArrayLiteral("\"asset_sha256\":\"") + kArchiveHash
			 + QByteArrayLiteral("\""),
		 QByteArrayLiteral("\"asset_sha256\":1")},
		{QByteArrayLiteral("\"min_os\":\"13.0\""),
		 QByteArrayLiteral("\"min_os\":1")},
		{QByteArrayLiteral("\"min_os\":\"13.0\""),
		 QByteArrayLiteral("\"min_os\":[]")},
	};
	for (const auto &[before, after] : wrongTypes) {
		rejects(ReplaceOnce(valid, before, after));
	}
	const auto extra = valid.left(valid.size() - 1)
					   + QByteArrayLiteral(",\"unexpected\":true}");
	rejects(extra);
	const auto missing
		= ReplaceOnce(valid, QByteArrayLiteral(",\"min_os\":\"13.0\""), {});
	rejects(missing);
}

TEST_CASE(TeagramUpdateGeneratedPackageRoundTrip) {
	const auto key = GenerateTestSigningKey();
	if (!key) {
		return;
	}
	auto temporaryDirectory = QTemporaryDir();
	CHECK(temporaryDirectory.isValid());
	if (!temporaryDirectory.isValid()) {
		return;
	}
	const auto keyPath = temporaryDirectory.filePath(u"fixture-key.pem"_q);
	if (!WritePrivateKey(key->privateKey.get(), keyPath)) {
		return;
	}
	const auto archive = CreateArchive(temporaryDirectory.path());
	CHECK(!archive.isEmpty());
	if (archive.isEmpty()) {
		return;
	}
	const auto dev = ProducePackage(
		archive, keyPath, temporaryDirectory.filePath(u"dev"_q),
		QByteArrayLiteral("dev"), QByteArrayLiteral("101"));
	const auto main = ProducePackage(
		archive, keyPath, temporaryDirectory.filePath(u"main"_q),
		QByteArrayLiteral("main"), QByteArrayLiteral("102"));
	if (!dev || !main) {
		CHECK(dev.has_value());
		CHECK(main.has_value());
		return;
	}
	const auto publicKey
		= std::optional<TeagramUpdatePublicKey>(key->publicKey);
	const auto devForDev = Verify(dev->manifest, dev->signature, dev->archive,
								  100, TeagramUpdateChannel::Dev, publicKey);
	CHECK(devForDev.eligible());
	CHECK(devForDev.package.has_value());
	if (devForDev.package) {
		CHECK_EQ(devForDev.package->build, quint64(101));
		CHECK_EQ(devForDev.package->assetName,
				 u"Teagram-macOS-arm64-101.zip"_q);
	}
	const auto devForMain = Verify(dev->manifest, dev->signature, dev->archive,
								   100, TeagramUpdateChannel::Main, publicKey);
	CHECK(devForMain.reason
		  == TeagramUpdateVerificationReason::IneligibleChannel);

	const auto mainForMain
		= Verify(main->manifest, main->signature, main->archive, 100,
				 TeagramUpdateChannel::Main, publicKey);
	const auto mainForDev
		= Verify(main->manifest, main->signature, main->archive, 100,
				 TeagramUpdateChannel::Dev, publicKey);
	CHECK(mainForMain.eligible());
	CHECK(mainForDev.eligible());
	CHECK(mainForMain.package.has_value());
	if (mainForMain.package) {
		CHECK_EQ(mainForMain.package->build, quint64(102));
		CHECK_EQ(mainForMain.package->assetName,
				 u"Teagram-macOS-arm64-102.zip"_q);
	}

	auto changed = dev->archive;
	changed[0] = char(changed[0] ^ 1);
	const auto tampered = Verify(dev->manifest, dev->signature, changed, 100,
								 TeagramUpdateChannel::Dev, publicKey);
	CHECK(tampered.reason
		  == TeagramUpdateVerificationReason::ArchiveHashMismatch);
}
