/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/sticker_import.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace Core {
namespace {

constexpr auto kMaximumArchiveBytes = quint64(120) * 1024 * 1024;
constexpr auto kMaximumExpandedBytes = quint64(110) * 1024 * 1024;
constexpr auto kMaximumEntries = 201;
constexpr auto kMaximumEntryBytes = 512 * 1024;
constexpr auto kMaximumArchiveNameBytes = 4096;
constexpr auto kMaximumStickerCount = 120;
constexpr auto kMaximumEmojiCount = 200;
constexpr auto kMaximumStickerSetTitleLength = 64;
constexpr auto kMaximumShortNameLength = 64;
constexpr auto kCentralSignature = quint32(0x02014B50);
constexpr auto kLocalSignature = quint32(0x04034B50);

struct ZipEntry {
	QString name;
	quint16 flags = 0;
	quint16 method = 0;
	quint32 crc = 0;
	quint32 compressedSize = 0;
	quint32 expandedSize = 0;
	quint32 localOffset = 0;
	quint64 dataOffset = 0;
	bool directory = false;
	bool symbolicLink = false;
};

struct FileFormat {
	QString mimeType;
	quint64 maximumBytes = 0;
	bool webp = false;
};

[[nodiscard]] quint8 U8(const QByteArray &bytes, quint64 offset) {
	return quint8(uchar(bytes.at(qsizetype(offset))));
}

[[nodiscard]] quint16 U16(const QByteArray &bytes, quint64 offset) {
	return quint16(U8(bytes, offset)) | (quint16(U8(bytes, offset + 1)) << 8);
}

[[nodiscard]] quint32 U32(const QByteArray &bytes, quint64 offset) {
	return quint32(U16(bytes, offset))
		   | (quint32(U16(bytes, offset + 2)) << 16);
}

[[nodiscard]] QString BaseName(QString name) {
	name.replace(QChar('\\'), QChar('/'));
	return name.mid(name.lastIndexOf(QChar('/')) + 1);
}

[[nodiscard]] std::optional<FileFormat> FormatForName(const QString &name) {
	const auto extension = QFileInfo(name).suffix().toLower();
	if (extension == u"webp"_q) {
		return FileFormat{
			.mimeType = u"image/webp"_q,
			.maximumBytes = 512 * 1024,
			.webp = true,
		};
	} else if (extension == u"tgs"_q) {
		return FileFormat{
			.mimeType = u"application/x-tgsticker"_q,
			.maximumBytes = 64 * 1024,
		};
	} else if (extension == u"webm"_q) {
		return FileFormat{
			.mimeType = u"video/webm"_q,
			.maximumBytes = 256 * 1024,
		};
	}
	return std::nullopt;
}

[[nodiscard]] int MaximumItems(StickerImportType type) {
	return (type == StickerImportType::Emoji) ? kMaximumEmojiCount
											  : kMaximumStickerCount;
}

void AddIssue(StickerImportScan &result, const QString &name,
			  StickerImportProblem problem) {
	result.issues.push_back({name, problem});
}

void Fail(StickerImportScan &result, StickerImportProblem problem) {
	result.files.clear();
	result.problem = problem;
}

[[nodiscard]] StickerImportProblem ValidateBytes(const QString &name,
												 const QByteArray &bytes,
												 StickerImportType type,
												 const FileFormat &format,
												 QSize *dimensions) {
	if (bytes.isEmpty()) {
		return StickerImportProblem::InvalidMedia;
	}
	if (format.webp) {
		auto buffer = QBuffer();
		buffer.setData(bytes);
		if (!buffer.open(QIODevice::ReadOnly)) {
			return StickerImportProblem::InvalidMedia;
		}
		auto reader = QImageReader(&buffer);
		reader.setFormat("webp");
		const auto size = reader.size();
		if (!reader.canRead() || size.isEmpty()) {
			return StickerImportProblem::InvalidMedia;
		}
		const auto valid
			= (type == StickerImportType::Emoji)
				  ? (size.width() == 100 && size.height() == 100)
				  : (size.width() <= 512 && size.height() <= 512
					 && (size.width() == 512 || size.height() == 512));
		if (!valid) {
			return StickerImportProblem::InvalidDimensions;
		}
		*dimensions = size;
	} else if (format.mimeType == u"application/x-tgsticker"_q) {
		if (!bytes.startsWith(QByteArray("\x1f\x8b", 2))) {
			return StickerImportProblem::InvalidMedia;
		}
	} else if (!bytes.startsWith(QByteArray("\x1a\x45\xdf\xa3", 4))) {
		return StickerImportProblem::InvalidMedia;
	}
	return StickerImportProblem::None;
}

void AddBytes(StickerImportScan &result, QString name, QByteArray bytes,
			  StickerImportType type, quint64 *totalBytes) {
	name = BaseName(std::move(name));
	const auto format = FormatForName(name);
	if (!format) {
		AddIssue(result, name, StickerImportProblem::UnsupportedFormat);
		return;
	} else if (quint64(bytes.size()) > format->maximumBytes) {
		AddIssue(result, name, StickerImportProblem::FileTooLarge);
		return;
	}
	auto dimensions = QSize();
	const auto problem = ValidateBytes(name, bytes, type, *format, &dimensions);
	if (problem != StickerImportProblem::None) {
		AddIssue(result, name, problem);
		return;
	} else if (int(result.files.size()) >= MaximumItems(type)) {
		AddIssue(result, name, StickerImportProblem::TooManyItems);
		return;
	} else if (quint64(bytes.size()) > kMaximumExpandedBytes - *totalBytes) {
		Fail(result, StickerImportProblem::TotalSizeLimit);
		return;
	}
	*totalBytes += quint64(bytes.size());
	result.files.push_back({
		.name = std::move(name),
		.bytes = std::move(bytes),
		.mimeType = format->mimeType,
		.dimensions = dimensions,
	});
}

[[nodiscard]] bool ReadFileBytes(const QString &path, quint64 maximumBytes,
								 QByteArray *bytes) {
	if (!bytes) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	*bytes = file.read(qsizetype(maximumBytes + 1));
	return file.error() == QFileDevice::NoError
		   && quint64(bytes->size()) <= maximumBytes && file.atEnd();
}

[[nodiscard]] bool InflateEntry(const QByteArray &archive,
								const ZipEntry &entry, QByteArray *bytes) {
	if (!bytes || entry.expandedSize > kMaximumEntryBytes
		|| entry.dataOffset > quint64(archive.size())
		|| entry.compressedSize > quint64(archive.size()) - entry.dataOffset) {
		return false;
	}
	if (entry.method == 0) {
		if (entry.compressedSize != entry.expandedSize) {
			return false;
		}
		*bytes = archive.mid(qsizetype(entry.dataOffset),
							 qsizetype(entry.compressedSize));
	} else if (entry.method == 8) {
		auto stream = z_stream();
		if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
			return false;
		}
		stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(
			archive.constData() + qsizetype(entry.dataOffset)));
		stream.avail_in = uInt(entry.compressedSize);
		bytes->clear();
		bytes->reserve(int(entry.expandedSize));
		auto output = std::array<Bytef, 16 * 1024>();
		auto status = Z_OK;
		while (status == Z_OK) {
			const auto remaining = entry.expandedSize - quint64(bytes->size());
			const auto capacity
				= std::min<quint64>(output.size(), remaining + 1);
			stream.next_out = output.data();
			stream.avail_out = uInt(capacity);
			status = inflate(&stream, Z_NO_FLUSH);
			const auto produced = capacity - stream.avail_out;
			if (produced > remaining) {
				status = Z_DATA_ERROR;
				break;
			}
			bytes->append(reinterpret_cast<const char *>(output.data()),
						  qsizetype(produced));
			if ((status == Z_OK) && !produced && !stream.avail_in) {
				status = Z_DATA_ERROR;
				break;
			}
		}
		const auto valid = status == Z_STREAM_END
						   && quint64(bytes->size()) == entry.expandedSize
						   && stream.total_in == entry.compressedSize;
		inflateEnd(&stream);
		if (!valid) {
			return false;
		}
	} else {
		return false;
	}
	const auto crc
		= crc32(0, reinterpret_cast<const Bytef *>(bytes->constData()),
				uInt(bytes->size()));
	return quint32(crc) == entry.crc;
}

