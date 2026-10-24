/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "mtproto/connection_abstract.h"
#include "mtproto/details/mtproto_dcenter.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/session.h"
#include "mtproto/session_private.h"
#include "mtproto/mtproto_auth_key.h"
#include "base/unixtime.h"

#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

// session_private.cpp is linked as shipped. The application-only code it
// calls into (Instance, Session behind SessionData, the real transports
// behind AbstractConnection) is replaced below by the smallest stand-ins
// the reconnect path needs.

namespace MTP {

bool Instance::isTestMode() const { return false; }

QString Instance::deviceModel() const { return QString(); }

QString Instance::systemVersion() const { return QString(); }

bool Instance::isServerEnrollmentNetworkAllowed() const { return true; }

uint64 Instance::serverEnrollmentStopToken() const { return 0; }

bool Instance::hasCallback(mtpRequestId requestId) const { return true; }

bool Instance::isKeysDestroyer() const { return false; }

void Instance::keyWasPossiblyDestroyed(ShiftedDcId shiftedDcId) {}

void Instance::requestConfig() {}

void Instance::requestConfigIfOld() {}

void Instance::requestCDNConfig() {}

void Instance::badConfigurationError() {}

void Instance::syncHttpUnixtime() {}

void Instance::restartedByTimeout(ShiftedDcId) {}

void Instance::keyDestroyedOnServer(ShiftedDcId, uint64) {}

} // namespace MTP

namespace MTP::details {
namespace {

bool UnitConnectionInited = false;
Fn<void()> UnitNeedToResumeAndSend;

} // namespace

mtpBuffer AbstractConnection::prepareSecurePacket(uint64 keyId,
												  MTPint128 msgKey,
												  uint32 size) const {
	auto result = mtpBuffer();
	constexpr auto kTcpPrefixInts = 2;
	constexpr auto kAuthKeyIdPosition = kTcpPrefixInts;
	constexpr auto kAuthKeyIdInts = 2;
	constexpr auto kMessageKeyPosition = kAuthKeyIdPosition + kAuthKeyIdInts;
	constexpr auto kMessageKeyInts = 4;
	constexpr auto kPrefixInts
		= kTcpPrefixInts + kAuthKeyIdInts + kMessageKeyInts;
	constexpr auto kTcpPostfixInts = 4;
	result.reserve(kPrefixInts + size + kTcpPostfixInts);
	result.resize(kPrefixInts);
	*reinterpret_cast<uint64 *>(&result[kAuthKeyIdPosition]) = keyId;
	*reinterpret_cast<MTPint128 *>(&result[kMessageKeyPosition]) = msgKey;
	return result;
}

gsl::span<const mtpPrime>
AbstractConnection::parseNotSecureResponse(const mtpBuffer &buffer) const {
	Unexpected("Key creation is not exercised by these tests.");
}

uint32 AbstractConnection::extendedNotSecurePadding() const {
	Unexpected("Key creation is not exercised by these tests.");
}

QString AbstractConnection::ProtocolDcDebugId(int16 protocolDcId) {
	return QString::number(protocolDcId);
}

void AbstractConnection::logInfo(const QString &message) {}

void SessionData::notifyConnectionInited(const SessionOptions &options) {}

void SessionData::queueTryToReceive() {}

void SessionData::queueNeedToResumeAndSend() {
	if (UnitNeedToResumeAndSend) {
		UnitNeedToResumeAndSend();
	}
}

void SessionData::queueConnectionStateChange(int newState) {}

void SessionData::queuePinnedServerFailure(PinnedServerFailure failure,
										   uint64 presentedFingerprint,
										   const QString &pinnedHostname,
										   const QString &dialledAddress) {}

void SessionData::queueResetDone() {}

void SessionData::queueSendAnything(crl::time msCanWait) {}

bool SessionData::connectionInited() const { return UnitConnectionInited; }

AuthKeyPtr SessionData::getPersistentKey() const { return nullptr; }

AuthKeyPtr SessionData::getTemporaryKey(TemporaryKeyType type) const {
	return nullptr;
}

CreatingKeyType SessionData::acquireKeyCreation(DcType type) {
	return CreatingKeyType::None;
}

CreatingKeyType SessionData::acquirePersistentKeyCreation() {
	return CreatingKeyType::None;
}

bool SessionData::releaseKeyCreationOnDone(
	const AuthKeyPtr &temporaryKey,
	const AuthKeyPtr &persistentKeyUsedForBind) {
	return false;
}

bool SessionData::releasePersistentKeyCreationOnDone(
	const AuthKeyPtr &persistentKey) {
	return false;
}

bool SessionData::releaseCdnKeyCreationOnDone(const AuthKeyPtr &temporaryKey) {
	return false;
}

void SessionData::releaseKeyCreationOnFail() {}

void SessionData::releasePersistentKeyCreationOnFail() {}

void SessionData::destroyPersistentKey(uint64 keyId) {}

void SessionData::destroyTemporaryKey(uint64 keyId) {}

class SessionPrivateTestAccess final {
  public:
	static std::unique_ptr<SessionPrivate>
	Create(std::shared_ptr<SessionData> data) {
		return std::unique_ptr<SessionPrivate>(new SessionPrivate(
			SessionPrivate::TestTag(),
			not_null<Instance *>(reinterpret_cast<Instance *>(quintptr(1))),
			not_null<QThread *>(QThread::currentThread()), std::move(data),
			ShiftedDcId(2)));
	}

