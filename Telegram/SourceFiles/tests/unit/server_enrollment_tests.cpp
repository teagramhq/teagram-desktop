/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "config.h"
#include "intro/intro_server_discovery.h"
#include "main/main_account.h"
#include "main/main_account_persistence.h"
#include "main/main_domain.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/mtproto_server_enrollment.h"
#include "mtproto/mtproto_server_discovery.h"
#include "mtproto/session.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"
#include "storage/storage_server_forget_startup.h"
#include "tests/unit/server_endpoint_refresh_test_support.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtCore/Qt>
#include <QtGui/QKeyEvent>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Storage {

struct AccountTestPeer {
	static void ReadPrefs(Account &account, FileKey key) {
		account._prefsKey = key;
		account.readPrefs();
	}

	static void ReadStoredCustomServerPin(Account &account) {
		account.readStoredCustomServerPin();
	}

	static FileKey PrefsKey(const Account &account) {
		return account._prefsKey;
	}

	static bool HasTelegramStickerBotTokenPreference(const Account &account) {
		return account.readPrefGeneric("telegram_sticker_bot_token")
			.has_value();
	}
	static bool TokenClearPending(const Account &account) {
		return account._telegramStickerBotTokenClearPending;
	}
};

} // namespace Storage

namespace {

using namespace MTP;

const char kEnrollmentServerKey[] = "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n\
5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n\
62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n\
+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n\
t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n\
5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n\
-----END RSA PUBLIC KEY-----";

const char kDifferentEnrollmentServerKey[] = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAyMEdY1aR+sCR3ZSJrtztKTKqigvO/vBfqACJLZtS7QMgCGXJ6XIR
yy7mx66W0/sOFa7/1mAZtEoIokDP3ShoqF4fVNb6XeqgQfaUHd8wJpDWHcR2OFwv
plUUI1PLTktZ9uW2WE23b+ixNwJjJGwBDJPQEQFBE+vfmH0JP503wr5INS1poWg/
j25sIWeYPHYeOrFp/eXaqhISP6G+q2IeTaWTXpwZj4LzXq5YOpk4bYEQ6mvRq7D1
aHWfYmlEGepfaYR8Q0YqvvhYtMte3ITnuSJs171+GDqpdKcSwHnd6FudwGO4pcCO
j4WcDuXc2CTHgH8gFTNhp/Y8/SpDOhvn9QIDAQAB
-----END RSA PUBLIC KEY-----)";

[[nodiscard]] std::shared_ptr<details::RSAPublicKey>
MakeEnrollmentServerKey() {
	return std::make_shared<details::RSAPublicKey>(bytes::make_span(
		kEnrollmentServerKey,
		sizeof(kEnrollmentServerKey) - 1));
}

[[nodiscard]] std::shared_ptr<MTP::Config> MakeEnrollmentConfig() {
	auto result = std::make_shared<MTP::Config>(MTP::Environment::Production);
	Expects(result->dcOptions().setCustomServer(CustomServer{
		.dcId = 2,
		.ip = "10.4.1.7",
		.port = 8443,
		.key = MakeEnrollmentServerKey(),
	}));
	return result;
}

[[nodiscard]] CustomServer MakeEndpointRefreshPin(const char *ip) {
	return CustomServer{
		.dcId = 2,
		.ip = ip,
		.port = 2443,
		.key = MakeEnrollmentServerKey(),
		.serverSelection = "teagram.tailaa4918.ts.net",
		.discoveryPolicy = ServerDiscoveryPolicy::PublicHttps,
		.discoveryOrigin = "https://teagram.tailaa4918.ts.net/"
						   ".well-known/teagramd/client",
	};
}

[[nodiscard]] MTP::AuthKeyPtr MakeEnrollmentStorageKey() {
	return std::make_shared<MTP::AuthKey>(
		MTP::AuthKey::Data{ { gsl::byte{} } });
}

[[nodiscard]] MTP::AuthKeyPtr MakeEndpointRefreshAuthKey(int value) {
	auto data = MTP::AuthKey::Data();
	std::fill(data.begin(), data.end(), static_cast<gsl::byte>(value));
	return std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile,
										  MTP::DcId(2), data);
}

[[nodiscard]] QByteArray
SerializeEndpointRefreshAuthKey(const MTP::AuthKeyPtr &key) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	key->write(stream);
	return result;
}

[[nodiscard]] MTP::AuthKeyPtr
RestoreEndpointRefreshAuthKey(const QByteArray &serialized) {
	if (serialized.size() != MTP::AuthKey::kSize) {
		return nullptr;
	}
	auto data = MTP::AuthKey::Data();
	auto stream = QDataStream(serialized);
	if (stream.readRawData(reinterpret_cast<char *>(data.data()),
						   int(data.size()))
		!= int(data.size())) {
		return nullptr;
	}
	return std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile,
										  MTP::DcId(2), data);
}

[[nodiscard]] bool WriteEnrollmentAccountList(
		const QString &dataName,
		const QString &basePath,
		const MTP::AuthKeyPtr &localKey,
		const std::vector<int> &indices,
		int active) {
	const auto salt = QByteArray(LocalEncryptSaltSize, '\x42');
	const auto passcodeKey = Storage::details::CreateLocalKey(
		QByteArray(),
		salt);
	Storage::details::EncryptedDescriptor localKeyData(MTP::AuthKey::kSize);
	localKey->write(localKeyData.stream);
	const auto localKeyEncrypted = Storage::details::PrepareEncrypted(
		localKeyData,
		passcodeKey);

	Storage::details::EncryptedDescriptor info(
		sizeof(qint32) * (2 + indices.size()));
	info.stream << qint32(indices.size());
	for (const auto index : indices) {
		info.stream << qint32(index);
	}
	info.stream << qint32(active);

	Storage::details::FileWriteDescriptor file(
		u"key_"_q + dataName,
		basePath,
		true);
	file.writeData(salt);
	file.writeData(localKeyEncrypted);
	file.writeEncrypted(info, localKey);
	return file.finish();
}

[[nodiscard]] std::unique_ptr<Storage::Account> MakeEnrollmentStorageAccount(
		const QString &basePath,
		const MTP::AuthKeyPtr &key,
		Fn<QByteArray()> serializeMtpAuthorization = nullptr,
		Fn<void(const QByteArray &)> restoreMtpAuthorization = nullptr,
		Fn<bool()> writeMtpAuthorizationOverride = nullptr,
		const QString &tempPath = {},
		const QString &databasePath = {},
		bool hasStoredCustomServer = false,
		bool mtpAuthorizationWriteFailed = false) {
	return std::make_unique<Storage::Account>(
		basePath,
		key,
		MakeEnrollmentConfig(),
		hasStoredCustomServer,
		std::move(serializeMtpAuthorization),
		std::move(restoreMtpAuthorization),
		std::move(writeMtpAuthorizationOverride),
		tempPath,
		databasePath,
		0,
		mtpAuthorizationWriteFailed);
}

[[nodiscard]] std::optional<Storage::FileKey> ReadEnrollmentPrefsKey(
		const QString &basePath,
		const MTP::AuthKeyPtr &key) {
	Storage::details::FileReadDescriptor file;
	if (!Storage::details::ReadFile(file, u"map"_q, basePath)) {
		return std::nullopt;
	}

	QByteArray legacySalt, legacyKey, encrypted;
	file.stream >> legacySalt >> legacyKey >> encrypted;
	if (!Storage::details::CheckStreamStatus(file.stream)) {
		return std::nullopt;
	}

	Storage::details::EncryptedDescriptor map;
	if (!Storage::details::DecryptLocal(map, encrypted, key)) {
		return std::nullopt;
	}

	quint32 keyType = 0;
	Storage::FileKey prefsKey = 0;
	map.stream >> keyType >> prefsKey;
	return Storage::details::CheckStreamStatus(map.stream)
		&& keyType == 0x1e
		&& prefsKey != 0
		? std::optional<Storage::FileKey>(prefsKey)
		: std::nullopt;
}

[[nodiscard]] bool HasReadableEnrollmentMap(
		const QString &basePath,
		const MTP::AuthKeyPtr &key) {
	return ReadEnrollmentPrefsKey(basePath, key).has_value();
}

[[nodiscard]] std::unique_ptr<MTP::Config> ReadEnrollmentConfig(
		const QString &basePath,
		const MTP::AuthKeyPtr &key) {
	Storage::details::FileReadDescriptor file;
	if (!Storage::details::ReadEncryptedFile(
			file,
			u"config"_q,
			basePath,
			key)) {
		return nullptr;
	}

	QByteArray serialized;
	file.stream >> serialized;
	if (!Storage::details::CheckStreamStatus(file.stream)) {
		return nullptr;
	}
	return MTP::Config::FromSerialized(serialized);
}

[[nodiscard]] bool HasReadableEnrollmentConfig(
		const QString &basePath,
		const MTP::AuthKeyPtr &key) {
	const auto restored = ReadEnrollmentConfig(basePath, key);
	return restored != nullptr && restored->hasCustomServer();
}

void AppendBigEndian(QByteArray &target, quint32 value) {
	target.append(char(value >> 24));
	target.append(char(value >> 16));
	target.append(char(value >> 8));
	target.append(char(value));
}

void AppendBigEndian(QByteArray &target, quint16 value) {
	target.append(char(value >> 8));
	target.append(char(value));
}