[[nodiscard]] bool ParseZipDirectory(const QByteArray &archive,
									 quint64 *directoryOffset,
									 quint64 *directorySize,
									 quint16 *entryCount) {
	if (archive.size() < 22) {
		return false;
	}
	const auto size = quint64(archive.size());
	const auto searchStart = (size > 65557) ? size - 65557 : 0;
	const auto signature = QByteArray("PK\x05\x06", 4);
	auto endOffset = archive.lastIndexOf(signature, qsizetype(size - 22));
	while (endOffset >= qsizetype(searchStart)) {
		const auto offset = quint64(endOffset);
		if (offset + 22 <= size
			&& offset + 22 + U16(archive, offset + 20) == size) {
			break;
		}
		endOffset = archive.lastIndexOf(signature, endOffset - 1);
	}
	if (endOffset < qsizetype(searchStart)) {
		return false;
	}
	const auto offset = quint64(endOffset);
	if (U16(archive, offset + 4) != 0 || U16(archive, offset + 6) != 0
		|| U16(archive, offset + 8) != U16(archive, offset + 10)) {
		return false;
	}
	*entryCount = U16(archive, offset + 10);
	*directorySize = U32(archive, offset + 12);
	*directoryOffset = U32(archive, offset + 16);
	return (*directoryOffset <= size)
		   && (*directorySize <= size - *directoryOffset)
		   && (*directoryOffset + *directorySize == offset);
}

