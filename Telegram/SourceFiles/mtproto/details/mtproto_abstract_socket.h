/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"
#include "base/basic_types.h"

namespace MTP::details {

[[nodiscard]] inline bool IsGoodObfuscatedStartNonce(bytes::const_span nonce) {
	Expects(nonce.size() >= 2 * sizeof(uint32));

	const auto bytes = nonce.data();
	const auto zero = *reinterpret_cast<const uchar *>(bytes);
	const auto first = *reinterpret_cast<const uint32 *>(bytes);
	const auto second = *(reinterpret_cast<const uint32 *>(bytes) + 1);
	const auto reserved01 = 0x000000EFU;
	const auto reserved11 = 0x44414548U;
	const auto reserved12 = 0x54534F50U;
	const auto reserved13 = 0x20544547U;
	const auto reserved14 = 0xEEEEEEEEU;
	const auto reserved15 = 0xDDDDDDDDU;
	const auto reserved16 = 0x02010316U;
	const auto reserved21 = 0x00000000U;
	return (zero != reserved01) && (first != reserved11)
		   && (first != reserved12) && (first != reserved13)
		   && (first != reserved14) && (first != reserved15)
		   && (first != reserved16) && (second != reserved21);
}

class AbstractSocket : protected QObject {
  public:
	static std::unique_ptr<AbstractSocket>
	Create(not_null<QThread *> thread, const bytes::vector &secret,
		   const QNetworkProxy &proxy, bool protocolForFiles,
		   const QString &webSocketEndpoint = {},
		   const QString &webSocketAddress = {});

	void setDebugId(const QString &id) {
		_debugId = id;
	}

	explicit AbstractSocket(not_null<QThread*> thread) {
		moveToThread(thread);
	}
	virtual ~AbstractSocket() = default;

	[[nodiscard]] rpl::producer<> connected() const {
		return _connected.events();
	}
	[[nodiscard]] rpl::producer<> disconnected() const {
		return _disconnected.events();
	}
	[[nodiscard]] rpl::producer<> readyRead() const {
		return _readyRead.events();
	}
	[[nodiscard]] rpl::producer<> error() const {
		return _error.events();
	}
	[[nodiscard]] virtual bool isBrowserOnlyRefusal() const { return false; }
	[[nodiscard]] rpl::producer<> syncTimeRequests() const {
		return _syncTimeRequests.events();
	}

	virtual void connectToHost(const QString &address, int port) = 0;
	[[nodiscard]] virtual bool isGoodStartNonce(bytes::const_span nonce) = 0;
	virtual void timedOut() = 0;
	[[nodiscard]] virtual bool isConnected() = 0;
	[[nodiscard]] virtual bool hasBytesAvailable() = 0;
	[[nodiscard]] virtual int64 read(bytes::span buffer) = 0;
	virtual void write(
		bytes::const_span prefix,
		bytes::const_span buffer) = 0;

	virtual int32 debugState() = 0;
	[[nodiscard]] virtual QString debugPostfix() const = 0;

protected:
	static const int kFilesSendBufferSize = 2 * 1024 * 1024;
	static const int kFilesReceiveBufferSize = 2 * 1024 * 1024;

	void logError(int errorCode, const QString &errorText);

	QString _debugId;
	rpl::event_stream<> _connected;
	rpl::event_stream<> _disconnected;
	rpl::event_stream<> _readyRead;
	rpl::event_stream<> _error;
	rpl::event_stream<> _syncTimeRequests;

};

} // namespace MTP::details