[[nodiscard]] QByteArray LocalEndpointRefreshResponse(const QByteArray &nonce) {
	const auto der = MakeEnrollmentServerKey()->getSubjectPublicKeyInfo();
	auto result = QByteArray("telegramd-key-r1", 16) + nonce;
	AppendBigEndian(result, quint32(6 + der.size()));
	AppendBigEndian(result, quint32(2));
	AppendBigEndian(result, quint16(der.size()));
	result += QByteArray(reinterpret_cast<const char *>(der.data()),
						 int(der.size()));
	return result;
}

struct EndpointRefreshDiscoveryFixture {
	MTP::ServerSelectionCheck selection;
	QString origin;
	MTP::ServerDiscoveryResult result;
	bool oldEndpointUnreachable = false;
};

[[nodiscard]] std::optional<EndpointRefreshDiscoveryFixture>
DiscoverMovedEndpoint() {
	QTcpServer listener;
	if (!listener.listen(QHostAddress::LocalHost)) {
		return std::nullopt;
	}
	const auto selection = MTP::CheckServerSelection(
		u"127.0.0.1:"_q + QString::number(listener.serverPort()));
	if (!selection.valid()) {
		return std::nullopt;
	}
	const auto origin = u"local:"_q + selection.normalizedSelection;
	Intro::details::ServerEndpointRefreshDiscovery discovery;
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	auto completed = false;
	auto failed = false;
	auto result = std::optional<MTP::ServerDiscoveryResult>();
	QObject::connect(&listener, &QTcpServer::newConnection, &listener, [&] {
		while (listener.hasPendingConnections()) {
			auto socket = listener.nextPendingConnection();
			auto request = std::make_shared<QByteArray>();
			auto responded = std::make_shared<bool>(false);
			const auto respond = [=] {
				*request += socket->readAll();
				if (*responded || request->size() < 48) {
					return;
				}
				*responded = true;
				if (request->left(16) != QByteArray("telegramd-key-v1", 16)) {
					socket->abort();
					return;
				}
				socket->write(
					LocalEndpointRefreshResponse(request->mid(16, 32)));
				socket->disconnectFromHost();
			};
			QObject::connect(socket, &QTcpSocket::readyRead, socket, respond);
			QObject::connect(socket, &QTcpSocket::disconnected, socket,
							 &QObject::deleteLater);
			if (socket->bytesAvailable()) {
				respond();
			}
		}
	});
	const auto started
		= discovery.start(selection, origin,
						  {
							  .finished =
								  [&](MTP::ServerDiscoveryResult value) {
									  result = std::move(value);
									  completed = true;
									  loop.quit();
								  },
							  .failed =
								  [&](bool) {
									  failed = true;
									  loop.quit();
								  },
						  });
	if (!started) {
		return std::nullopt;
	}
	QTcpSocket oldEndpoint;
	oldEndpoint.connectToHost(QHostAddress("127.0.0.2"), listener.serverPort());
	const auto oldEndpointUnreachable = !oldEndpoint.waitForConnected(100);
	if (!completed && !failed) {
		timeout.start(5000);
		loop.exec();
	}
	discovery.cancel();
	if (!completed || failed || !result || !oldEndpointUnreachable) {
		return std::nullopt;
	}
	return EndpointRefreshDiscoveryFixture{
		.selection = selection,
		.origin = origin,
		.result = std::move(*result),
		.oldEndpointUnreachable = oldEndpointUnreachable,
	};
}

TEST_CASE(ServerForgetStartupLoadsPinnedStateWithoutForgetMarker) {
	auto events = std::vector<int>();
	auto skippedMtpData = true;
	const auto result = Storage::details::ProcessServerForgetStartup(
		false,
		[&](bool skipMtpData) {
			events.push_back(1);
			skippedMtpData = skipMtpData;
		},
		[&] { events.push_back(2); },
		[&] {
			events.push_back(3);
			return true;
		},
		[&] { events.push_back(4); },
		[&] { events.push_back(5); });

	CHECK(result == Storage::details::ServerForgetStartupResult::Loaded);
	CHECK(!skippedMtpData);
	CHECK_EQ(int(events.size()), 4);
	CHECK_EQ(events[0], 1);
	CHECK_EQ(events[1], 2);
	CHECK_EQ(events[2], 4);
	CHECK_EQ(events[3], 5);
}

TEST_CASE(ServerForgetStartupCompletesTombstoneBeforeLoadingConfig) {
	auto events = std::vector<int>();
	auto skippedMtpData = false;
	const auto result = Storage::details::ProcessServerForgetStartup(
		true,
		[&](bool skipMtpData) {
			events.push_back(1);
			skippedMtpData = skipMtpData;
		},
		[&] { events.push_back(2); },
		[&] {
			events.push_back(3);
			return true;
		},
		[&] { events.push_back(4); },
		[&] { events.push_back(5); });

	CHECK(result == Storage::details::ServerForgetStartupResult::Forgotten);
	CHECK(skippedMtpData);
	CHECK_EQ(int(events.size()), 3);
	CHECK_EQ(events[0], 1);
	CHECK_EQ(events[1], 2);
	CHECK_EQ(events[2], 3);
}

TEST_CASE(ServerForgetStartupBlocksWhenTombstoneCannotComplete) {
	auto events = std::vector<int>();
	auto skippedMtpData = false;
	const auto result = Storage::details::ProcessServerForgetStartup(
		true,
		[&](bool skipMtpData) {
			events.push_back(1);
			skippedMtpData = skipMtpData;
		},
		[&] { events.push_back(2); },
		[&] {
			events.push_back(3);
			return false;
		},
		[&] { events.push_back(4); },
		[&] { events.push_back(5); });

	CHECK(result == Storage::details::ServerForgetStartupResult::Blocked);
	CHECK(skippedMtpData);
	CHECK_EQ(int(events.size()), 3);
	CHECK_EQ(events[0], 1);
	CHECK_EQ(events[1], 2);
	CHECK_EQ(events[2], 3);
}

TEST_CASE(ReplaceInvalidatesQueuedStopFromPreviousEnrollment) {
	ServerEnrollmentGate gate;
	CHECK(gate.start());
	CHECK(gate.pause());
	CHECK(!gate.networkAllowed());

	const auto staleStop = gate.stopToken();
	const auto resumed = gate.resume();
	CHECK(resumed.resumed);
	CHECK(resumed.wasStarted);
	CHECK(gate.networkAllowed());
	CHECK(!gate.stopTokenIsCurrent(staleStop));

	auto stopApplied = false;
	if (gate.stopTokenIsCurrent(staleStop)) {
		stopApplied = true;
	}
	CHECK(!stopApplied);
}

TEST_CASE(UnconfirmedEnrollmentDoesNotAllowNetwork) {
	ServerEnrollmentGate gate(true);
	CHECK(!gate.networkAllowed());
	CHECK(!gate.start());
	CHECK(!gate.pause());

	const auto resumed = gate.resume();
	CHECK(resumed.resumed);
	CHECK(!resumed.wasStarted);
	// Clearing the pause does not create a session. The owning instance
	// starts it only after the confirmed pin has been persisted.
	CHECK(!gate.networkAllowed());
}

TEST_CASE(ConfirmedEnrollmentStartsNetworkAfterPinPersistence) {
	ServerEnrollmentGate gate(true);
	auto persisted = false;
	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[&] {
			persisted = true;
			return true;
		},
		[&] {
			const auto resumed = gate.resume();
			CHECK(resumed.resumed);
			CHECK(!resumed.wasStarted);
			CHECK(persisted);
			CHECK(gate.start());
		});

	CHECK(committed);
	CHECK(persisted);
	CHECK(gate.networkAllowed());
}

TEST_CASE(CurrentStopRemainsEffectiveUntilTheNextReplacement) {
	ServerEnrollmentGate gate;
	CHECK(gate.start());
	CHECK(gate.pause());
	CHECK(!gate.networkAllowed());

	const auto currentStop = gate.stopToken();
	CHECK(gate.stopTokenIsCurrent(currentStop));

	const auto resumed = gate.resume();
	CHECK(resumed.resumed);
	CHECK(!gate.stopTokenIsCurrent(currentStop));

	CHECK(gate.pause());
	CHECK(!gate.networkAllowed());
	const auto replacementStop = gate.stopToken();
	CHECK(gate.stopTokenIsCurrent(replacementStop));
	CHECK(!gate.stopTokenIsCurrent(currentStop));

	auto stopApplied = false;
	if (gate.stopTokenIsCurrent(currentStop)) {
		stopApplied = true;
	}
	CHECK(!stopApplied);
	if (gate.stopTokenIsCurrent(replacementStop)) {
		stopApplied = true;
	}
	CHECK(stopApplied);
}

// A selected endpoint failure can leave a response or timer callback queued
// after the enrollment attempt is cancelled. That callback must not reopen
// the gate or introduce a fallback endpoint in the next attempt.
TEST_CASE(LateSelectedEndpointCallbackCannotReopenTheGate) {
	ServerEnrollmentGate gate;
	CHECK(gate.start());
	const auto failedAttempt = gate.stopToken();
	CHECK(gate.pause());

	auto callbackReopenedGate = false;
	if (gate.stopTokenIsCurrent(failedAttempt)) {
		callbackReopenedGate = true;
		static_cast<void>(gate.resume());
	}

	CHECK(!callbackReopenedGate);
	CHECK(!gate.networkAllowed());
}