[[nodiscard]] bool ParseZipEntries(const QByteArray &archive,
								   quint64 directoryOffset,
								   quint64 directorySize, quint16 entryCount,
								   std::vector<ZipEntry> *entries,
								   quint64 *expandedBytes) {
	if (!entries || !expandedBytes) {
		return false;
	}
	entries->clear();
	entries->reserve(entryCount);
	const auto directoryEnd = directoryOffset + directorySize;
	auto offset = directoryOffset;
	*expandedBytes = 0;
	for (auto i = quint16(0); i != entryCount; ++i) {
		if (offset > directoryEnd || directoryEnd - offset < 46
			|| U32(archive, offset) != kCentralSignature) {
			return false;
		}
		const auto madeBy = U16(archive, offset + 4);
		const auto flags = U16(archive, offset + 8);
		const auto method = U16(archive, offset + 10);
		const auto crc = U32(archive, offset + 16);
		const auto compressedSize = U32(archive, offset + 20);
		const auto expandedSize = U32(archive, offset + 24);
		const auto nameSize = U16(archive, offset + 28);
		const auto extraSize = U16(archive, offset + 30);
		const auto commentSize = U16(archive, offset + 32);
		const auto disk = U16(archive, offset + 34);
		const auto externalAttributes = U32(archive, offset + 38);
		const auto localOffset = U32(archive, offset + 42);
		const auto recordSize
			= quint64(46) + nameSize + extraSize + commentSize;
		if (recordSize > directoryEnd - offset
			|| nameSize > kMaximumArchiveNameBytes || disk != 0
			|| compressedSize == std::numeric_limits<quint32>::max()
			|| expandedSize == std::numeric_limits<quint32>::max()
			|| localOffset == std::numeric_limits<quint32>::max()) {
			return false;
		}
		const auto nameBytes = archive.mid(qsizetype(offset + 46), nameSize);
		if (nameBytes.contains('\0')) {
			return false;
		}
		auto name = (flags & (1 << 11)) ? QString::fromUtf8(nameBytes)
										: QString::fromLatin1(nameBytes);
		const auto isDirectory = name.endsWith('/') || name.endsWith('\\');
		const auto host = quint8(madeBy >> 8);
		const auto mode = quint32(externalAttributes >> 16);
		const auto symbolicLink = (host == 3) && ((mode & 0170000) == 0120000);
		if (!isDirectory) {
			if (quint64(expandedSize)
				> kMaximumExpandedBytes - *expandedBytes) {
				*expandedBytes = kMaximumExpandedBytes + 1;
				return false;
			}
			*expandedBytes += expandedSize;
		}
		auto entry = ZipEntry{
			.name = std::move(name),
			.flags = flags,
			.method = method,
			.crc = crc,
			.compressedSize = compressedSize,
			.expandedSize = expandedSize,
			.localOffset = localOffset,
			.directory = isDirectory,
			.symbolicLink = symbolicLink,
		};
		if (!isDirectory) {
			const auto local = quint64(localOffset);
			if (local > directoryOffset || directoryOffset - local < 30
				|| U32(archive, local) != kLocalSignature) {
				return false;
			}
			const auto localFlags = U16(archive, local + 6);
			const auto localMethod = U16(archive, local + 8);
			const auto localNameSize = U16(archive, local + 26);
			const auto localExtraSize = U16(archive, local + 28);
			const auto dataOffset = local + 30 + localNameSize + localExtraSize;
			if (localFlags != flags || localMethod != method
				|| localNameSize != nameSize || dataOffset > directoryOffset
				|| quint64(compressedSize) > directoryOffset - dataOffset
				|| archive.mid(qsizetype(local + 30), localNameSize)
					   != nameBytes) {
				return false;
			}
			if (!(flags & (1 << 3))
				&& (U32(archive, local + 14) != crc
					|| U32(archive, local + 18) != compressedSize
					|| U32(archive, local + 22) != expandedSize)) {
				return false;
			}
			entry.dataOffset = dataOffset;
		}
		entries->push_back(std::move(entry));
		offset += recordSize;
	}
	return offset == directoryEnd;
}

