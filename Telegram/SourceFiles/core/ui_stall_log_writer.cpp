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

void TruncateUtf8(QByteArray &bytes, qsizetype size) {
	bytes.truncate(size);
	auto start = bytes.size();
	while (start > 0
		   && (static_cast<unsigned char>(bytes.at(start - 1)) & 0xC0) == 0x80) {
		--start;
	}
	if (!start) {
		bytes.clear();
		return;
	}
	const auto leadIndex = start - 1;
	const auto lead = static_cast<unsigned char>(bytes.at(leadIndex));
	auto expected = 1;
	if ((lead & 0xE0) == 0xC0) {
		expected = 2;
	} else if ((lead & 0xF0) == 0xE0) {
		expected = 3;
	} else if ((lead & 0xF8) == 0xF0) {
		expected = 4;
	}
	if (bytes.size() - leadIndex < expected) {
		bytes.truncate(leadIndex);
	}
}

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
	const auto record = Pending{file.fileName(), prefix.toUtf8(), offset, size};
	_pending = record;
	_last = record;
	return true;
}

QString UiStallLogWriter::fileName() const {
	return _last ? _last->fileName : QString();
}

bool UiStallLogWriter::writeUpdate(QFile &file, const QString &message) {
	if (!_last) {
		return false;
	}
	const auto record = _pending ? *_pending : *_last;
	if (!file.isOpen() || file.fileName() != record.fileName) {
		return false;
	}

	auto line = (QString::fromUtf8(record.prefix) + message).toUtf8();
	const auto slotContentSize = record.size - 1;
	if (line.size() > slotContentSize) {
		TruncateUtf8(line, slotContentSize);
	}
	if (line.size() < slotContentSize) {
		line += QByteArray(int(slotContentSize - line.size()), ' ');
	}
	line += '\n';
	const auto fileSize = file.size();
	if (fileSize < record.offset + record.size) {
		return false;
	}
	if (line.size() != record.size || !file.seek(record.offset)) {
		return false;
	}
	const auto original = file.read(record.size);
	if (file.error() != QFileDevice::NoError
		|| original.size() != record.size || !file.seek(record.offset)) {
		return false;
	}
	const auto written = file.write(line);
	const auto flushed = file.flush();
	if (written != line.size() || !flushed) {
		const auto restored = file.seek(record.offset)
							  && file.write(original) == original.size()
							  && file.flush();
		if (!restored) {
			reset();
		}
		return false;
	}
	_last = record;
	_pending.reset();
	return true;
}

void UiStallLogWriter::reset() {
	_pending.reset();
	_last.reset();
}

} // namespace Core
