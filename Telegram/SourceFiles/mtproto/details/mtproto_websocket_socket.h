/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/details/mtproto_abstract_socket.h"

#include <QtCore/QUrl>
#include <QtNetwork/QSslSocket>

namespace MTP::details {

enum class WebSocketUpgradeResult {
	Incomplete,
	Accepted,
	Rejected,
};

// Consumes a complete upgrade response from `incoming` and validates it
// against the handshake `key`. Bytes after the response are kept.
[[nodiscard]] WebSocketUpgradeResult
ParseWebSocketUpgrade(QByteArray &incoming, const QByteArray &key,
					  QString &error);

class WebSocketSocket final : public AbstractSocket {
  public:
	WebSocketSocket(not_null<QThread *> thread, const QString &endpoint,
					const QString &resolvedAddress, const QNetworkProxy &proxy,
					bool protocolForFiles);

	void connectToHost(const QString &address, int port) override;
	bool isGoodStartNonce(bytes::const_span nonce) override;
	void timedOut() override;
	bool isConnected() override;
	bool hasBytesAvailable() override;
	int64 read(bytes::span buffer) override;
	void write(bytes::const_span prefix, bytes::const_span buffer) override;

	int32 debugState() override;
	QString debugPostfix() const override;

  private:
	enum class State {
		NotConnected,
		Connecting,
		Handshaking,
		Connected,
		Error,
	};

	void sendHandshake();
	void handleReadyRead();
	void handleHandshake();
	void handleFrames();
	void appendMessage(QByteArray payload, bool final);
	void sendFrame(uchar opcode, const QByteArray &payload);
	void fail(const QString &message);

	const QUrl _url;
	const QString _resolvedAddress;
	QSslSocket _socket;
	State _state = State::NotConnected;
	QByteArray _incoming;
	QByteArray _handshakeKey;
	QByteArray _message;
	QByteArray _readBuffer;
	int _readOffset = 0;
	bool _fragmentedMessage = false;
};

} // namespace MTP::details
