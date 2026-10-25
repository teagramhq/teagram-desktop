/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ui_stall_log_writer.h"

#include <QtCore/QFile>

namespace Core {
namespace {

constexpr auto kDurationReserve = 19;

} // namespace

bool UiStallLogWriter::writeDetection(QFile &file, const QString &prefix,
									  const QString &message) {
	reset();
	if (!file.isOpen()) {
		return false;
	}

	auto bytes = (prefix + message).toUtf8();
	bytes += QByteArray(kDurationReserve, ' ');
	bytes += '\n';
	const auto offset = file.size();
	const auto size = bytes.size();
	const auto written = file.write(bytes);
	const auto flushed = file.flush();
	if (written != size || !flushed) {
		return false;
	}
	_pending = Pending{file.fileName(), prefix.toUtf8(), offset, size};
	return true;
}

QString UiStallLogWriter::fileName() const {
	return _pending ? _pending->fileName : QString();
}

bool UiStallLogWriter::writeCompletion(QFile &file, const QString &message) {
	if (!_pending) {
		return false;
	}
	const auto pending = *_pending;
	reset();
	if (!file.isOpen() || file.fileName() != pending.fileName) {
		return false;
	}

	auto bytes = (QString::fromUtf8(pending.prefix) + message).toUtf8();
	const auto padding = pending.size - bytes.size() - 1;
	if (padding < 0 || padding > kDurationReserve) {
		return false;
	}
	bytes += QByteArray(int(padding), ' ');
	bytes += '\n';
	if (file.size() < pending.offset + bytes.size()
		|| !file.seek(pending.offset)) {
		return false;
	}
	const auto written = file.write(bytes);
	const auto flushed = file.flush();
	return written == bytes.size() && flushed;
}

void UiStallLogWriter::reset() { _pending.reset(); }

} // namespace Core