TEST_CASE(RejectedEnrollmentDoesNotReachNetworkOrAuth) {
	auto networkCalls = 0;
	auto authCalls = 0;
	const auto committed = CommitServerEnrollment(
		[] { return false; },
		[&] {
			++networkCalls;
			return true;
		},
		[&] { ++authCalls; });

	CHECK(!committed);
	CHECK_EQ(networkCalls, 0);
	CHECK_EQ(authCalls, 0);
}

TEST_CASE(PinPersistencePrecedesTheFirstConnection) {
	auto events = std::vector<int>();
	const auto committed = CommitServerEnrollment(
		[&] {
			events.push_back(1); // set the verified endpoint and key
			return true;
		},
		[&] {
			events.push_back(2); // persist the pin
			return true;
		},
		[&] { events.push_back(3); }); // start network and auth

	CHECK(committed);
	CHECK_EQ(int(events.size()), 3);
	CHECK_EQ(events[0], 1);
	CHECK_EQ(events[1], 2);
	CHECK_EQ(events[2], 3);
}

TEST_CASE(EndpointRefreshNeedsProofAndPersistsBeforeReleasingRequests) {
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	const auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	auto staged = false;
	auto persisted = false;
	auto requestsReleased = false;
	const auto attempt = [&](bool authenticated) {
		return CommitServerEndpointRefresh(
			original, candidate, authenticated,
			[&] {
				staged = true;
				return true;
			},
			[&] {
				CHECK(staged);
				CHECK(!requestsReleased);
				persisted = true;
				return true;
			},
			[&] {
				CHECK(persisted);
				requestsReleased = true;
			},
			[&] { staged = false; });
	};

	CHECK(!attempt(false));
	CHECK(!staged);
	CHECK(!persisted);
	CHECK(!requestsReleased);

	CHECK(attempt(true));
	CHECK(staged);
	CHECK(persisted);
	CHECK(requestsReleased);
}

TEST_CASE(EndpointRefreshRollsBackWhenPersistenceFails) {
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	const auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	auto staged = false;
	auto rolledBack = false;
	auto requestsReleased = false;
	const auto committed = CommitServerEndpointRefresh(
		original, candidate, true,
		[&] {
			staged = true;
			return true;
		},
		[] { return false; }, [&] { requestsReleased = true; },
		[&] {
			staged = false;
			rolledBack = true;
		});

	CHECK(!committed);
	CHECK(rolledBack);
	CHECK(!staged);
	CHECK(!requestsReleased);
}

TEST_CASE(EndpointRefreshRejectsAChangedServerIdentity) {
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.dcId = 3;
	auto changed = false;
	const auto committed = CommitServerEndpointRefresh(
		original, candidate, true,
		[&] {
			changed = true;
			return true;
		},
		[] { return true; }, [] {}, [] {});

	CHECK(!committed);
	CHECK(!changed);
}

TEST_CASE(EndpointRefreshRejectsKeyMismatchBeforeStaging) {
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.key = std::make_shared<details::RSAPublicKey>(
		bytes::make_span(kDifferentEnrollmentServerKey,
						 sizeof(kDifferentEnrollmentServerKey) - 1));
	auto staged = false;
	const auto committed = CommitServerEndpointRefresh(
		original, candidate, true,
		[&] {
			staged = true;
			return true;
		},
		[] { return true; }, [] {}, [] {});

	CHECK(!committed);
	CHECK(!staged);
}

TEST_CASE(EndpointRefreshIdentityAllowsEndpointChangesOnly) {
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	CHECK(SameCustomServerEndpointRefreshIdentity(original, candidate));
	CHECK(!SameCustomServerPin(original, candidate));
	candidate.port += 1;
	CHECK(SameCustomServerEndpointRefreshIdentity(original, candidate));

	candidate.dcId = original.dcId + 1;
	CHECK(!SameCustomServerEndpointRefreshIdentity(original, candidate));
	candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.serverSelection += "/different";
	CHECK(!SameCustomServerEndpointRefreshIdentity(original, candidate));
	candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.discoveryOrigin += "/different";
	CHECK(!SameCustomServerEndpointRefreshIdentity(original, candidate));
	candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.discoveryPolicy = ServerDiscoveryPolicy::LocalDirect;
	CHECK(!SameCustomServerEndpointRefreshIdentity(original, candidate));
	candidate = MakeEndpointRefreshPin("100.112.103.90");
	candidate.key = std::make_shared<details::RSAPublicKey>(
		bytes::make_span(kDifferentEnrollmentServerKey,
						 sizeof(kDifferentEnrollmentServerKey) - 1));
	CHECK(!SameCustomServerEndpointRefreshIdentity(original, candidate));
}

TEST_CASE(EndpointRefreshPreservesAuthorizationForSameServerIdentity) {
	auto options = DcOptions(Environment::Production);
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	const auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	CHECK(options.setCustomServer(original));
	CHECK(options.markAuthorized(original.dcId));
	CHECK(!options.setCustomServer(candidate));

	CHECK(options.refreshCustomServerEndpoint(original, candidate));
	CHECK(SameCustomServerEndpointRefreshIdentity(options.customServer(),
												  candidate));
	CHECK(options.isAuthorized(original.dcId));
	CHECK(!SameCustomServerPin(options.customServer(), original));
}

TEST_CASE(RestoredSessionRefreshesMovedEndpointAfterAuthenticatedProof) {
	auto options = DcOptions(Environment::Production);
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	const auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	CHECK(options.setCustomServer(original));
	CHECK(options.markAuthorized(original.dcId));
	CHECK(CanRefreshServerEndpoint({
		.restoredSession = true,
		.hasPinnedServer = true,
		.pinAuthorized = true,
		.canResume = true,
		.validDiscoveryOrigin = true,
		.hasStoredAuthKey = true,
	}));
	auto requestsPaused = true;
	const auto committed = CommitServerEndpointRefresh(
		original, candidate, true,
		[&] {
			return options.refreshCustomServerEndpoint(original, candidate);
		},
		[] { return true; }, [&] { requestsPaused = false; });

	CHECK(committed);
	CHECK_EQ(options.customServer().ip, std::string("100.112.103.90"));
	CHECK(options.isAuthorized(original.dcId));
	CHECK(SameCustomServerEndpointRefreshIdentity(options.customServer(),
												  original));
	CHECK(!requestsPaused);
}

