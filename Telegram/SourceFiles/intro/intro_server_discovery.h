/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_server_discovery.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

#include <functional>
#include <optional>

class QSocketNotifier;
class QHostInfo;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Intro {
namespace details {

namespace internal {

[[nodiscard]] bool ConfigureNativeSocketForSend(
	qintptr descriptor,
	int &error);
[[nodiscard]] qint64 SendNativeSocket(
	qintptr descriptor,
	const char *data,
	int size,
	int &error);

}

enum class PublicDiscoveryAddressRole {
	Mtproto,
	WebSocket,
};

enum class PublicDiscoveryAddressResolution {
	Rejected,
	NeedsWebSocket,
	Resolved,
};

[[nodiscard]] PublicDiscoveryAddressResolution
ResolvePublicDiscoveryAddresses(const MTP::ServerSelectionCheck &selection,
								MTP::ServerDiscoveryResult &result,
								PublicDiscoveryAddressRole role,
								const QList<QHostAddress> &addresses);

class ServerDiscoveryFlow final {
public:
	struct Callbacks {
		std::function<void()> publicHttps;
		std::function<void()> localDirect;
		std::function<void(bool)> failed;
	};

	[[nodiscard]] bool start(
		const MTP::ServerSelectionCheck &selection,
		Callbacks callbacks);
	[[nodiscard]] bool discoveryFailed(bool connectionFailure);
	void finish();
	void cancel();

	[[nodiscard]] bool active() const {
		return _active;
	}

private:
	Callbacks _callbacks;
	bool _active = false;
};

struct ServerDiscoveryFailurePresentation {
	QString message;
	bool selectionEditable = false;
	bool continueEnabled = false;
};

using ServerDiscoveryBrowserOnlyHandler
	= std::function<void(ServerDiscoveryFailurePresentation)>;
using ServerDiscoveryInvalidHandler = std::function<void()>;
using ServerDiscoveryReadyHandler
	= std::function<void(MTP::ServerDiscoveryResult)>;

void HandleServerDiscoveryResult(ServerDiscoveryFlow &flow,
								 MTP::ServerDiscoveryResult result,
								 ServerDiscoveryBrowserOnlyHandler browserOnly,
								 ServerDiscoveryInvalidHandler invalid,
								 ServerDiscoveryReadyHandler ready);

class ServerWidgetDiscovery final : public QObject {
public:
	struct Attempt {
		std::optional<MTP::ServerDiscoveryAttempt> token;
		bool fieldEditable = false;
		bool retryable = false;
	};

	struct Callbacks {
		std::function<void(MTP::ServerDiscoveryResult)> finished;
		std::function<void(bool)> failed;
		std::function<void()> candidateStarted;
		std::function<void()> beforeRequestSend;
	};

	explicit ServerWidgetDiscovery(QObject *parent);
	~ServerWidgetDiscovery() override;

	[[nodiscard]] Attempt acquireAttempt() const;

	void start(
		const MTP::ServerSelectionCheck &selection,
		const QByteArray &nonce,
		const QList<QHostAddress> &addresses,
		Callbacks callbacks);
	void cancel();

	[[nodiscard]] bool timeout();
	[[nodiscard]] bool running() const {
		return _running;
	}

private:
	void startNextAddress();
	void sendRequest();
	void nativeReadyRead();
	void finishNativeRead();
	void closeNativeSocket();
	void fail(bool connectionFailure);
	void finish(MTP::ServerDiscoveryResult result);
	void stopSocket();

	MTP::ServerSelectionCheck _selection;
	QByteArray _nonce;
	QByteArray _request;
	QByteArray _response;
	QList<QHostAddress> _addresses;
	int _nextAddress = 0;
	int _writeOffset = 0;
	bool _beforeRequestSendNotified = false;
	bool _writeClosed = false;
	QHostAddress _resolvedAddress;
	qintptr _nativeReadDescriptor = -1;
	bool _connecting = false;
	QSocketNotifier *_nativeReadNotifier = nullptr;
	QSocketNotifier *_nativeWriteNotifier = nullptr;
	Callbacks _callbacks;
	bool _running = false;
};

// Reuses the same bounded discovery policy for an authenticated account's
// endpoint refresh. The caller still has to prove the discovered endpoint
// with its existing encrypted MTProto session before persisting it.
class ServerEndpointRefreshDiscovery final : public QObject {
public:
	struct Callbacks {
		std::function<void(MTP::ServerDiscoveryResult)> finished;
		std::function<void(bool)> failed;
	};

	explicit ServerEndpointRefreshDiscovery(QObject *parent = nullptr);
	~ServerEndpointRefreshDiscovery() override;

	[[nodiscard]] bool start(
		const MTP::ServerSelectionCheck &selection,
		const QString &origin,
		Callbacks callbacks);
	[[nodiscard]] bool running() const {
		return _running;
	}
	void cancel();

private:
	void beginPublicDiscovery();
	void beginLocalDiscovery();
	void startLocalDiscovery(const QList<QHostAddress> &addresses);
	void publicDiscoveryFinished();
	void resolvePublicEndpoint(MTP::ServerDiscoveryResult result);
	void publicEndpointResolved(
		MTP::ServerDiscoveryResult result,
		const QHostInfo &info);
	void resolvePublicWebSocketEndpoint(MTP::ServerDiscoveryResult result);
	void publicWebSocketEndpointResolved(
		MTP::ServerDiscoveryResult result,
		const QHostInfo &info);
	void discoveryTimeout();
	void fail(bool connectionFailure);
	void finish(MTP::ServerDiscoveryResult result);
	void stop();

	MTP::ServerSelectionCheck _selection;
	QString _origin;
	std::optional<MTP::ServerDiscoveryAttempt> _attemptToken;
	QNetworkAccessManager *_network = nullptr;
	QNetworkReply *_reply = nullptr;
	ServerWidgetDiscovery *_localDiscovery = nullptr;
	QTimer *_deadline = nullptr;
	QByteArray _publicResponse;
	QByteArray _localNonce;
	int _hostLookupId = -1;
	quint64 _attempt = 0;
	Callbacks _callbacks;
	bool _running = false;
};

[[nodiscard]] bool SubmitServerSelection(
	const QString &value,
	const std::function<void(MTP::ServerSelectionStatus)> &rejected,
	const std::function<void(const MTP::ServerSelectionCheck &)> &accepted);

} // namespace details
} // namespace Intro
