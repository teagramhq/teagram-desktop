/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_websocket_socket.h"

#include "base/bytes.h"
#include "base/invoke_queued.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QMap>
#include <QtCore/QtEndian>
#include <QtNetwork/QSslConfiguration>

#include <algorithm>
#include <cstring>

namespace MTP::details {
namespace {

constexpr auto kMaxWebSocketMessage = 16 * 1024 * 1024 + 64;
constexpr auto kMaxBufferedBytes = 2 * kMaxWebSocketMessage;
constexpr auto kMaxHandshakeBytes = 16 * 1024;
constexpr auto kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] bool HasToken(QByteArray value, QByteArray token) {
	const auto values = value.toLower().split(',');
	for (auto &item : values) {
		if (item.trimmed() == token) {
			return true;
		}
	}
	return false;
}

} // namespace

WebSocketSocket::WebSocketSocket(not_null<QThread *> thread,
								 const QString &endpoint,
								 const QString &resolvedAddress,
								 const QNetworkProxy &proxy,
								 bool protocolForFiles)
	: AbstractSocket(thread), _url(endpoint, QUrl::StrictMode),
	  _resolvedAddress(resolvedAddress) {
	_socket.moveToThread(thread);
	_socket.setProxy(proxy);
	auto sslConfiguration = _socket.sslConfiguration();
	sslConfiguration.setAllowedNextProtocols({"http/1.1"});
	_socket.setSslConfiguration(sslConfiguration);
	if (protocolForFiles) {
		_socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,
								kFilesSendBufferSize);
		_socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
								kFilesReceiveBufferSize);
	}
	const auto wrap = [&](auto handler) {
		return [=](auto &&...args) {
			InvokeQueued(this, [=] { handler(args...); });
		};
	};
	using Error = QAbstractSocket::SocketError;
	connect(&_socket, &QSslSocket::encrypted, wrap([=] { sendHandshake(); }));
	connect(&_socket, &QSslSocket::disconnected, wrap([=] {
		_state = State::NotConnected;
		_disconnected.fire({});
	}));
	connect(&_socket, &QSslSocket::readyRead, wrap([=] { handleReadyRead(); }));
	connect(&_socket, &QAbstractSocket::errorOccurred,
			wrap([=](Error) { fail(_socket.errorString()); }));
}

void WebSocketSocket::connectToHost(const QString &, int) {
	Expects(_state == State::NotConnected);
	if (!_url.isValid() || _url.scheme() != u"wss"_q || _url.host().isEmpty()) {
		fail(u"Invalid WSS endpoint."_q);
		return;
	}
	_state = State::Connecting;
	_socket.setPeerVerifyName(_url.host());
	_socket.connectToHostEncrypted(
		_resolvedAddress.isEmpty() ? _url.host() : _resolvedAddress,
		quint16(_url.port(443)));
}

bool WebSocketSocket::isGoodStartNonce(bytes::const_span) { return true; }

void WebSocketSocket::timedOut() { fail(u"WSS connection timed out."_q); }

bool WebSocketSocket::isConnected() {
	return _state == State::Connected
		   && _socket.state() == QAbstractSocket::ConnectedState;
}

bool WebSocketSocket::hasBytesAvailable() {
	return _readOffset < _readBuffer.size();
}

int64 WebSocketSocket::read(bytes::span buffer) {
	const auto available = _readBuffer.size() - _readOffset;
	const auto count = std::min<int64>(available, buffer.size());
	if (count <= 0) {
		return 0;
	}
	memcpy(buffer.data(), _readBuffer.constData() + _readOffset,
		   std::size_t(count));
	_readOffset += int(count);
	if (_readOffset == _readBuffer.size()) {
		_readBuffer.clear();
		_readOffset = 0;
	} else if (_readOffset >= kMaxWebSocketMessage) {
		_readBuffer.remove(0, _readOffset);
		_readOffset = 0;
	}
	return count;
}

void WebSocketSocket::write(bytes::const_span prefix,
							bytes::const_span buffer) {
	Expects(!buffer.empty());
	if (!isConnected()) {
		return;
	}
	if (prefix.size() > std::size_t(kMaxWebSocketMessage)
		|| buffer.size() > std::size_t(kMaxWebSocketMessage) - prefix.size()) {
		fail(u"WSS message exceeds the transport limit."_q);
		return;
	}
	auto payload = QByteArray();
	if (!prefix.empty()) {
		payload.append(reinterpret_cast<const char *>(prefix.data()),
					   int(prefix.size()));
	}
	payload.append(reinterpret_cast<const char *>(buffer.data()),
				   int(buffer.size()));
	sendFrame(0x2, payload);
}