TEST_CASE(RestoredSessionRefreshDiscoversEndpointAndWaitsForStoredKeyProof) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	if (!directory.isValid()) {
		return;
	}
	const auto discovery = DiscoverMovedEndpoint();
	CHECK(discovery.has_value());
	if (!discovery) {
		return;
	}
	CHECK(discovery->oldEndpointUnreachable);

	const auto basePath = directory.path() + QDir::separator();
	const auto localKey = MakeEnrollmentStorageKey();
	const auto storedAuthKey = MakeEndpointRefreshAuthKey(0x3A);
	const auto differentAuthKey = MakeEndpointRefreshAuthKey(0x4B);
	const auto serializedAuthKey
		= SerializeEndpointRefreshAuthKey(storedAuthKey);
	auto config = std::make_shared<MTP::Config>(MTP::Environment::Production);
	const auto original = MTP::CustomServer{
		.dcId = 2,
		.ip = "127.0.0.2",
		.port = discovery->selection.operationalPort,
		.key = MakeEnrollmentServerKey(),
		.serverSelection
		= discovery->selection.normalizedSelection.toStdString(),
		.discoveryPolicy = discovery->selection.policy,
		.discoveryOrigin = discovery->origin.toStdString(),
	};
	CHECK(config->dcOptions().setCustomServer(original));
	CHECK(config->dcOptions().markAuthorized(original.dcId));
	{
		auto initial = Storage::Account(basePath, localKey, config, true,
										[&] { return serializedAuthKey; });
		CHECK(initial.writeMtpConfig(true));
		CHECK(initial.writeMtpData(true));
	}

	auto restoredConfig = ReadEnrollmentConfig(basePath, localKey);
	CHECK(restoredConfig != nullptr);
	if (!restoredConfig) {
		return;
	}
	const auto restoredPin = restoredConfig->dcOptions().customServer();
	CHECK_EQ(restoredPin.ip, "127.0.0.2");
	CHECK(restoredConfig->dcOptions().isAuthorized(restoredPin.dcId));
	auto restoredConfigShared
		= std::shared_ptr<MTP::Config>(restoredConfig.release());
	auto restoredAuthData = QByteArray();
	auto restored = Storage::Account(
		basePath, localKey, restoredConfigShared, true,
		[&] { return serializedAuthKey; },
		[&](const QByteArray &value) { restoredAuthData = value; });
	restored.readMtpDataForTest();
	CHECK_EQ(restoredAuthData, serializedAuthKey);
	const auto restoredAuthKey
		= RestoreEndpointRefreshAuthKey(restoredAuthData);
	CHECK(restoredAuthKey != nullptr);
	if (!restoredAuthKey) {
		return;
	}
	CHECK(restoredAuthKey->equals(storedAuthKey));

	const auto selection = MTP::CheckServerSelection(
		QString::fromStdString(restoredPin.serverSelection));
	const auto validOrigin
		= selection.valid() && selection.policy == restoredPin.discoveryPolicy
		  && selection.normalizedSelection.toStdString()
				 == restoredPin.serverSelection
		  && restoredPin.discoveryOrigin
				 == (u"local:"_q + selection.normalizedSelection).toStdString();
	const auto eligibility = MTP::ServerEndpointRefreshEligibility{
		.restoredSession
		= Main::details::ShouldRestoreStoredSession(true, true),
		.hasPinnedServer = bool(restoredPin.key),
		.pinAuthorized
		= restoredConfigShared->dcOptions().isAuthorized(restoredPin.dcId),
		.canResume
		= MTP::CanResumeServerEnrollment(restoredConfigShared->dcOptions()),
		.validDiscoveryOrigin = validOrigin,
		.hasStoredAuthKey
		= restoredAuthKey->type() == MTP::AuthKey::Type::ReadFromFile
		  && restoredAuthKey->dcId() == restoredPin.dcId,
	};
	CHECK(MTP::CanRefreshServerEndpoint(eligibility));

	const auto candidate = MTP::BuildCustomServerFromDiscovery(
		discovery->selection, discovery->result, discovery->origin);
	CHECK(candidate.has_value());
	if (!candidate) {
		return;
	}
	CHECK_EQ(candidate->ip, "127.0.0.1");
	CHECK(
		MTP::SameCustomServerEndpointRefreshIdentity(restoredPin, *candidate));

	auto gate = MTP::ServerEnrollmentGate();
	CHECK(gate.start());
	CHECK(gate.pause());
	CHECK(!gate.networkAllowed());
	auto pinChanged = false;
	auto configPersisted = false;
	auto requestsResumed = false;
	const auto verifyAndCommit = [&](const MTP::AuthKeyPtr &responseKey) {
		return MTP::UnitVerifyEndpointAndResumeRequestsForTest(
			restoredAuthKey, responseKey,
			[&](bool authenticatedProof,
				const std::function<void()> &resumeQueuedRequests) {
				return MTP::CommitServerEndpointRefresh(
					restoredPin, *candidate, authenticatedProof,
					[&] {
						pinChanged = restoredConfigShared->dcOptions()
										 .refreshCustomServerEndpoint(
											 restoredPin, *candidate);
						return pinChanged;
					},
					[&] {
						configPersisted = restored.writeMtpConfig(true);
						return configPersisted;
					},
					[&] {
						CHECK(!gate.networkAllowed());
						CHECK(configPersisted);
						requestsResumed = gate.resume().resumed;
						resumeQueuedRequests();
					});
			});
	};
	const auto rejected = verifyAndCommit(differentAuthKey);
	CHECK(rejected.requestWasPausedBeforeProof);
	CHECK(!rejected.proofAccepted);
	CHECK(!rejected.requestResumed);
	CHECK(!rejected.refreshCommitted);
	CHECK(!pinChanged);
	CHECK(!configPersisted);
	CHECK(!requestsResumed);
	CHECK(!gate.networkAllowed());
	CHECK(MTP::SameCustomServerPin(
		restoredConfigShared->dcOptions().customServer(), restoredPin));

	const auto committed = verifyAndCommit(restoredAuthKey);
	CHECK(committed.requestWasPausedBeforeProof);
	CHECK(committed.proofAccepted);
	CHECK(committed.requestResumed);
	CHECK(committed.refreshCommitted);
	CHECK(pinChanged);
	CHECK(configPersisted);
	CHECK(requestsResumed);
	CHECK(gate.networkAllowed());
	CHECK(restoredConfigShared->dcOptions().isAuthorized(restoredPin.dcId));

	const auto persisted = ReadEnrollmentConfig(basePath, localKey);
	CHECK(persisted != nullptr);
	if (persisted) {
		CHECK_EQ(persisted->dcOptions().customServer().ip, "127.0.0.1");
		CHECK(persisted->dcOptions().isAuthorized(restoredPin.dcId));
	}
}

TEST_CASE(EndpointWithoutAuthKeyProofKeepsOriginalPin) {
	auto options = DcOptions(Environment::Production);
	const auto original = MakeEndpointRefreshPin("100.124.236.66");
	const auto candidate = MakeEndpointRefreshPin("100.112.103.90");
	CHECK(options.setCustomServer(original));
	CHECK(options.markAuthorized(original.dcId));
	auto staged = false;
	auto persisted = false;
	const auto committed = CommitServerEndpointRefresh(
		original, candidate, false,
		[&] {
			staged = true;
			return options.refreshCustomServerEndpoint(original, candidate);
		},
		[&] {
			persisted = true;
			return true;
		},
		[] {});

	CHECK(!committed);
	CHECK(!staged);
	CHECK(!persisted);
	CHECK(SameCustomServerPin(options.customServer(), original));
	CHECK(options.isAuthorized(original.dcId));
}

TEST_CASE(EndpointRefreshRequiresRestoredAuthorizedBoundSession) {
	const auto ready = ServerEndpointRefreshEligibility{
		.restoredSession = true,
		.hasPinnedServer = true,
		.pinAuthorized = true,
		.canResume = true,
		.validDiscoveryOrigin = true,
		.hasStoredAuthKey = true,
	};
	CHECK(CanRefreshServerEndpoint(ready));

	auto state = ready;
	state.restoredSession = false;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.hasPinnedServer = false;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.pinAuthorized = false;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.canResume = false;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.cacheBindingMismatch = true;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.validDiscoveryOrigin = false;
	CHECK(!CanRefreshServerEndpoint(state));
	state = ready;
	state.hasStoredAuthKey = false;
	CHECK(!CanRefreshServerEndpoint(state));
}

TEST_CASE(RefusedServerReplacementPersistsAdvertisedDcBeforeResume) {
	const auto originalDc = DcId(2);
	const auto replacementDc = DcId(3);
	auto mainDc = originalDc;
	auto sessionDcs = std::vector<DcId>{originalDc};
	auto browserOnlyRefused = true;
	auto pinPersisted = false;
	auto mainDcPersisted = false;
	auto resumed = false;
	const auto committed = CommitServerEnrollment(
		[&] {
			ReinitializeMainDcAfterServerReplacement(
				{
					.browserOnlyServerRefused = browserOnlyRefused,
					.currentMainDcId = mainDc,
					.replacementDcId = replacementDc,
				},
				[&](DcId dcId) {
					mainDc = dcId;
					sessionDcs = {dcId};
					browserOnlyRefused = false;
				});
			return true;
		},
		[&] {
			CHECK_EQ(mainDc, replacementDc);
			pinPersisted = true;
			mainDcPersisted = true;
			return true;
		},
		[&] {
			CHECK(pinPersisted);
			CHECK(mainDcPersisted);
			CHECK_EQ(mainDc, replacementDc);
			CHECK_EQ(int(sessionDcs.size()), 1);
			CHECK_EQ(sessionDcs.front(), replacementDc);
			CHECK(!browserOnlyRefused);
			resumed = true;
		});

	CHECK(committed);
	CHECK(resumed);
}

TEST_CASE(ServerEnrollmentRestoresMainDcFromPinnedServerAfterCrash) {
	const auto originalDc = DcId(2);
	const auto replacementDc = DcId(3);
	const auto newPinWithOldMain = ServerEnrollmentMainDcId({
		.hasAuthenticatedSession = false,
		.storedMainDcId = originalDc,
		.pinnedServerDcId = replacementDc,
	});
	const auto oldPinWithNewMain = ServerEnrollmentMainDcId({
		.hasAuthenticatedSession = false,
		.storedMainDcId = replacementDc,
		.pinnedServerDcId = originalDc,
	});
	const auto authenticated = ServerEnrollmentMainDcId({
		.hasAuthenticatedSession = true,
		.storedMainDcId = originalDc,
		.pinnedServerDcId = replacementDc,
	});

	CHECK_EQ(newPinWithOldMain, replacementDc);
	CHECK_EQ(oldPinWithNewMain, originalDc);
	CHECK_EQ(authenticated, originalDc);
}

TEST_CASE(FailedRefusedServerReplacementRestoresPreviousDc) {
	const auto originalDc = DcId(2);
	const auto replacementDc = DcId(3);
	auto mainDc = originalDc;
	const auto setPin = [&] {
		ReinitializeMainDcAfterServerReplacement(
			{
				.browserOnlyServerRefused = true,
				.currentMainDcId = mainDc,
				.replacementDcId = replacementDc,
			},
			[&](DcId dcId) { mainDc = dcId; });
		return true;
	};
	const auto persistPin = [] { return false; };
	const auto resume = [] {};
	const auto rollbackPin = [&] { mainDc = originalDc; };
	const auto committed
		= CommitServerEnrollment(setPin, persistPin, resume, rollbackPin);

	CHECK(!committed);
	CHECK_EQ(mainDc, originalDc);
}

TEST_CASE(EnrollmentPersistenceFailureRollsBackBeforeReturning) {
	auto rolledBack = false;
	auto resumed = false;
	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[] { return false; },
		[&] { resumed = true; },
		[&] { rolledBack = true; });

	CHECK(!committed);
	CHECK(!resumed);
	CHECK(rolledBack);
}

TEST_CASE(EnrollmentPersistsReadableMapBeforeConfig) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	const auto basePath = directory.path() + QDir::separator();
	const auto key = MakeEnrollmentStorageKey();
	auto account = MakeEnrollmentStorageAccount(basePath, key);

	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[&] { return account->writeMtpConfig(true); },
		[] {});

	CHECK(committed);
	CHECK(HasReadableEnrollmentMap(basePath, key));
	CHECK(HasReadableEnrollmentConfig(basePath, key));
}

