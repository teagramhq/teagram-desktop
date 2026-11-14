/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "mtproto/connection_abstract.h"
#include "mtproto/connection_http.h"
#include "mtproto/connection_server_resolving.h"
#include "mtproto/details/mtproto_dcenter.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/session.h"
#include "mtproto/session_private.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_dc_options.h"
#include "base/unixtime.h"
#include "tests/unit/server_endpoint_refresh_test_support.h"

#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace {

MTP::DcOptions UnitDcOptions(MTP::Environment::Production);

class HttpStatusReply final : public QNetworkReply {
  public:
	explicit HttpStatusReply(int status) {
		setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
	}

	void abort() override {}

  protected:
	qint64 readData(char *, qint64) override { return -1; }
};

} // namespace

namespace MTP::details {

extern DcOptions *UnitProxyCheckOptions;

} // namespace MTP::details

// session_private.cpp is linked as shipped. The application-only code it
// calls into (Instance, Session behind SessionData, the real transports
// behind AbstractConnection) is replaced below by the smallest stand-ins
// the reconnect path needs.

namespace MTP {

namespace {

const Instance *UnitBlockedServerEnrollmentInstance = nullptr;
std::vector<ShiftedDcId> UnitServerEndpointRefreshRequests;

} // namespace

class Instance::Private final {};

Instance::Instance(Mode, Fields &&) : QObject(), _private(nullptr) {}

Instance::~Instance() = default;

bool Instance::isTestMode() const { return false; }

QString Instance::deviceModel() const { return QString(); }

QString Instance::systemVersion() const { return QString(); }

bool Instance::isServerEnrollmentNetworkAllowed() const {
	return this != UnitBlockedServerEnrollmentInstance;
}

uint64 Instance::serverEnrollmentStopToken() const { return 0; }

bool Instance::hasCallback(mtpRequestId requestId) const { return true; }

bool Instance::isKeysDestroyer() const { return false; }

bool Instance::isEndpointVerifier() const { return false; }

void Instance::keyWasPossiblyDestroyed(ShiftedDcId shiftedDcId) {}

void Instance::requestConfig() {}

void Instance::requestConfigIfOld() {}

void Instance::requestCDNConfig() {}

void Instance::badConfigurationError() {}

void Instance::syncHttpUnixtime() {}

void Instance::restartedByTimeout(ShiftedDcId) {}

void Instance::requestServerEndpointRefresh(ShiftedDcId shiftedDcId) {
	UnitServerEndpointRefreshRequests.push_back(shiftedDcId);
}

void Instance::keyDestroyedOnServer(ShiftedDcId, uint64) {}

void Instance::onBrowserOnlyServerRefused() {}

} // namespace MTP

namespace MTP::details {
namespace {

bool UnitConnectionInited = false;
int UnitBrowserOnlyServerRefusals = 0;
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

mtpBuffer AbstractConnection::preparePQFake(const MTPint128 &nonce) const {
	Unexpected("HTTP connection setup is not exercised by these tests.");
}

std::optional<MTPResPQ>
AbstractConnection::readPQFakeReply(const mtpBuffer &buffer) const {
	Unexpected("HTTP connection setup is not exercised by these tests.");
}

uint32 AbstractConnection::extendedNotSecurePadding() const {
	Unexpected("Key creation is not exercised by these tests.");
}

QString AbstractConnection::ProtocolDcDebugId(int16 protocolDcId) {
	return QString::number(protocolDcId);
}

void AbstractConnection::logInfo(const QString &message) {}

void AbstractConnection::logError(const QString &message) {}

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

void SessionData::queueBrowserOnlyServerRefused() {
	++UnitBrowserOnlyServerRefusals;
}

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
	Create(std::shared_ptr<SessionData> data,
		   Instance *instance = reinterpret_cast<Instance *>(quintptr(1))) {
		return std::unique_ptr<SessionPrivate>(new SessionPrivate(
			SessionPrivate::TestTag(), not_null<Instance *>(instance),
			not_null<QThread *>(QThread::currentThread()), std::move(data),
			ShiftedDcId(2)));
	}

