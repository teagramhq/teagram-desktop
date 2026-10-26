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
	if (line.size() <= record.size - 1) {
		line += QByteArray(int(record.size - 1 - line.size()), ' ');
	} else {
		line += QByteArray(kDurationReserve, ' ');
	}
	line += '\n';
	const auto fileSize = file.size();
	if (fileSize < record.offset + record.size) {
		return false;
	}
	if (line.size() == record.size) {
		if (!file.seek(record.offset)) {
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
		auto updated = record;
		updated.size = line.size();
		_last = updated;
		_pending.reset();
		return true;
	}
	if (!file.seek(record.offset)) {
		return false;
	}
	const auto contents = file.readAll();
	const auto expectedSuffixSize = fileSize - record.offset;
	if (file.error() != QFileDevice::NoError
		|| contents.size() != expectedSuffixSize) {
		return false;
	}
	auto bytes = line;
	bytes += contents.mid(record.size);
	if (!file.seek(record.offset)) {
		return false;
	}
	const auto written = file.write(bytes);
	const auto size = record.offset + bytes.size();
	const auto resized = file.resize(size);
	const auto flushed = file.flush();
	if (written != bytes.size() || !resized || !flushed) {
		const auto restored = [&] {
			if (!file.seek(record.offset)) {
				return false;
			}
			const auto restoredBytes = file.write(contents);
			if (restoredBytes != contents.size()
				|| !file.resize(record.offset + contents.size())) {
				return false;
			}
			return file.flush();
		}();
		if (!restored) {
			reset();
		}
		return false;
	}
	auto updated = record;
	updated.size = line.size();
	_last = updated;
	_pending.reset();
	return true;
}

void UiStallLogWriter::reset() {
	_pending.reset();
	_last.reset();
}

} // namespace Core