TEST_CASE(EnrollmentRestartRestoresBoundServerStep) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	const auto basePath = directory.path() + QDir::separator();
	const auto key = MakeEnrollmentStorageKey();
	auto account = MakeEnrollmentStorageAccount(basePath, key);
	auto resumed = false;

	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[&] { return account->writeMtpConfig(true); },
		[&] { resumed = true; });

	CHECK(committed);
	CHECK(resumed);
	account.reset();

	const auto restored = ReadEnrollmentConfig(basePath, key);
	CHECK(restored != nullptr);
	if (!restored) {
		return;
	}
	CHECK(restored->hasCustomServer());
	const auto server = restored->customServer();
	CHECK_EQ(server.ip, "10.4.1.7");
	CHECK_EQ(server.port, 8443);
	CHECK(server.key != nullptr);
	CHECK(MTP::ShouldOpenServerEnrollment(
		restored->hasCustomServer(),
		false));
	CHECK(!MTP::ShouldOpenServerEnrollment(false, false));
	CHECK(MTP::ShouldOpenServerEnrollment(false, true));
}

TEST_CASE(ServerReenrollmentConfirmationLeavesBlockedPinUnchanged) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	auto account = std::make_unique<Storage::Account>(
		basePath,
		key,
		std::move(config),
		false,
		[] { return QByteArray("old-server-auth-key"); });
	CHECK(account->writeMtpConfig(true));
	CHECK(account->writeMtpData(true));

	MTP::ServerEnrollmentGate gate;
	CHECK(gate.start());
	CHECK(gate.pause());
	auto wipeCalls = 0;
	const auto wipe = [&] {
		++wipeCalls;
		const auto written = account->writeServerReenrollmentTombstone();
		if (written) {
			static_cast<void>(gate.resume());
		}
		return written;
	};

	// Canceling the identity warning and declining the final prompt must not
	// reach the destructive callback, even though the first prompt was seen.
	CHECK(!Main::details::CommitServerReenrollment(
		Main::details::ServerReenrollmentPrompt::IdentityChange,
		false,
		wipe));
	CHECK(!Main::details::CommitServerReenrollment(
		Main::details::ServerReenrollmentPrompt::IdentityChange,
		true,
		wipe));
	CHECK(!Main::details::CommitServerReenrollment(
		Main::details::ServerReenrollmentPrompt::DestructiveConfirmation,
		false,
		wipe));

	CHECK_EQ(wipeCalls, 0);
	CHECK(!account->serverReenrollmentPending());
	CHECK(!gate.networkAllowed());
	const auto restored = ReadEnrollmentConfig(basePath, key);
	CHECK(restored != nullptr);
	if (restored) {
		CHECK(restored->hasCustomServer());
		CHECK(restored->dcOptions().isAuthorized(2));
	}
}

TEST_CASE(ServerReenrollmentWipeRemovesFutureStores) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto dataName = u"reenrollment"_q;
	const auto tdataPath = directory.path() + u"/tdata/"_q;
	const auto basePath = tdataPath + u"account/"_q;
	const auto tempPath = tdataPath + u"temp_"_q + dataName + u"/"_q;
	const auto databasePath = tdataPath + u"user_"_q + dataName + u"/"_q;
	const auto key = MakeEnrollmentStorageKey();
	const auto serialized = QByteArray("old-server-auth-key");
	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	auto account = std::make_unique<Storage::Account>(
		basePath,
		key,
		std::move(config),
		false,
		[serialized] { return serialized; },
		nullptr,
		nullptr,
		tempPath,
		databasePath);
	CHECK(account->writeMtpConfig(true));
	CHECK(account->writeMtpData(true));

	CHECK(QDir().mkpath(basePath + u"future/nested"_q));
	CHECK(QDir().mkpath(databasePath + u"future"_q));
	CHECK(QDir().mkpath(tempPath + u"future"_q));
	QFile futureStore(basePath + u"future/nested/messages"_q);
	CHECK(futureStore.open(QIODevice::WriteOnly));
	futureStore.write("server-scoped");
	futureStore.close();

	CHECK(account->writeServerReenrollmentTombstone());
	CHECK(account->serverReenrollmentPending());
	CHECK(account->completeServerReenrollment());
	CHECK(!account->serverReenrollmentPending());
	CHECK(ReadEnrollmentConfig(basePath, key) == nullptr);
	CHECK(!QFile::exists(basePath + u"future/nested/messages"_q));
	CHECK(!QDir(databasePath).exists());
	CHECK(!QDir(tempPath).exists());

	auto restored = QByteArray();
	auto restarted = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; },
		nullptr,
		tempPath,
		databasePath);
	restarted->readMtpDataForTest();
	CHECK(restored.isEmpty());
}

TEST_CASE(ServerReenrollmentWipePreservesOtherAccountsLegacyStore) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto tdataPath = directory.path() + u"/tdata/"_q;
	const auto accountAPath = tdataPath + u"account_a/"_q;
	const auto tempAPath = tdataPath + u"temp_account_a/"_q;
	const auto databaseAPath = tdataPath + u"user_account_a/"_q;
	const auto accountBPath = tdataPath + u"account_b/"_q;
	const auto tempBPath = tdataPath + u"temp_account_b/"_q;
	const auto databaseBPath = tdataPath + u"user_account_b/"_q;
	const auto legacyStorePath = tdataPath
		+ u"tdld/account_b/future/messages"_q;
	CHECK(QDir().mkpath(tdataPath + u"tdld/account_b/future"_q));
	QFile legacyStore(legacyStorePath);
	CHECK(legacyStore.open(QIODevice::WriteOnly));
	legacyStore.write("account-b-server-scoped");
	legacyStore.close();

	const auto key = MakeEnrollmentStorageKey();
	auto accountB = MakeEnrollmentStorageAccount(
		accountBPath,
		key,
		nullptr,
		nullptr,
		nullptr,
		tempBPath,
		databaseBPath);
	CHECK(accountB != nullptr);
	CHECK(QDir().mkpath(databaseBPath + u"future"_q));

	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	auto accountA = std::make_unique<Storage::Account>(
		accountAPath,
		key,
		std::move(config),
		false,
		[] { return QByteArray("account-a-server-auth-key"); },
		nullptr,
		nullptr,
		tempAPath,
		databaseAPath);
	CHECK(accountA->writeMtpConfig(true));
	CHECK(accountA->writeMtpData(true));
	CHECK(accountA->writeServerReenrollmentTombstone());
	CHECK(accountA->completeServerReenrollment());

	CHECK(QFile::exists(legacyStorePath));
	QFile preservedLegacyStore(legacyStorePath);
	CHECK(preservedLegacyStore.open(QIODevice::ReadOnly));
	CHECK_EQ(
		preservedLegacyStore.readAll(),
		QByteArray("account-b-server-scoped"));
	CHECK(QDir(databaseBPath).exists());
}

TEST_CASE(ServerReenrollmentStartupEntersEnrollment) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"/account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	{
		auto account = std::make_unique<Storage::Account>(
			basePath,
			key,
			std::move(config),
			false,
			[] { return QByteArray("old-server-auth-key"); });
		CHECK(account->writeMtpConfig(true));
		CHECK(account->writeMtpData(true));
		CHECK(account->writeServerReenrollmentTombstone());
		CHECK(account->serverReenrollmentPending());
	}

	auto restarted = MakeEnrollmentStorageAccount(basePath, nullptr);
	const auto enrollment = restarted->startServerReenrollmentForTest(key);
	CHECK(enrollment != nullptr);
	if (enrollment) {
		CHECK(enrollment->dcOptions().unenrolled());
		CHECK(!enrollment->hasCustomServer());
	}
}

TEST_CASE(ServerReenrollmentMultiAccountStartupRestoresEncryptedAccountList) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto dataName = u"reenrollment"_q;
	const auto tdataPath = directory.path() + u"/tdata/"_q;
	const auto activeDatabasePath = Main::Account::storageDatabasePathForTest(
		dataName,
		0);
	const auto pendingBasePath = Main::Account::storageBasePathForTest(
		dataName,
		1);
	const auto pendingTempPath = Main::Account::storageTempPathForTest(
		dataName,
		1);
	const auto pendingDatabasePath
		= Main::Account::storageDatabasePathForTest(dataName, 1);
	const auto key = MakeEnrollmentStorageKey();

	CHECK(QDir().mkpath(activeDatabasePath + u"future"_q));

	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	auto pending = std::make_unique<Storage::Account>(
		pendingBasePath,
		key,
		std::move(config),
		false,
		[] { return QByteArray("pending-server-auth-key"); },
		nullptr,
		nullptr,
		pendingTempPath,
		pendingDatabasePath,
		Storage::FileKey(2));
	CHECK(pending->writeMtpConfig(true));
	CHECK(pending->writeMtpData(true));
	CHECK(pending->writeServerReenrollmentTombstone());
	CHECK(pending->serverReenrollmentPending());

	const auto indices = std::vector<int>{ 0, 1 };
	CHECK(WriteEnrollmentAccountList(
		dataName,
		tdataPath,
		key,
		indices,
		0));

	auto restarted = Main::Domain(dataName);
	CHECK(restarted.local().start(QByteArray())
		== Storage::StartResult::Success);
	const auto &restored = restarted.accounts();
	CHECK_EQ(int(restored.size()), 2);
	auto restoredIndices = base::flat_set<int>();
	for (const auto &[index, account] : restored) {
		CHECK(account != nullptr);
		restoredIndices.emplace(index);
		if (index == 1) {
			CHECK(account->startedUnenrolledForTest());
			CHECK(!account->local().serverReenrollmentPending());
		}
	}

	CHECK(restoredIndices.contains(0));
	CHECK(restoredIndices.contains(1));
	CHECK_EQ(int(restoredIndices.size()), 2);
	CHECK(QDir(activeDatabasePath).exists());
}