	static void SetTestFixture(SessionPrivate &session, bool value) {
		session._testFixture = value;
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

	static void BrowserOnlyError(SessionPrivate &session,
								 not_null<AbstractConnection *> connection) {
		// clang-format off
		session.onError(
			connection,
			kErrorCodeBrowserOnly,
			{});
		// clang-format on
	}

	static void PacketError(SessionPrivate &session,
							AbstractConnection *connection,
							const mtpBuffer &packet) {
		const auto target = not_null<AbstractConnection *>(connection);
		const auto mapped = ServerErrorFromPacket(packet);
		session.onError(target, mapped, {});
	}

	static void ConnectionError(SessionPrivate &session,
								AbstractConnection *connection,
								qint32 errorCode) {
		session.onError(not_null<AbstractConnection *>(connection), errorCode,
						{});
	}

	static bool GaveUpOnBrowserOnlyServer(const SessionPrivate &session) {
		return session._gaveUpOnBrowserOnlyServer;
	}

	static bool RetryTimerActive(const SessionPrivate &session) {
		return session._retryTimer.isActive();
	}

	static void ResetBrowserOnlyServerRefusals() {
		UnitBrowserOnlyServerRefusals = 0;
	}

	static int BrowserOnlyServerRefusals() {
		return UnitBrowserOnlyServerRefusals;
	}

	static void ResumeAfterServerEnrollment(SessionPrivate &session) {
		session.resumeAfterServerEnrollment();
	}

	static void DcOptionsChanged(SessionPrivate &session) {
		session.dcOptionsChanged();
	}

	static int TestConnectionCount(const SessionPrivate &session) {
		return int(session._testConnections.size());
	}

	static bool SessionDataBrowserOnlyRefusal(const SessionPrivate &session) {
		return session._sessionData->browserOnlyServerRefused();
	}

	static void Disconnect(SessionPrivate &session) {
		session._waitForConnectedTimer.cancel();
		session.doDisconnect();
	}

	static crl::time ArmConnectedTimer(SessionPrivate &session,
									   ConnectionPointer connection) {
		session._testConnections.clear();
		session._testConnections.push_back({std::move(connection), 0});
		session.armConnectedTimer();
		return session._waitForConnected;
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

	static bool HasPendingAck(SessionPrivate &session, mtpMsgId messageId) {
		for (const auto &ack : session._ackRequestData) {
			if (ack.v == messageId) {
				return true;
			}
		}
		return false;
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
				  std::shared_ptr<TransportLog> log,
				  crl::time firstPassTimeout = 0)
		: AbstractConnection(thread, proxy), _log(std::move(log)),
		  _firstPassTimeout(firstPassTimeout) {}

	[[nodiscard]] ConnectionPointer clone(const ProxyData &proxy) override {
		return ConnectionPointer::New<FakeTransport>(thread(), proxy, _log);
	}
	[[nodiscard]] crl::time pingTime() const override { return 0; }
	[[nodiscard]] crl::time fullConnectTimeout() const override { return 0; }
	[[nodiscard]] crl::time firstPassConnectTimeout() const override {
		return _firstPassTimeout;
	}
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
	crl::time _firstPassTimeout = 0;
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

[[nodiscard]] mtpBuffer
MakeAuthenticatedServerPacket(const AuthKeyPtr &key, uint64 salt,
							  uint64 sessionId, mtpMsgId messageId,
							  const mtpBuffer &body, uint32 seqNo = 0) {
	auto decrypted = mtpBuffer(8 + body.size() + 3);
	memcpy(decrypted.data(), &salt, sizeof(salt));
	memcpy(decrypted.data() + 2, &sessionId, sizeof(sessionId));
	memcpy(decrypted.data() + 4, &messageId, sizeof(messageId));
	decrypted[6] = seqNo;
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

namespace MTP {

UnitEndpointRefreshProofResult UnitVerifyEndpointAndResumeRequestsForTest(
	const AuthKeyPtr &sessionKey, const AuthKeyPtr &responseKey,
	const UnitEndpointRefreshCommitForTest &commit) {
	if (!sessionKey || !responseKey || !commit) {
		return {};
	}
	constexpr auto kSessionId = uint64(0x1122334455667788ULL);
	constexpr auto kSessionSalt = uint64(42);
	constexpr auto kVerifierRequestId = mtpRequestId(170);
	constexpr auto kQueuedRequestId = mtpRequestId(171);
	const auto mainInstance = reinterpret_cast<Instance *>(quintptr(1));
	const auto verifierInstance = reinterpret_cast<Instance *>(quintptr(2));
	UnitBlockedServerEnrollmentInstance = mainInstance;
	auto unpinnedOptions = DcOptions(Environment::Production);
	const auto previousOptions = details::UnitProxyCheckOptions;
	details::UnitProxyCheckOptions = &unpinnedOptions;
	const auto resetOptions = gsl::finally(
		[=] { details::UnitProxyCheckOptions = previousOptions; });
	auto log = std::make_shared<::TransportLog>();
	auto verifierLog = std::make_shared<::TransportLog>();
	auto queuedSession = std::unique_ptr<details::SessionPrivate>();
	auto verifier = std::unique_ptr<details::SessionPrivate>();
	details::UnitConnectionInited = true;
	details::UnitNeedToResumeAndSend = [&] {
		if (queuedSession) {
			queuedSession->tryToSend();
		}
	};
	const auto resetHooks = gsl::finally([] {
		details::UnitConnectionInited = false;
		details::UnitNeedToResumeAndSend = nullptr;
		UnitBlockedServerEnrollmentInstance = nullptr;
	});
	auto queuedData = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	queuedSession
		= details::SessionPrivateTestAccess::Create(queuedData, mainInstance);
	details::SessionPrivateTestAccess::SetTestFixture(*queuedSession, false);
	auto queuedConnection = details::ConnectionPointer::New<::FakeTransport>(
		QThread::currentThread(), ProxyData(), log);
	details::SessionPrivateTestAccess::StartConnected(
		*queuedSession, std::move(queuedConnection), sessionKey, kSessionId,
		kSessionSalt);
	{
		QWriteLocker locker(queuedData->toSendMutex());
		queuedData->toSendMap().emplace(
			kQueuedRequestId, ::MakeUsernameCheckRequest(kQueuedRequestId));
	}
	queuedSession->tryToSend();
	const auto requestWasPausedBeforeProof
		= log->sent.empty()
		  && !details::SessionPrivateTestAccess::SentMessageId(
			  *queuedSession, kQueuedRequestId);
	if (!sessionKey->equals(responseKey)) {
		const auto refreshCommitted = commit(false, [] {});
		return {
			.requestWasPausedBeforeProof = requestWasPausedBeforeProof,
			.proofAccepted = false,
			.requestResumed = false,
			.refreshCommitted = refreshCommitted,
		};
	}

	auto verifierData = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	verifier = details::SessionPrivateTestAccess::Create(verifierData,
														 verifierInstance);
	details::SessionPrivateTestAccess::SetTestFixture(*verifier, false);
	auto verifierConnection = details::ConnectionPointer::New<::FakeTransport>(
		QThread::currentThread(), ProxyData(), verifierLog);
	details::SessionPrivateTestAccess::StartConnected(
		*verifier, std::move(verifierConnection), sessionKey, kSessionId,
		kSessionSalt);
	{
		QWriteLocker locker(verifierData->toSendMutex());
		verifierData->toSendMap().emplace(
			kVerifierRequestId, ::MakeUsernameCheckRequest(kVerifierRequestId));
	}
	verifier->tryToSend();
	const auto requestMessageId
		= details::SessionPrivateTestAccess::SentMessageId(*verifier,
														   kVerifierRequestId);
	if (!requestMessageId) {
		return {.requestWasPausedBeforeProof = requestWasPausedBeforeProof};
	}
	const auto response = ::MakeAuthenticatedServerPacket(
		responseKey, kSessionSalt, kSessionId, ::ServerMessageId(),
		::MakeUsernameCheckReply(requestMessageId));
	details::SessionPrivateTestAccess::Receive(*verifier, response);
	const auto proofAccepted
		= verifier->getState() == ConnectedState
		  && details::SessionPrivateTestAccess::ReceivedCount(*verifierData)
				 == 1;
	auto requestResumed = false;
	auto requestCompleted = false;
	const auto refreshCommitted = commit(proofAccepted, [&] {
		UnitBlockedServerEnrollmentInstance = nullptr;
		queuedSession->tryToSend();
		const auto queuedMessageId
			= details::SessionPrivateTestAccess::SentMessageId(
				*queuedSession, kQueuedRequestId);
		requestResumed = queuedMessageId != 0;
		if (requestResumed) {
			details::SessionPrivateTestAccess::Receive(
				*queuedSession,
				::MakeAuthenticatedServerPacket(
					sessionKey, kSessionSalt, kSessionId, ::ServerMessageId(),
					::MakeUsernameCheckReply(queuedMessageId)));
			requestCompleted
				= details::SessionPrivateTestAccess::ReceivedCount(*queuedData)
				  == 1;
		}
	});
	return {
		.requestWasPausedBeforeProof = requestWasPausedBeforeProof,
		.proofAccepted = proofAccepted,
		.requestResumed = requestResumed,
		.requestCompleted = requestCompleted,
		.refreshCommitted = refreshCommitted,
	};
}

} // namespace MTP

TEST_CASE(IdleReconnectResumesUsernameCheckWithStoredKey) {
	RunReconnectScenario(AuthKey::Type::ReadFromFile);
}

TEST_CASE(IdleReconnectResumesUsernameCheckWithFreshKey) {
	RunReconnectScenario(AuthKey::Type::Generated);
}

TEST_CASE(DuplicateOddSeqnoReplyIsAcknowledgedAfterReconnect) {
	constexpr auto kSessionId = uint64(0x1122334455667788ULL);
	constexpr auto kSessionSalt = uint64(42);
	constexpr auto kRequestId = mtpRequestId(17);
	const auto duplicateMessageId = ServerMessageId();
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
	const auto key = MakeTestAuthKey(AuthKey::Type::ReadFromFile);
	auto connection = ConnectionPointer::New<FakeTransport>(
		QThread::currentThread(), ProxyData(), log);
	SessionPrivateTestAccess::StartConnected(*session, std::move(connection),
											 key, kSessionId, kSessionSalt);
	{
		QWriteLocker locker(data->toSendMutex());
		data->toSendMap().emplace(kRequestId,
								  MakeUsernameCheckRequest(kRequestId));
	}
	session->tryToSend();
	const auto requestMsgId
		= SessionPrivateTestAccess::SentMessageId(*session, kRequestId);
	CHECK(requestMsgId != 0);
	const auto retransmitted = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, duplicateMessageId,
		MakeUsernameCheckReply(requestMsgId), 1);
	SessionPrivateTestAccess::Receive(*session, retransmitted);
	CHECK_EQ(session->getState(), ConnectedState);
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(1));
	CHECK(
		SessionPrivateTestAccess::HasPendingAck(*session, duplicateMessageId));

	session->tryToSend();
	CHECK(
		!SessionPrivateTestAccess::HasPendingAck(*session, duplicateMessageId));
	CHECK(!log->sent.empty());
	log->sent.clear();

	SessionPrivateTestAccess::IdleDisconnect(*session);
	CHECK(session->getState() < 0);
	SessionPrivateTestAccess::Reconnect(
		*session, ConnectionPointer::New<FakeTransport>(
					  QThread::currentThread(), ProxyData(), log));
	SessionPrivateTestAccess::Receive(*session, retransmitted);
	CHECK_EQ(session->getState(), ConnectingState);
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(1));
	CHECK(
		SessionPrivateTestAccess::HasPendingAck(*session, duplicateMessageId));

	const auto reconnectReply = MakeAuthenticatedServerPacket(
		key, kSessionSalt, kSessionId, ServerMessageId(), MakeUntrackedPong());
	SessionPrivateTestAccess::Receive(*session, reconnectReply);
	CHECK_EQ(session->getState(), ConnectedState);
	CHECK_EQ(quint64(SessionPrivateTestAccess::ReceivedCount(*data)),
			 quint64(1));
	CHECK(
		!SessionPrivateTestAccess::HasPendingAck(*session, duplicateMessageId));
	CHECK(!log->sent.empty());
}

// The session destroys a connection when its connect deadline passes, so a
// connection that has to exhaust TCP before it may use WSS needs a first
// deadline long enough for that; otherwise every retry restarts at TCP.
TEST_CASE(ConnectDeadlineCoversWebSocketFallbackOnTheFirstPass) {
	auto data = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	auto session = SessionPrivateTestAccess::Create(data);
	auto log = std::make_shared<TransportLog>();

	const auto plain = SessionPrivateTestAccess::ArmConnectedTimer(
		*session, ConnectionPointer::New<FakeTransport>(
					  QThread::currentThread(), ProxyData(), log));
	CHECK_EQ(int(plain), 1000);

	const auto withFallback = SessionPrivateTestAccess::ArmConnectedTimer(
		*session,
		ConnectionPointer::New<FakeTransport>(
			QThread::currentThread(), ProxyData(), log, crl::time(30000)));
	CHECK_EQ(int(withFallback), 30000);
}

TEST_CASE(ServerErrorFromPacketDistinguishesBrowserOnlyResponse) {
	const auto browserOnlyPacket = mtpBuffer(1, -498);
	const auto browserOnlyError = ServerErrorFromPacket(browserOnlyPacket);
	CHECK_EQ(browserOnlyError, kErrorCodeBrowserOnlyResponse);
	const auto serverErrorPacket = mtpBuffer(1, -404);
	const auto serverError = ServerErrorFromPacket(serverErrorPacket);
	CHECK_EQ(serverError, -404);
}

TEST_CASE(HttpConnectionMapsBrowserOnlyStatusToResponseCode) {
	auto reply = HttpStatusReply(498);
	auto connection
		= MTP::details::HttpConnection(QThread::currentThread(), ProxyData());
	CHECK_EQ(connection.handleError(&reply), kErrorCodeBrowserOnlyResponse);
	auto serverReply = HttpStatusReply(404);
	CHECK_EQ(connection.handleError(&serverReply), -404);
}

TEST_CASE(SessionRequestsEndpointRefreshOnlyForTransportFailure) {
	auto instance
		= MTP::Instance(MTP::Instance::Mode::Normal, MTP::Instance::Fields());
	const auto makeSession = [&] {
		auto data = std::make_shared<MTP::details::SessionData>(
			not_null<MTP::details::Session *>(
				reinterpret_cast<MTP::details::Session *>(quintptr(1))));
		return MTP::details::SessionPrivateTestAccess::Create(data, &instance);
	};
	const auto makeConnection = [] {
		return MTP::details::ConnectionPointer::New<FakeTransport>(
			QThread::currentThread(), ProxyData(),
			std::make_shared<TransportLog>());
	};
	MTP::UnitServerEndpointRefreshRequests.clear();
	auto transportSession = makeSession();
	auto transportConnection = makeConnection();
	MTP::details::SessionPrivateTestAccess::ConnectionError(
		*transportSession, transportConnection.get(),
		MTP::details::AbstractConnection::kErrorCodeOther);
	QCoreApplication::processEvents();
	CHECK_EQ(int(MTP::UnitServerEndpointRefreshRequests.size()), 1);
	if (!MTP::UnitServerEndpointRefreshRequests.empty()) {
		CHECK_EQ(int(MTP::UnitServerEndpointRefreshRequests.front()), 2);
	}
	MTP::details::SessionPrivateTestAccess::Disconnect(*transportSession);

	MTP::UnitServerEndpointRefreshRequests.clear();
	auto responseSession = makeSession();
	auto responseConnection = makeConnection();
	MTP::details::SessionPrivateTestAccess::ConnectionError(
		*responseSession, responseConnection.get(),
		MTP::details::kErrorCodeBrowserOnlyResponse);
	QCoreApplication::processEvents();
	CHECK(MTP::UnitServerEndpointRefreshRequests.empty());
	MTP::details::SessionPrivateTestAccess::Disconnect(*responseSession);
}

TEST_CASE(ServerErrorFromPacketDoesNotRecordBrowserOnlyRefusal) {
	SessionPrivateTestAccess::ResetBrowserOnlyServerRefusals();
	auto data = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	auto session = SessionPrivateTestAccess::Create(data);
	const auto thread = QThread::currentThread();
	const auto proxy = ProxyData();
	const auto log = std::make_shared<TransportLog>();
	auto connection = ConnectionPointer::New<FakeTransport>(thread, proxy, log);
	const auto packet = mtpBuffer(1, -498);
	SessionPrivateTestAccess::PacketError(*session, connection.get(), packet);

	CHECK(!SessionPrivateTestAccess::GaveUpOnBrowserOnlyServer(*session));
	CHECK_EQ(SessionPrivateTestAccess::BrowserOnlyServerRefusals(), 0);
	CHECK(!SessionPrivateTestAccess::SessionDataBrowserOnlyRefusal(*session));
	SessionPrivateTestAccess::Disconnect(*session);
}

TEST_CASE(BrowserOnlyServerRefusalStopsSessionRetries) {
	const auto previousOptions = MTP::details::UnitProxyCheckOptions;
	MTP::details::UnitProxyCheckOptions = &UnitDcOptions;
	const auto resetOptions = gsl::finally(
		[=] { MTP::details::UnitProxyCheckOptions = previousOptions; });
	SessionPrivateTestAccess::ResetBrowserOnlyServerRefusals();
	auto log = std::make_shared<TransportLog>();
	SetServerConnectionTestFactory(
		[=](QThread *thread, const ProxyData &proxy) {
			return ConnectionPointer::New<FakeTransport>(thread, proxy, log);
		});
	const auto resetFactory
		= gsl::finally([] { SetServerConnectionTestFactory({}); });
	auto data = std::make_shared<SessionData>(
		not_null<Session *>(reinterpret_cast<Session *>(quintptr(1))));
	auto options = SessionOptions();
	options.useIPv6 = false;
	options.useHttp = false;
	data->setOptions(std::move(options));
	auto session = SessionPrivateTestAccess::Create(data);
	// clang-format off
	auto connection = ConnectionPointer::New<FakeTransport>(
		QThread::currentThread(),
		ProxyData(),
		std::make_shared<TransportLog>());
	SessionPrivateTestAccess::BrowserOnlyError(
		*session,
		not_null<AbstractConnection *>(connection.get()));
	// clang-format on

	CHECK(SessionPrivateTestAccess::GaveUpOnBrowserOnlyServer(*session));
	CHECK(!SessionPrivateTestAccess::RetryTimerActive(*session));
	CHECK_EQ(SessionPrivateTestAccess::BrowserOnlyServerRefusals(), 1);
	CHECK(SessionPrivateTestAccess::SessionDataBrowserOnlyRefusal(*session));

	SessionPrivateTestAccess::ResumeAfterServerEnrollment(*session);
	CHECK(SessionPrivateTestAccess::GaveUpOnBrowserOnlyServer(*session));
	CHECK_EQ(SessionPrivateTestAccess::BrowserOnlyServerRefusals(), 2);
	CHECK(!SessionPrivateTestAccess::RetryTimerActive(*session));
	CHECK_EQ(SessionPrivateTestAccess::TestConnectionCount(*session), 0);

	SessionPrivateTestAccess::DcOptionsChanged(*session);
	CHECK(!SessionPrivateTestAccess::GaveUpOnBrowserOnlyServer(*session));
	CHECK(!SessionPrivateTestAccess::SessionDataBrowserOnlyRefusal(*session));
	CHECK(SessionPrivateTestAccess::TestConnectionCount(*session) > 0);
	SessionPrivateTestAccess::Disconnect(*session);
}