void ReadZipEntries(const QByteArray &archive,
					const std::vector<ZipEntry> &entries,
					StickerImportType type, StickerImportScan *result) {
	auto totalBytes = quint64(0);
	for (const auto &entry : entries) {
		if (entry.directory) {
			continue;
		}
		const auto name = BaseName(entry.name);
		if (entry.symbolicLink) {
			AddIssue(*result, name, StickerImportProblem::SymbolicLink);
			continue;
		}
		const auto format = FormatForName(name);
		if (!format) {
			AddIssue(*result, name, StickerImportProblem::UnsupportedFormat);
			continue;
		} else if (entry.expandedSize > format->maximumBytes) {
			AddIssue(*result, name, StickerImportProblem::FileTooLarge);
			continue;
		} else if (entry.flags & 1) {
			AddIssue(*result, name, StickerImportProblem::InvalidMedia);
			continue;
		} else if ((entry.method != 0) && (entry.method != 8)) {
			AddIssue(*result, name, StickerImportProblem::InvalidMedia);
			continue;
		} else if (int(result->files.size()) >= MaximumItems(type)) {
			AddIssue(*result, name, StickerImportProblem::TooManyItems);
			continue;
		}
		auto bytes = QByteArray();
		if (!InflateEntry(archive, entry, &bytes)) {
			AddIssue(*result, name, StickerImportProblem::InvalidArchive);
			continue;
		}
		AddBytes(*result, name, std::move(bytes), type, &totalBytes);
		if (result->problem != StickerImportProblem::None) {
			return;
		}
	}
	if (result->files.empty()
		&& result->problem == StickerImportProblem::None) {
		result->problem = StickerImportProblem::NoFiles;
	}
}

} // namespace