TEST_CASE(ServerReenrollmentTombstoneReplaysAfterInterruption) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	const auto prepare = [&] {
		auto config = MakeEnrollmentConfig();
		CHECK(config->dcOptions().markAuthorized(2));
		auto account = std::make_unique<Storage::Account>(
			basePath,
			key,
			std::move(config),
			false,
			[] { return QByteArray("old-server-auth-key"); });
		CHECK(account->writeMtpConfig(true));
		CHECK(account->writeMtpData(true));
		CHECK(QDir().mkpath(basePath + u"future"_q));
		QFile futureStore(basePath + u"future/messages"_q);
		CHECK(futureStore.open(QIODevice::WriteOnly));
		futureStore.write("server-scoped");
		futureStore.close();
		CHECK(account->writeServerReenrollmentTombstone());
		return account;
	};

	for (const auto interruption : { 1, 2 }) {
		auto account = prepare();
		// The injected stop models a process kill at two different durable
		// boundaries. The next account instance is the next launch.
		account->setServerReenrollmentInterruptionForTest(interruption);
		CHECK(!account->completeServerReenrollment());
		CHECK(account->serverReenrollmentPending());
		account.reset();

		auto restored = QByteArray();
		auto restarted = MakeEnrollmentStorageAccount(
			basePath,
			key,
			[] { return QByteArray(); },
			[&](const QByteArray &value) { restored = value; });
		CHECK(restarted->serverReenrollmentPending());
		CHECK(restarted->completeServerReenrollment());
		CHECK(!restarted->serverReenrollmentPending());
		restarted->readMtpDataForTest();
		CHECK(restored.isEmpty());
	}
}

TEST_CASE(MtpAuthorizationLifecycleWriteSurvivesCleanAccountRestart) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	const auto serialized = QByteArray("persisted-auth-key");
	{
		auto config = MakeEnrollmentConfig();
		CHECK(config->dcOptions().markAuthorized(2));
		auto account = std::make_unique<Storage::Account>(
			basePath,
			key,
			std::move(config),
			false,
			[serialized] { return serialized; });
		// This is the mandatory synchronous seam used by Main::Account's
		// post-auth and clean-teardown paths.
		auto published = false;
		CHECK(Main::details::CommitPostAuthMtpAuthorization(
			account.get(),
			[&] { published = true; },
			[] {}));
		CHECK(published);
	}

	const auto restoredConfig = ReadEnrollmentConfig(basePath, key);
	CHECK(restoredConfig != nullptr);
	if (restoredConfig) {
		CHECK(restoredConfig->dcOptions().isAuthorized(2));
	}

	auto restored = QByteArray();
	{
		auto account = MakeEnrollmentStorageAccount(
			basePath,
			key,
			[] { return QByteArray(); },
			[&](const QByteArray &value) { restored = value; });
		account->readMtpDataForTest();
	}

	CHECK_EQ(restored, serialized);
}

TEST_CASE(PostAuthAuthorizationCommitFailureKeepsSessionClosed) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	auto writes = 0;
	auto account = MakeEnrollmentStorageAccount(
		basePath,
		MakeEnrollmentStorageKey(),
		nullptr,
		nullptr,
		[&] {
			++writes;
			return false;
		});
	auto published = false;
	auto failed = false;
	const auto result = Main::details::CommitPostAuthMtpAuthorization(
		account.get(),
		[&] { published = true; },
		[&] {
			failed = account->writeMtpAuthorizationFailure();
		});

	CHECK(!result);
	CHECK_EQ(writes, 1);
	CHECK(!published);
	CHECK(failed);
	CHECK(account->mtpAuthorizationWriteFailed());
	account.reset();

	auto restarted = MakeEnrollmentStorageAccount(
		basePath,
		MakeEnrollmentStorageKey());
	restarted->readMtpAuthorizationFailureMarkerForTest();
	CHECK(restarted->mtpAuthorizationWriteFailed());
}

TEST_CASE(CleanTeardownAuthorizationCommitFailureKeepsLastState) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	const auto serialized = QByteArray("last-durable-auth-key");
	auto account = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[serialized] { return serialized; },
		nullptr,
		[] { return false; });
	CHECK(account->writeMtpConfig(true));
	CHECK(account->writeMtpData(true));

	auto published = false;
	auto blocked = false;
	const auto result = Main::details::CommitTeardownMtpAuthorization(
		account.get(),
		[&] { published = true; },
		[&] {
			blocked = account->writeMtpAuthorizationFailure();
			published = false;
		});

	CHECK(!result);
	CHECK(!published);
	CHECK(blocked);
	CHECK(account->mtpAuthorizationWriteFailed());
	account.reset();

	auto restored = QByteArray();
	auto restarted = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; });
	restarted->readMtpAuthorizationFailureMarkerForTest();
	CHECK(restarted->mtpAuthorizationWriteFailed());
	restarted->readMtpDataForTest();
	const auto config = ReadEnrollmentConfig(basePath, key);
	CHECK(config != nullptr);
	CHECK(config && config->hasCustomServer());
	CHECK_EQ(restored, serialized);
}

TEST_CASE(ForgetServerDropsSavedIdentityAndAuthorizationButKeepsCache) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto databasePath = directory.path() + u"database/"_q;
	const auto cachePath = databasePath + u"cache"_q;
	const auto cleanupCanaryPath = basePath + u"startup-cleanup-canary"_q;
	CHECK(QDir().mkpath(basePath));
	CHECK(QDir().mkpath(databasePath));
	QFile cache(cachePath);
	CHECK(cache.open(QIODevice::WriteOnly));
	cache.write("cached-messages");
	cache.close();
	QFile cleanupCanary(cleanupCanaryPath);
	CHECK(cleanupCanary.open(QIODevice::WriteOnly));
	cleanupCanary.write("wait-for-startup-cleanup");
	cleanupCanary.close();

	const auto key = MakeEnrollmentStorageKey();
	const auto serialized = QByteArray("old-server-auth-key");
	auto fingerprint = uint64(0);
	{
		auto config = MakeEnrollmentConfig();
		CHECK(config->dcOptions().markAuthorized(2));
		fingerprint = config->dcOptions().customServer().key->fingerprint();
		auto account = std::make_unique<Storage::Account>(
			basePath,
			key,
			std::move(config),
			false,
			[serialized] { return serialized; },
			nullptr,
			nullptr,
			QString(),
			databasePath);
		CHECK(account->writeMtpConfig(true));
		CHECK(account->writeMtpData(true));
		CHECK(account->writeMtpAuthorizationFailure());
		CHECK(account->ensureServerCacheBinding(Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = fingerprint,
			.userIdKnown = true,
			.userId = 4242,
		}));
	}

	auto restored = QByteArray();
	auto restarted = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; },
		nullptr,
		QString(),
		databasePath);
	auto cleanupCompleted = false;
	auto skippedMtpData = true;
	auto loadedConfig = std::unique_ptr<MTP::Config>();
	const auto initialResult = Storage::details::ProcessServerForgetStartup(
		false,
		[&](bool skipMtpData) {
			skippedMtpData = skipMtpData;
			const auto prefsKey = ReadEnrollmentPrefsKey(basePath, key);
			CHECK(prefsKey.has_value());
			if (prefsKey) {
				Storage::AccountTestPeer::ReadPrefs(*restarted, *prefsKey);
			}
			if (!skipMtpData) {
				restarted->readMtpDataForTest();
			}
		},
		[&] {
			cleanupCompleted = QFile::remove(cleanupCanaryPath);
		},
		[] { return true; },
		[&] {
			Storage::AccountTestPeer::ReadStoredCustomServerPin(*restarted);
		},
		[&] { loadedConfig = ReadEnrollmentConfig(basePath, key); });
	CHECK(initialResult
		== Storage::details::ServerForgetStartupResult::Loaded);
	CHECK(cleanupCompleted);
	CHECK(!skippedMtpData);
	CHECK(loadedConfig != nullptr);
	CHECK(loadedConfig && loadedConfig->hasCustomServer());
	CHECK(restarted->hasStoredCustomServer());
	CHECK(restarted->mtpAuthorizationWriteFailed());
	CHECK(!QFileInfo::exists(cleanupCanaryPath));
	CHECK(restarted->checkServerCacheBinding(fingerprint, 4242)
		== Storage::ServerCacheBindingStatus::Match);
	CHECK_EQ(restored, serialized);
	restarted->setServerForgetInterruptionForTest(1);
	CHECK(!restarted->beginServerForget(Storage::ServerCacheBinding{
		.fingerprintKnown = true,
		.fingerprint = fingerprint,
		.userIdKnown = true,
		.userId = 4242,
	}));
	CHECK(restarted->serverForgetPending());
	restarted.reset();

	QFile nextCleanupCanary(cleanupCanaryPath);
	CHECK(nextCleanupCanary.open(QIODevice::WriteOnly));
	nextCleanupCanary.write("wait-for-post-forget-cleanup");
	nextCleanupCanary.close();
	restored.clear();
	auto afterForget = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; },
		nullptr,
		QString(),
		databasePath,
		true,
		true);
	cleanupCompleted = false;
	skippedMtpData = false;
	auto pinRead = false;
	auto configRead = false;
	const auto forgetResult = Storage::details::ProcessServerForgetStartup(
		afterForget->serverForgetPending(),
		[&](bool skipMtpData) {
			skippedMtpData = skipMtpData;
			const auto prefsKey = ReadEnrollmentPrefsKey(basePath, key);
			CHECK(prefsKey.has_value());
			if (prefsKey) {
				Storage::AccountTestPeer::ReadPrefs(*afterForget, *prefsKey);
			}
			if (!skipMtpData) {
				afterForget->readMtpDataForTest();
			}
		},
		[&] {
			cleanupCompleted = QFile::remove(cleanupCanaryPath);
		},
		[&] {
			CHECK(cleanupCompleted);
			return afterForget->completeServerForgetForTest(key);
		},
		[&] {
			pinRead = true;
			Storage::AccountTestPeer::ReadStoredCustomServerPin(*afterForget);
		},
		[&] {
			configRead = true;
			loadedConfig = ReadEnrollmentConfig(basePath, key);
		});
	CHECK(forgetResult
		== Storage::details::ServerForgetStartupResult::Forgotten);
	CHECK(cleanupCompleted);
	CHECK(skippedMtpData);
	CHECK(!pinRead);
	CHECK(!configRead);
	CHECK(!QFileInfo::exists(cleanupCanaryPath));

	CHECK(ReadEnrollmentConfig(basePath, key) == nullptr);
	CHECK(!afterForget->hasStoredCustomServer());
	CHECK(!afterForget->mtpAuthorizationWriteFailed());
	CHECK(restored.isEmpty());
	CHECK(QFile::exists(cachePath));
	const auto authorization = directory.path()
		+ u"/tdata/"_q
		+ Storage::details::ToFilePart(Storage::FileKey(0));
	CHECK(!QFileInfo::exists(authorization + 's'));
	CHECK(!QFileInfo::exists(authorization + '0'));
	CHECK(!QFileInfo::exists(authorization + '1'));
	CHECK(afterForget->checkServerCacheBinding(fingerprint, 4242)
		== Storage::ServerCacheBindingStatus::Match);
	CHECK(afterForget->checkServerCacheBinding(fingerprint, 5252)
		== Storage::ServerCacheBindingStatus::Mismatch);
	CHECK(afterForget->checkServerCacheBinding(fingerprint ^ 1, 4242)
		== Storage::ServerCacheBindingStatus::Mismatch);
	afterForget.reset();

	// A second storage restart must get its unpinned decision from the
	// rewritten map and preferences, even when constructor defaults say pinned.
	restored.clear();
	auto finalRestart = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; },
		nullptr,
		QString(),
		databasePath,
		true,
		true);
	loadedConfig.reset();
	const auto finalResult = Storage::details::ProcessServerForgetStartup(
		finalRestart->serverForgetPending(),
		[&](bool skipMtpData) {
			CHECK(!skipMtpData);
			if (const auto prefsKey = ReadEnrollmentPrefsKey(basePath, key)) {
				Storage::AccountTestPeer::ReadPrefs(*finalRestart, *prefsKey);
			}
			if (!skipMtpData) {
				finalRestart->readMtpDataForTest();
			}
		},
		[] {},
		[] { return true; },
		[&] {
			Storage::AccountTestPeer::ReadStoredCustomServerPin(*finalRestart);
		},
		[&] { loadedConfig = ReadEnrollmentConfig(basePath, key); });
	CHECK(finalResult == Storage::details::ServerForgetStartupResult::Loaded);
	CHECK(!finalRestart->hasStoredCustomServer());
	CHECK(!finalRestart->mtpAuthorizationWriteFailed());
	CHECK(loadedConfig == nullptr);
	CHECK(restored.isEmpty());
	CHECK(finalRestart->checkServerCacheBinding(fingerprint, 4242)
		== Storage::ServerCacheBindingStatus::Match);
}