	static void StartConnected(SessionPrivate &session,
							   ConnectionPointer connection, AuthKeyPtr key,
							   uint64 sessionId, uint64 salt) {
		session._connection = std::move(connection);
		session._encryptionKey = std::move(key);
		session._keyId = session._encryptionKey->keyId();
		session._sessionId = sessionId;
		session._sessionSalt = salt;
		session._state = ConnectedState;
	}

	static void IdleDisconnect(SessionPrivate &session) {
		const auto connection = session._connection.get();
		connection->disconnectFromServer();
		session.onDisconnected(connection);
	}

	static void Reconnect(SessionPrivate &session,
						  ConnectionPointer connection) {
		session._retryTimer.cancel();
		session._connection = std::move(connection);
		session.setState(ConnectingState);
	}

	static void Receive(SessionPrivate &session, mtpBuffer packet) {
		session._connection->received().push_back(std::move(packet));
		session.handleReceived();
	}

	static mtpMsgId SentMessageId(SessionPrivate &session,
								  mtpRequestId requestId) {
		QReadLocker locker(session._sessionData->haveSentMutex());
		for (const auto &[msgId, request] :
			 session._sessionData->haveSentMap()) {
			if (request->requestId == requestId) {
				return msgId;
			}
		}
		return 0;
	}

	static uint64 SessionSalt(SessionPrivate &session) {
		return session._sessionSalt;
	}

	static size_t ReceivedCount(SessionData &data) {
		QReadLocker locker(data.haveReceivedMutex());
		return data.haveReceivedMessages().size();
	}
};

} // namespace MTP::details

namespace {

using namespace MTP;
using namespace MTP::details;

struct TransportLog final {
	std::vector<mtpBuffer> sent;
};

class FakeTransport final : public AbstractConnection {
  public:
	FakeTransport(QThread *thread, const ProxyData &proxy,
				  std::shared_ptr<TransportLog> log)
		: AbstractConnection(thread, proxy), _log(std::move(log)) {}

	[[nodiscard]] ConnectionPointer clone(const ProxyData &proxy) override {
		return ConnectionPointer::New<FakeTransport>(thread(), proxy, _log);
	}
	[[nodiscard]] crl::time pingTime() const override { return 0; }
	[[nodiscard]] crl::time fullConnectTimeout() const override { return 0; }
	void sendData(mtpBuffer &&buffer) override {
		_log->sent.push_back(std::move(buffer));
	}
	void disconnectFromServer() override { _connected = false; }
	void connectToServer(const QString &, int, const bytes::vector &, int16,
						 bool) override {
		_connected = true;
	}
	[[nodiscard]] bool isConnected() const override { return _connected; }
	[[nodiscard]] int32 debugState() const override {
		return _connected ? 1 : 0;
	}
	[[nodiscard]] QString transport() const override { return u"fake"_q; }
	[[nodiscard]] QString tag() const override { return u"fake"_q; }