StickerImportScan ScanStickerImportFolder(const QString &path,
										  StickerImportType type) {
	auto result = StickerImportScan();
	const auto root = QFileInfo(path);
	if (!root.isDir() || root.isSymLink()) {
		result.problem = StickerImportProblem::InvalidSource;
		return result;
	}
	auto entries = 0;
	auto totalBytes = quint64(0);
	const auto filters
		= QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
	auto iterator = QDirIterator(path, filters, QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		const auto entryPath = iterator.next();
		if (++entries > kMaximumEntries) {
			Fail(result, StickerImportProblem::TooManyEntries);
			return result;
		}
		const auto info = iterator.fileInfo();
		const auto name = info.fileName();
		if (info.isSymLink()) {
			AddIssue(result, name, StickerImportProblem::SymbolicLink);
			continue;
		} else if (!info.isFile()) {
			continue;
		}
		const auto format = FormatForName(name);
		if (!format) {
			AddIssue(result, name, StickerImportProblem::UnsupportedFormat);
			continue;
		} else if (info.size() < 0
				   || quint64(info.size()) > format->maximumBytes) {
			AddIssue(result, name, StickerImportProblem::FileTooLarge);
			continue;
		} else if (int(result.files.size()) >= MaximumItems(type)) {
			AddIssue(result, name, StickerImportProblem::TooManyItems);
			continue;
		}
		auto bytes = QByteArray();
		if (!ReadFileBytes(entryPath, format->maximumBytes, &bytes)) {
			AddIssue(result, name, StickerImportProblem::ReadFailed);
			continue;
		}
		AddBytes(result, name, std::move(bytes), type, &totalBytes);
		if (result.problem != StickerImportProblem::None) {
			return result;
		}
	}
	if (result.files.empty() && result.problem == StickerImportProblem::None) {
		result.problem = StickerImportProblem::NoFiles;
	}
	return result;
}

StickerImportScan ScanStickerImportZip(const QByteArray &bytes,
									   StickerImportType type) {
	auto result = StickerImportScan();
	if (quint64(bytes.size()) > kMaximumArchiveBytes) {
		result.problem = StickerImportProblem::TotalSizeLimit;
		return result;
	}
	auto directoryOffset = quint64(0);
	auto directorySize = quint64(0);
	auto entryCount = quint16(0);
	if (!ParseZipDirectory(bytes, &directoryOffset, &directorySize,
						   &entryCount)) {
		result.problem = StickerImportProblem::InvalidArchive;
		return result;
	} else if (entryCount > kMaximumEntries) {
		result.problem = StickerImportProblem::TooManyEntries;
		return result;
	}
	auto entries = std::vector<ZipEntry>();
	auto expandedBytes = quint64(0);
	if (!ParseZipEntries(bytes, directoryOffset, directorySize, entryCount,
						 &entries, &expandedBytes)) {
		result.problem = (expandedBytes > kMaximumExpandedBytes)
							 ? StickerImportProblem::TotalSizeLimit
							 : StickerImportProblem::InvalidArchive;
		return result;
	} else if (expandedBytes > kMaximumExpandedBytes) {
		result.problem = StickerImportProblem::TotalSizeLimit;
		return result;
	}
	ReadZipEntries(bytes, entries, type, &result);
	return result;
}

StickerImportScan ScanStickerImportZipFile(const QString &path,
										   StickerImportType type) {
	auto bytes = QByteArray();
	const auto info = QFileInfo(path);
	if (!info.isFile() || info.isSymLink() || info.size() < 0) {
		return {.problem = StickerImportProblem::InvalidSource};
	} else if (quint64(info.size()) > kMaximumArchiveBytes) {
		return {.problem = StickerImportProblem::TotalSizeLimit};
	} else if (!ReadFileBytes(path, kMaximumArchiveBytes, &bytes)) {
		return {.problem = StickerImportProblem::ReadFailed};
	}
	return ScanStickerImportZip(bytes, type);
}

QString StickerImportShareLinkPath(StickerImportType type,
								   const QString &shortName) {
	const auto prefix
		= (type == StickerImportType::Emoji) ? u"addemoji"_q : u"addstickers"_q;
	return prefix + '/' + shortName;
}

bool IsValidStickerSetShortName(const QString &name) {
	if (name.isEmpty() || name.size() > kMaximumShortNameLength
		|| !((name.front() >= 'a' && name.front() <= 'z')
			 || (name.front() >= 'A' && name.front() <= 'Z'))) {
		return false;
	}
	return std::all_of(name.cbegin(), name.cend(), [](QChar ch) {
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
			   || (ch >= '0' && ch <= '9') || ch == '_';
	});
}

bool IsValidStickerSetTitle(const QString &title) {
	const auto length = title.toUcs4().size();
	return (length > 0) && (length <= kMaximumStickerSetTitleLength);
}

} // namespace Core