TEST_CASE(ForgetServerTransitionReplaysAfterInterruption) {
	for (const auto interruption : { 1, 2, 3, 4, 5, 6 }) {
		QTemporaryDir directory;
		CHECK(directory.isValid());

		const auto previousWorkingDir = QDir::currentPath();
		QDir::setCurrent(directory.path());
		const auto restoreWorkingDir = gsl::finally([&] {
			QDir::setCurrent(previousWorkingDir);
		});

		const auto basePath = directory.path() + u"account/"_q;
		const auto databasePath = directory.path() + u"database/"_q;
		const auto cachePath = databasePath + u"cache"_q;
		CHECK(QDir().mkpath(databasePath));
		QFile cache(cachePath);
		CHECK(cache.open(QIODevice::WriteOnly));
		cache.write("cached-messages");
		cache.close();

		const auto key = MakeEnrollmentStorageKey();
		auto config = MakeEnrollmentConfig();
		CHECK(config->dcOptions().markAuthorized(2));
		const auto fingerprint
			= config->dcOptions().customServer().key->fingerprint();
		auto account = std::make_unique<Storage::Account>(
			basePath,
			key,
			std::move(config),
			false,
			[] { return QByteArray("old-server-auth-key"); },
			nullptr,
			nullptr,
			QString(),
			databasePath);
		CHECK(account->writeMtpConfig(true));
		CHECK(account->writeMtpData(true));
		CHECK(account->writeMtpAuthorizationFailure());
		account->setServerForgetInterruptionForTest(interruption);
		CHECK(!account->beginServerForget(Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = fingerprint,
			.userIdKnown = true,
			.userId = 4242,
		}));
		CHECK(account->serverForgetPending());
		account.reset();

		auto restored = QByteArray();
		auto restarted = MakeEnrollmentStorageAccount(
			basePath,
			key,
			[] { return QByteArray(); },
			[&](const QByteArray &value) { restored = value; },
			nullptr,
			QString(),
			databasePath);
		CHECK(restarted->completeServerForgetForTest(key));

		CHECK(!restarted->serverForgetBlocked());
		CHECK(!restarted->serverForgetPending());
		CHECK(!restarted->hasStoredCustomServer());
		CHECK(!restarted->mtpAuthorizationWriteFailed());
		CHECK(restored.isEmpty());
		CHECK(QFile::exists(cachePath));
		CHECK(restarted->checkServerCacheBinding(fingerprint, 4242)
			== Storage::ServerCacheBindingStatus::Match);
	}
}

TEST_CASE(EnrollmentDoesNotResumeWhenProductionMapStorageFails) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	const auto basePath = directory.path() + QDir::separator();
	CHECK(QDir().mkpath(basePath + u"maps"_q));
	const auto key = MakeEnrollmentStorageKey();
	auto account = MakeEnrollmentStorageAccount(basePath, key);
	auto resumed = false;

	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[&] { return account->writeMtpConfig(true); },
		[&] { resumed = true; });

	CHECK(!committed);
	CHECK(!resumed);
	CHECK(QDir(basePath + u"maps"_q).exists());
	CHECK(!QFile::exists(basePath + u"configs"_q));
}

TEST_CASE(EnrollmentDoesNotResumeWhenProductionConfigStorageFails) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	const auto basePath = directory.path() + QDir::separator();
	CHECK(QDir().mkpath(basePath + u"configs"_q));
	const auto key = MakeEnrollmentStorageKey();
	auto account = MakeEnrollmentStorageAccount(basePath, key);
	auto resumed = false;

	const auto committed = CommitServerEnrollment(
		[] { return true; },
		[&] { return account->writeMtpConfig(true); },
		[&] { resumed = true; });

	CHECK(!committed);
	CHECK(!resumed);
	CHECK(QDir(basePath + u"configs"_q).exists());
	CHECK(HasReadableEnrollmentMap(basePath, key));
}

TEST_CASE(EnrollmentDoesNotResumeWhileProductionPersistenceStalls) {
	QTemporaryDir directory;
	CHECK(directory.isValid());
	const auto basePath = directory.path() + QDir::separator();
	const auto key = MakeEnrollmentStorageKey();
	QSemaphore entered;
	QSemaphore release;
	auto resumed = std::atomic_bool(false);
	auto committed = std::atomic_bool(false);

	std::thread enrollment([&] {
		const auto result = CommitServerEnrollment(
			[] { return true; },
			[&] {
				entered.release();
				release.acquire();
				auto account = MakeEnrollmentStorageAccount(basePath, key);
				return account->writeMtpConfig(true);
			},
			[&] { resumed.store(true); });
		committed.store(result);
	});

	CHECK(entered.tryAcquire(1, 1000));
	CHECK(!resumed.load());
	CHECK(!committed.load());

	release.release();
	enrollment.join();
	CHECK(committed.load());
	CHECK(resumed.load());
}

TEST_CASE(EnrollmentStepConsumesSpaceOutsideConfirm) {
	// Exercise the same QKeyEvent gate used by ServerKeyWidget::keyPressEvent.
	auto enter = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Enter,
		Qt::NoModifier);
	CHECK(ConsumeServerEnrollmentActivationKey(enter));
	CHECK(enter.isAccepted());

	auto returnKey = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Return,
		Qt::NoModifier);
	CHECK(ConsumeServerEnrollmentActivationKey(returnKey));
	CHECK(returnKey.isAccepted());

	auto space = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Space,
		Qt::NoModifier);
	CHECK(ConsumeServerEnrollmentActivationKey(space));
	CHECK(space.isAccepted());

	auto tab = QKeyEvent(
		QEvent::KeyPress,
		Qt::Key_Tab,
		Qt::NoModifier);
	tab.ignore();
	CHECK(!ConsumeServerEnrollmentActivationKey(tab));
	CHECK(!tab.isAccepted());
}