  private:
	std::shared_ptr<TransportLog> _log;
	bool _connected = true;
};

[[nodiscard]] AuthKeyPtr MakeTestAuthKey(AuthKey::Type type) {
	auto data = AuthKey::Data();
	for (auto i = 0; i != int(data.size()); ++i) {
		data[i] = gsl::byte(i + 1);
	}
	return std::make_shared<AuthKey>(type, DcId(2), data);
}

[[nodiscard]] SerializedRequest
MakeUsernameCheckRequest(mtpRequestId requestId) {
	auto request = SerializedRequest::Serialize(MTPauth_SendCode(
		MTP_string(u"steward"_q), MTP_int(1), MTP_string(u"test-api-hash"_q),
		MTP_codeSettings(MTP_flags(0), MTPVector<MTPbytes>(), MTPstring(),
						 MTPBool())));
	request->requestId = requestId;
	return request;
}

void AppendLong(mtpBuffer &buffer, uint64 value) {
	const auto index = buffer.size();
	buffer.resize(index + 2);
	memcpy(buffer.data() + index, &value, sizeof(value));
}

[[nodiscard]] mtpMsgId ServerMessageId() {
	static auto next = uint32(1);
	const auto low = next;
	next += 4;
	return (uint64(base::unixtime::now()) << 32) | low;
}

[[nodiscard]] mtpBuffer MakeAuthenticatedServerPacket(const AuthKeyPtr &key,
													  uint64 salt,
													  uint64 sessionId,
													  mtpMsgId messageId,
													  const mtpBuffer &body) {
	auto decrypted = mtpBuffer(8 + body.size() + 3);
	memcpy(decrypted.data(), &salt, sizeof(salt));
	memcpy(decrypted.data() + 2, &sessionId, sizeof(sessionId));
	memcpy(decrypted.data() + 4, &messageId, sizeof(messageId));
	decrypted[6] = 0;
	decrypted[7] = body.size() * sizeof(mtpPrime);
	memcpy(decrypted.data() + 8, body.constData(),
		   body.size() * sizeof(mtpPrime));
	while (decrypted.size() % 4) {
		decrypted.push_back(0);
	}

	auto digest = std::array<uchar, 32>();
	SHA256_CTX context;
	SHA256_Init(&context);
	SHA256_Update(&context, key->partForMsgKey(false), 32);
	SHA256_Update(&context, decrypted.constData(),
				  decrypted.size() * sizeof(mtpPrime));
	SHA256_Final(digest.data(), &context);

	auto messageKey = MTPint128();
	memcpy(&messageKey, digest.data() + 8, sizeof(messageKey));
	auto aesKey = MTPint256();
	auto aesIv = MTPint256();
	key->prepareAES(messageKey, aesKey, aesIv, false);

	auto packet = mtpBuffer(6 + decrypted.size());
	const auto keyId = key->keyId();
	memcpy(packet.data(), &keyId, sizeof(keyId));
	memcpy(packet.data() + 2, &messageKey, sizeof(messageKey));
	aesIgeEncryptRaw(decrypted.constData(), packet.data() + 6,
					 decrypted.size() * sizeof(mtpPrime), &aesKey, &aesIv);
	return packet;
}

[[nodiscard]] mtpBuffer MakeUntrackedPong() {
	auto body = mtpBuffer();
	MTPPong(MTP_pong(MTP_long(mtpMsgId(1)), MTP_long(mtpPingId(2))))
		.write(body);
	return body;
}

[[nodiscard]] mtpBuffer MakeMalformedPong() { return {mtpPrime(0x347773c5)}; }

[[nodiscard]] mtpBuffer MakeUsernameCheckReply(mtpMsgId requestMsgId) {
	auto body = mtpBuffer{mtpPrime(0xf35c6d01)};
	AppendLong(body, requestMsgId);
	body += mtpBuffer{
		mtpPrime(0x5e002502), mtpPrime(0),			mtpPrime(0xc000bba2),
		mtpPrime(5),		  mtpPrime(0x73616804), mtpPrime(0x00000068),
	};
	return body;
}

void RunReconnectScenario(AuthKey::Type keyType) {
	constexpr auto kSessionId = uint64(0x1122334455667788ULL);
	constexpr auto kSessionSalt = uint64(42);
	constexpr auto kRequestId = mtpRequestId(17);
	auto log = std::make_shared<TransportLog>();
	auto session = std::unique_ptr<SessionPrivate>();
	UnitConnectionInited = true;
	UnitNeedToResumeAndSend = [&] {
		if (session) {
			session->tryToSend();
		}
	};
	const auto resetHooks = gsl::finally([] {
		UnitConnectionInited = false;
		UnitNeedToResumeAndSend = nullptr;
	});
	auto data = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	session = SessionPrivateTestAccess::Create(data);
	const auto key = MakeTestAuthKey(keyType);
	auto oldConnection = ConnectionPointer::New<FakeTransport>(
		QThread::currentThread(), ProxyData(), log);
	SessionPrivateTestAccess::StartConnected(*session, std::move(oldConnection),
											 key, kSessionId, kSessionSalt);

	const auto priorReplyId = ServerMessageId();
	auto priorReply = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, priorReplyId, MakeUntrackedPong());
	SessionPrivateTestAccess::Receive(*session, priorReply);