int32 WebSocketSocket::debugState() { return int32(_socket.state()); }

QString WebSocketSocket::debugPostfix() const { return u"_ws"_q; }

void WebSocketSocket::sendHandshake() {
	if (_state != State::Connecting) {
		return;
	}
	auto random = bytes::vector(16);
	bytes::set_random(bytes::make_span(random));
	_handshakeKey = QByteArray(reinterpret_cast<const char *>(random.data()),
							   int(random.size()))
						.toBase64();
	auto host = _url.host(QUrl::FullyEncoded).toLatin1();
	if (host.contains(':')) {
		host.prepend('[');
		host.append(']');
	}
	const auto explicitPort = _url.port(-1);
	if (explicitPort >= 0) {
		host.append(':');
		host.append(QByteArray::number(explicitPort));
	}
	auto path = _url.path(QUrl::FullyEncoded).toLatin1();
	if (path.isEmpty()) {
		path = "/";
	}
	const auto request = QByteArray("GET ") + path
						 + " HTTP/1.1\r\nHost: " + host
						 + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
						 + "Sec-WebSocket-Key: " + _handshakeKey
						 + "\r\nSec-WebSocket-Version: 13\r\n"
						 + "Sec-WebSocket-Protocol: binary\r\n\r\n";
	_state = State::Handshaking;
	if (_socket.write(request) != request.size()) {
		fail(u"Could not write the WSS upgrade request."_q);
	}
}

void WebSocketSocket::handleReadyRead() {
	_incoming += _socket.readAll();
	if (_incoming.size() > kMaxBufferedBytes + kMaxHandshakeBytes) {
		fail(u"WSS receive buffer exceeded its limit."_q);
		return;
	}
	if (_state == State::Handshaking) {
		handleHandshake();
	}
	if (_state == State::Connected) {
		handleFrames();
	}
}

void WebSocketSocket::handleHandshake() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		if (_incoming.size() > kMaxHandshakeBytes) {
			fail(u"WSS upgrade response is too large."_q);
		}
		return;
	}
	if (end > kMaxHandshakeBytes) {
		fail(u"WSS upgrade response is too large."_q);
		return;
	}
	const auto response = _incoming.left(end);
	_incoming.remove(0, end + 4);
	auto lines = response.split('\n');
	if (lines.isEmpty()) {
		fail(u"WSS upgrade response is empty."_q);
		return;
	}
	auto status = lines.takeFirst();
	if (!status.endsWith('\r')) {
		fail(u"WSS upgrade status line is malformed."_q);
		return;
	}
	status.chop(1);
	const auto statusParts = status.split(' ');
	if (statusParts.size() < 2 || statusParts[0] != "HTTP/1.1"
		|| statusParts[1] != "101") {
		fail(u"WSS upgrade was refused."_q);
		return;
	}
	auto headers = QMap<QByteArray, QByteArray>();
	for (auto line : lines) {
		if (!line.endsWith('\r')) {
			fail(u"WSS upgrade header is malformed."_q);
			return;
		}
		line.chop(1);
		const auto separator = line.indexOf(':');
		if (separator <= 0) {
			fail(u"WSS upgrade header is malformed."_q);
			return;
		}
		const auto name = line.left(separator).trimmed().toLower();
		const auto value = line.mid(separator + 1).trimmed();
		if (headers.contains(name)) {
			fail(u"WSS upgrade contains duplicate headers."_q);
			return;
		}
		headers.insert(name, value);
	}
	const auto expectedAccept
		= QCryptographicHash::hash(_handshakeKey + kWebSocketGuid,
								   QCryptographicHash::Sha1)
			  .toBase64();
	if (headers.value("upgrade").compare("websocket", Qt::CaseInsensitive) != 0
		|| !HasToken(headers.value("connection"), "upgrade")
		|| headers.value("sec-websocket-accept") != expectedAccept
		|| headers.value("sec-websocket-protocol") != "binary"
		|| headers.contains("sec-websocket-extensions")) {
		fail(u"WSS upgrade response failed validation."_q);
		return;
	}
	_state = State::Connected;
	_connected.fire({});
}