TEST_CASE(RestoredSessionWithoutReadablePinMustBeBlocked) {
	CHECK(Main::details::ShouldBlockRestoredSessionWithoutServerPin(
		true,
		false));
	CHECK(!Main::details::ShouldBlockRestoredSessionWithoutServerPin(
		true,
		true));
	CHECK(!Main::details::ShouldBlockRestoredSessionWithoutServerPin(
		false,
		false));
}

TEST_CASE(SessionRestorationRequiresStoredSelfUser) {
	CHECK(!Main::details::ShouldRestoreStoredSession(true, false));
	CHECK(Main::details::ShouldRestoreStoredSession(true, true));
	CHECK(!Main::details::ShouldRestoreStoredSession(false, true));
}

TEST_CASE(ForgetServerStorageFailureDoesNotRestartAndStaysBlocked) {
	QTemporaryDir directory;
	CHECK(directory.isValid());

	const auto previousWorkingDir = QDir::currentPath();
	QDir::setCurrent(directory.path());
	const auto restoreWorkingDir = gsl::finally([&] {
		QDir::setCurrent(previousWorkingDir);
	});

	const auto basePath = directory.path() + u"account/"_q;
	const auto key = MakeEnrollmentStorageKey();
	auto config = MakeEnrollmentConfig();
	CHECK(config->dcOptions().markAuthorized(2));
	auto account = std::make_unique<Storage::Account>(
		basePath,
		key,
		config,
		false,
		[] { return QByteArray("old-server-auth-key"); });
	CHECK(account->writeMtpConfig(true));
	CHECK(account->writeMtpData(true));
	CHECK(account->writeMtpAuthorizationFailure());
	config->dcOptions().constructBlocked();

	const auto tombstone = directory.path()
		+ u"/tdata/server_forget_"_q
		+ Storage::details::ToFilePart(Storage::FileKey(0));
	CHECK(QDir().mkpath(tombstone + u"s"_q));
	CHECK(QDir().mkpath(tombstone + u"0"_q));

	auto restarts = 0;
	CHECK(!Main::details::CommitServerForget(
		account.get(),
		Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = MakeEnrollmentServerKey()->fingerprint(),
			.userIdKnown = true,
			.userId = 4242,
		},
		[&] { ++restarts; }));
	CHECK_EQ(restarts, 0);
	CHECK(config->dcOptions().blocked());

	auto restored = QByteArray();
	auto restarted = MakeEnrollmentStorageAccount(
		basePath,
		key,
		[] { return QByteArray(); },
		[&](const QByteArray &value) { restored = value; });
	CHECK(!restarted->completeServerForgetForTest(key));
	CHECK(restarted->serverForgetBlocked());
	CHECK(restarted->serverForgetPending());
	CHECK(restored.isEmpty());
}

TEST_CASE(QueuedStopRunsOnlyForTheCurrentEnrollment) {
	// Session::stopUntilPinChange() uses this production queue to cross into
	// the session thread after taking the account-side generation token.
	QThread sessionThread;
	sessionThread.start();
	auto target = std::make_unique<QObject>();
	target->moveToThread(&sessionThread);

	auto currentToken = uint64(7);
	auto stopped = std::atomic_int(0);
	QSemaphore callbackDone;

	QueueServerEnrollmentStop(
		target.get(),
		uint64(6),
		[&](uint64 token) { return token == currentToken; },
		[&] {
			stopped.fetch_add(1);
			callbackDone.release();
		});
	CHECK(!callbackDone.tryAcquire(1, 50));
	CHECK_EQ(stopped.load(), 0);

	QueueServerEnrollmentStop(
		target.get(),
		currentToken,
		[&](uint64 token) { return token == currentToken; },
		[&] {
			stopped.fetch_add(1);
			callbackDone.release();
		});
	CHECK(callbackDone.tryAcquire(1, 1000));
	CHECK_EQ(stopped.load(), 1);

	const auto mainThread = QCoreApplication::instance()->thread();
	QMetaObject::invokeMethod(
		target.get(),
		[targetPtr = target.get(), mainThread] {
			targetPtr->moveToThread(mainThread);
		},
		Qt::BlockingQueuedConnection);
	sessionThread.quit();
	sessionThread.wait();
}

TEST_CASE(TelegramStickerBotTokenUsesEncryptedAccountPreferences) {
	const auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	if (!directory.isValid()) {
		return;
	}
	const auto basePath = directory.path() + u"/account"_q;
	CHECK(QDir().mkpath(basePath));
	const auto key = MakeEnrollmentStorageKey();
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	const auto tokenValue = Core::TelegramBotToken::Create(token);
	CHECK(tokenValue.has_value());
	if (!tokenValue) {
		return;
	}
	const auto expectedUrl = tokenValue->apiUrl(u"getStickerSet"_q);
	auto prefsKey = Storage::FileKey(0);
	{
		auto account = MakeEnrollmentStorageAccount(basePath, key);
		CHECK(account->writeTelegramStickerBotToken(*tokenValue));
		const auto saved = account->telegramStickerBotToken();
		CHECK(saved && saved->apiUrl(u"getStickerSet"_q) == expectedUrl);
		prefsKey = Storage::AccountTestPeer::PrefsKey(*account);
	}
	CHECK(prefsKey != 0);
	{
		auto restarted = MakeEnrollmentStorageAccount(basePath, key);
		Storage::AccountTestPeer::ReadPrefs(*restarted, prefsKey);
		const auto saved = restarted->telegramStickerBotToken();
		CHECK(saved && saved->apiUrl(u"getStickerSet"_q) == expectedUrl);
		CHECK(Storage::AccountTestPeer::HasTelegramStickerBotTokenPreference(
			*restarted));
		CHECK(restarted->clearTelegramStickerBotToken());
		CHECK(!restarted->telegramStickerBotToken().has_value());
		CHECK(!Storage::AccountTestPeer::HasTelegramStickerBotTokenPreference(
			*restarted));
	}
	{
		auto reopened = MakeEnrollmentStorageAccount(basePath, key);
		Storage::AccountTestPeer::ReadPrefs(*reopened, prefsKey);
		CHECK(!reopened->telegramStickerBotToken().has_value());
		CHECK(!Storage::AccountTestPeer::HasTelegramStickerBotTokenPreference(
			*reopened));
	}
	auto iterator
		= QDirIterator(basePath, QDir::Files | QDir::Hidden | QDir::System,
					   QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		auto file = QFile(iterator.next());
		if (file.open(QIODevice::ReadOnly)) {
			CHECK(!file.readAll().contains(token.toUtf8()));
		}
	}
}

TEST_CASE(TelegramStickerBotTokenRetriesFailedPreferenceWrites) {
	const auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	if (!directory.isValid()) {
		return;
	}
	const auto basePath = directory.filePath(u"account"_q);
	const auto movedPath = directory.filePath(u"saved-account"_q);
	CHECK(QDir().mkpath(basePath));
	const auto key = MakeEnrollmentStorageKey();
	const auto oldToken = Core::TelegramBotToken::Create(
		u"12345:AbCdEf0123456789012345678901234567890"_q);
	const auto newToken = Core::TelegramBotToken::Create(
		u"45678:AbCdEf0123456789012345678901234567890"_q);
	CHECK(oldToken.has_value());
	CHECK(newToken.has_value());
	if (!oldToken || !newToken) {
		return;
	}
	auto account = MakeEnrollmentStorageAccount(basePath, key);
	CHECK(account->writeTelegramStickerBotToken(*oldToken));
	CHECK(QDir().rename(basePath, movedPath));
	auto blocker = QFile(basePath);
	CHECK(blocker.open(QIODevice::WriteOnly));
	blocker.close();
	CHECK(!account->writeTelegramStickerBotToken(*newToken));
	const auto restored = account->telegramStickerBotToken();
	CHECK(restored
		  && restored->apiUrl(u"getStickerSet"_q)
				 == oldToken->apiUrl(u"getStickerSet"_q));
	CHECK(!account->clearTelegramStickerBotToken());
	CHECK(!account->telegramStickerBotToken().has_value());
	CHECK(Storage::AccountTestPeer::TokenClearPending(*account));
	CHECK(blocker.remove());
	CHECK(QDir().rename(movedPath, basePath));
	CHECK(account->clearTelegramStickerBotToken());
	CHECK(!Storage::AccountTestPeer::TokenClearPending(*account));
	CHECK(!account->telegramStickerBotToken().has_value());

	const auto freshPath = directory.filePath(u"fresh"_q);
	auto freshBlocker = QFile(freshPath);
	CHECK(freshBlocker.open(QIODevice::WriteOnly));
	freshBlocker.close();
	auto fresh = MakeEnrollmentStorageAccount(freshPath, key);
	CHECK(!fresh->writeTelegramStickerBotToken(*newToken));
	CHECK(!fresh->telegramStickerBotToken().has_value());
	CHECK(Storage::AccountTestPeer::TokenClearPending(*fresh));
	CHECK(freshBlocker.remove());
	CHECK(QDir().mkpath(freshPath));
	CHECK(fresh->clearTelegramStickerBotToken());
	CHECK(!Storage::AccountTestPeer::TokenClearPending(*fresh));
}

} // namespace