	SessionPrivateTestAccess::IdleDisconnect(*session);
	CHECK(session->getState() < 0);
	CHECK(log->sent.empty());

	{
		QWriteLocker locker(data->toSendMutex());
		data->toSendMap().emplace(kRequestId,
								  MakeUsernameCheckRequest(kRequestId));
	}
	auto reconnectConnection = ConnectionPointer::New<FakeTransport>(
		QThread::currentThread(), ProxyData(), log);
	SessionPrivateTestAccess::Reconnect(*session,
										std::move(reconnectConnection));

	auto invalidAuthentication = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, ServerMessageId(), MakeUntrackedPong());
	invalidAuthentication[6] ^= 1;
	SessionPrivateTestAccess::Receive(*session, invalidAuthentication);
	CHECK(session->getState() < 0);
	CHECK(log->sent.empty());
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(0));
	CHECK(SessionPrivateTestAccess::SessionSalt(*session) == kSessionSalt);

	SessionPrivateTestAccess::Reconnect(
		*session, ConnectionPointer::New<FakeTransport>(
					  QThread::currentThread(), ProxyData(), log));

	const auto malformedReply = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, ServerMessageId(), MakeMalformedPong());
	SessionPrivateTestAccess::Receive(*session, malformedReply);
	CHECK(session->getState() < 0);
	CHECK(log->sent.empty());
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(0));
	CHECK(SessionPrivateTestAccess::SessionSalt(*session) == kSessionSalt);

	SessionPrivateTestAccess::Reconnect(
		*session, ConnectionPointer::New<FakeTransport>(
					  QThread::currentThread(), ProxyData(), log));

	SessionPrivateTestAccess::Receive(*session, std::move(priorReply));
	CHECK_EQ(session->getState(), ConnectingState);
	CHECK(log->sent.empty());
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(0));

	const auto reconnectReply = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, ServerMessageId(), MakeUntrackedPong());
	SessionPrivateTestAccess::Receive(*session, reconnectReply);
	CHECK_EQ(session->getState(), ConnectedState);
	CHECK(!log->sent.empty());
	CHECK(SessionPrivateTestAccess::SessionSalt(*session) == kSessionSalt);
	CHECK(SessionPrivateTestAccess::SentMessageId(*session, kRequestId) != 0);

	const auto requestMsgId
		= SessionPrivateTestAccess::SentMessageId(*session, kRequestId);
	const auto usernameReply = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, ServerMessageId(),
		MakeUsernameCheckReply(requestMsgId));
	SessionPrivateTestAccess::Receive(*session, usernameReply);

	QReadLocker receivedLock(data->haveReceivedMutex());
	const auto &received = data->haveReceivedMessages();
	CHECK(received.size() == 1);
	if (received.size() == 1) {
		CHECK(received.front().requestId == kRequestId);
		auto reply = MTPauth_SentCode();
		auto from = received.front().reply.constData();
		const auto end = from + received.front().reply.size();
		const auto parsed = reply.read(from, end);
		CHECK(parsed);
		if (parsed) {
			CHECK(from == end);
			reply.match(
				[](const MTPDauth_sentCode &result) {
					CHECK(result.vphone_code_hash().v == u"hash"_q);
				},
				[](const MTPDauth_sentCodeSuccess &) { CHECK(false); },
				[](const MTPDauth_sentCodePaymentRequired &) { CHECK(false); });
		}
	}
}

} // namespace

TEST_CASE(IdleReconnectResumesUsernameCheckWithStoredKey) {
	RunReconnectScenario(AuthKey::Type::ReadFromFile);
}

TEST_CASE(IdleReconnectResumesUsernameCheckWithFreshKey) {
	RunReconnectScenario(AuthKey::Type::Generated);
}