void WebSocketSocket::handleFrames() {
	while (_state == State::Connected) {
		if (_incoming.size() < 2) {
			return;
		}
		const auto first = uchar(_incoming[0]);
		const auto second = uchar(_incoming[1]);
		const auto final = (first & 0x80) != 0;
		const auto opcode = first & 0x0F;
		const auto masked = (second & 0x80) != 0;
		if ((first & 0x70) != 0 || masked) {
			fail(u"WSS server frame has invalid flags."_q);
			return;
		}
		quint64 length = second & 0x7F;
		auto headerSize = 2;
		if (length == 126) {
			if (_incoming.size() < 4) {
				return;
			}
			length = qFromBigEndian<quint16>(
				reinterpret_cast<const uchar *>(_incoming.constData() + 2));
			headerSize = 4;
		} else if (length == 127) {
			if (_incoming.size() < 10) {
				return;
			}
			length = qFromBigEndian<quint64>(
				reinterpret_cast<const uchar *>(_incoming.constData() + 2));
			if (length & (quint64(1) << 63)) {
				fail(u"WSS server frame length is invalid."_q);
				return;
			}
			headerSize = 10;
		}
		const auto control = (opcode & 0x08) != 0;
		if (length > kMaxWebSocketMessage
			|| (control && (!final || length > 125))) {
			fail(u"WSS server frame exceeds its limit."_q);
			return;
		}
		if (length > quint64(_incoming.size() - headerSize)) {
			return;
		}
		const auto payload = _incoming.mid(headerSize, int(length));
		_incoming.remove(0, headerSize + int(length));
		switch (opcode) {
		case 0x0:
			if (!_fragmentedMessage) {
				fail(u"Unexpected WSS continuation frame."_q);
				return;
			}
			appendMessage(payload, final);
			break;
		case 0x1:
			fail(u"WSS text frames are not supported."_q);
			return;
		case 0x2:
			if (_fragmentedMessage) {
				fail(u"Nested WSS data frame."_q);
				return;
			}
			appendMessage(payload, final);
			break;
		case 0x8:
			if (payload.size() == 1) {
				fail(u"WSS close frame is malformed."_q);
				return;
			}
			sendFrame(0x8, payload);
			_socket.disconnectFromHost();
			return;
		case 0x9:
			sendFrame(0xA, payload);
			break;
		case 0xA:
			break;
		default:
			fail(u"WSS frame opcode is invalid."_q);
			return;
		}
	}
}

void WebSocketSocket::appendMessage(QByteArray payload, bool final) {
	if (!_fragmentedMessage) {
		_message.clear();
	}
	if (_message.size() + payload.size() > kMaxWebSocketMessage) {
		fail(u"WSS message exceeds the transport limit."_q);
		return;
	}
	_message += payload;
	_fragmentedMessage = !final;
	if (!final) {
		return;
	}
	if (_readOffset) {
		_readBuffer.remove(0, _readOffset);
		_readOffset = 0;
	}
	if (_readBuffer.size() + _message.size() > kMaxBufferedBytes) {
		fail(u"WSS receive buffer exceeded its limit."_q);
		return;
	}
	_readBuffer += _message;
	_message.clear();
	_readyRead.fire({});
}

void WebSocketSocket::sendFrame(uchar opcode, const QByteArray &payload) {
	if (payload.size() > kMaxWebSocketMessage) {
		fail(u"WSS frame exceeds the transport limit."_q);
		return;
	}
	auto frame = QByteArray();
	frame.reserve(payload.size() + 14);
	frame.append(char(0x80 | opcode));
	if (payload.size() < 126) {
		frame.append(char(0x80 | payload.size()));
	} else if (payload.size() <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		const auto length = qToBigEndian(quint16(payload.size()));
		frame.append(reinterpret_cast<const char *>(&length), sizeof(length));
	} else {
		frame.append(char(0x80 | 127));
		const auto length = qToBigEndian(quint64(payload.size()));
		frame.append(reinterpret_cast<const char *>(&length), sizeof(length));
	}
	auto mask = bytes::vector(4);
	bytes::set_random(bytes::make_span(mask));
	frame.append(reinterpret_cast<const char *>(mask.data()), int(mask.size()));
	const auto offset = frame.size();
	frame.resize(offset + payload.size());
	for (auto i = 0; i != payload.size(); ++i) {
		frame[offset + i] = payload[i] ^ char(mask[i % 4]);
	}
	if (_socket.write(frame) != frame.size()) {
		fail(_socket.errorString());
	}
}

void WebSocketSocket::fail(const QString &message) {
	if (_state == State::Error) {
		return;
	}
	logError(QAbstractSocket::UnknownSocketError, message);
	_state = State::Error;
	_error.fire({});
	_socket.abort();
}

} // namespace MTP::details
