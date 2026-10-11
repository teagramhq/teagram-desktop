/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_archive.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryFile>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Core {
namespace {

constexpr auto kMaximumArchiveSize = quint64(512) * 1024 * 1024;
constexpr auto kMaximumExpandedSize = quint64(2) * 1024 * 1024 * 1024;
constexpr auto kMaximumEntryCount = 50'000;
constexpr auto kMaximumPathDepth = 128;
constexpr auto kMaximumSymlinkSize = 4096;
constexpr auto kZipEndSignature = quint32(0x06054b50);
constexpr auto kZipCentralSignature = quint32(0x02014b50);
constexpr auto kZipLocalSignature = quint32(0x04034b50);
constexpr auto kZipDescriptorSignature = quint32(0x08074b50);

enum class EntryType {
	Directory,
	Regular,
	Symlink,
};

enum class ParseStatus {
	Valid,
	Cancelled,
	LimitExceeded,
	Invalid,
};

struct ArchiveEntry {
	QByteArray rawName;
	std::vector<QString> components;
	EntryType type = EntryType::Regular;
	quint32 mode = 0;
	quint16 flags = 0;
	quint16 method = 0;
	quint32 crc = 0;
	quint32 compressedSize = 0;
	quint32 expandedSize = 0;
	quint32 localOffset = 0;
	quint64 dataOffset = 0;
	quint64 recordEnd = 0;
	QByteArray symlinkTarget;
};

struct DirectoryEntry {
	std::vector<QString> components;
	quint32 mode = 0755;
};

struct ParsedArchive {
	std::vector<ArchiveEntry> entries;
	std::map<QString, DirectoryEntry> directories;
	quint64 expandedSize = 0;
};

struct FileDescriptor final {
	FileDescriptor() = default;
	explicit FileDescriptor(int value) : value(value) {}
	~FileDescriptor() {
		if (value >= 0) {
			::close(value);
		}
	}
	FileDescriptor(FileDescriptor &&other) noexcept
		: value(std::exchange(other.value, -1)) {}
	FileDescriptor &operator=(FileDescriptor &&other) noexcept {
		if (this != &other) {
			if (value >= 0) {
				::close(value);
			}
			value = std::exchange(other.value, -1);
		}
		return *this;
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;

	int value = -1;
};

struct PathInterval {
	quint64 start = 0;
	quint64 end = 0;
};

[[nodiscard]] quint16 U16(const char *bytes) {
	const auto value = reinterpret_cast<const uchar *>(bytes);
	return quint16(value[0]) | (quint16(value[1]) << 8);
}

[[nodiscard]] quint32 U32(const char *bytes) {
	const auto value = reinterpret_cast<const uchar *>(bytes);
	return quint32(value[0])
		| (quint32(value[1]) << 8)
		| (quint32(value[2]) << 16)
		| (quint32(value[3]) << 24);
}

[[nodiscard]] bool Cancelled(const std::function<bool()> &isCancelled) {
	return isCancelled && isCancelled();
}

[[nodiscard]] bool ReadAt(QFile &file, quint64 offset, qsizetype size,
						  QByteArray *result) {
	if (!result || offset > quint64(file.size())
		|| quint64(size) > quint64(file.size()) - offset
		|| !file.seek(qint64(offset))) {
		return false;
	}
	*result = file.read(size);
	return result->size() == size;
}

[[nodiscard]] bool ValidUtf8(const QByteArray &value) {
	return !value.contains('\0') && QString::fromUtf8(value).toUtf8() == value;
}

[[nodiscard]] QString PathKey(const std::vector<QString> &components,
							  qsizetype count) {
	auto result = QString();
	for (auto i = qsizetype(0); i != count; ++i) {
		if (i) {
			result.append('/');
		}
		result.append(components[i]
					  .normalized(QString::NormalizationForm_D)
					  .toCaseFolded());
	}
	return result;
}

[[nodiscard]] QString RawPath(const std::vector<QString> &components,
							  qsizetype count) {
	auto result = QString();
	for (auto i = qsizetype(0); i != count; ++i) {
		if (i) {
			result.append('/');
		}
		result.append(components[i]);
	}
	return result;
}

[[nodiscard]] bool ParseName(const QByteArray &raw, bool directory,
							 std::vector<QString> *components) {
	if (!components || raw.isEmpty() || raw.startsWith('/')
		|| raw.contains('\\') || raw.contains(':') || !ValidUtf8(raw)) {
		return false;
	}
	auto name = raw;
	if (name.endsWith('/')) {
		if (!directory) {
			return false;
		}
		name.chop(1);
	}
	if (name.isEmpty()) {
		return false;
	}
	const auto parts = name.split('/');
	if (parts.size() > kMaximumPathDepth) {
		return false;
	}
	components->clear();
	components->reserve(parts.size());
	for (const auto &part : parts) {
		if (part.isEmpty() || part.size() > 255 || part == "."
			|| part == "..") {
			return false;
		}
		components->push_back(QString::fromUtf8(part));
	}
	return !components->empty() && components->front() == u"Teagram.app"_q;
}

[[nodiscard]] bool ValidateExtraFields(const QByteArray &extra) {
	for (auto offset = qsizetype(0); offset < extra.size();) {
		if (extra.size() - offset < 4) {
			return false;
		}
		const auto id = U16(extra.constData() + offset);
		const auto size = U16(extra.constData() + offset + 2);
		offset += 4;
		if (size > extra.size() - offset || id == 0x0001) {
			return false;
		}
		offset += size;
	}
	return true;
}

[[nodiscard]] bool AddEntryPaths(ParsedArchive *archive) {
	auto explicitPaths = std::set<QString>();
	auto rawPaths = std::map<QString, QString>();
	auto pathTypes = std::map<QString, EntryType>();
	for (const auto &entry : archive->entries) {
		const auto count = qsizetype(entry.components.size());
		const auto normalized = PathKey(entry.components, count);
		const auto raw = RawPath(entry.components, count);
		if (const auto i = rawPaths.find(normalized); i != rawPaths.end()) {
			if (i->second != raw) {
				return false;
			}
		} else {
			rawPaths.emplace(normalized, raw);
		}
		for (auto parentCount = qsizetype(1); parentCount < count;
			 parentCount++) {
			const auto key = PathKey(entry.components, parentCount);
			const auto parent = pathTypes.find(key);
			if (parent != pathTypes.end()
				&& parent->second != EntryType::Directory) {
				return false;
			}
			pathTypes.insert_or_assign(key, EntryType::Directory);
			const auto parentRaw = RawPath(entry.components, parentCount);
			if (const auto existing = rawPaths.find(key);
				existing != rawPaths.end() && existing->second != parentRaw) {
				return false;
			} else if (existing == rawPaths.end()) {
				rawPaths.emplace(key, parentRaw);
			}
			archive->directories.try_emplace(
				key,
				DirectoryEntry{
					std::vector<QString>(
						entry.components.begin(),
						entry.components.begin() + parentCount),
					0755,
				});
		}
		if (!explicitPaths.insert(normalized).second) {
			return false;
		}
		if (const auto existing = pathTypes.find(normalized);
			existing != pathTypes.end() && existing->second != entry.type) {
			return false;
		}
		if (const auto existing = rawPaths.find(normalized);
			existing != rawPaths.end() && existing->second != raw) {
			return false;
		}
		pathTypes.insert_or_assign(normalized, entry.type);
		if (entry.type == EntryType::Directory) {
			const auto mode = (entry.mode & 0777) | 0700;
			archive->directories.insert_or_assign(
				normalized,
				DirectoryEntry{ entry.components, mode });
		}
	}
	const auto root = PathKey({ u"Teagram.app" }, 1);
	return pathTypes.contains(root)
		   && pathTypes.at(root) == EntryType::Directory;
}

[[nodiscard]] ParseStatus ParseArchive(
		QFile &file,
		const std::function<bool()> &isCancelled,
		ParsedArchive *result) {
	if (!result || file.size() < 22
		|| quint64(file.size()) > kMaximumArchiveSize) {
		return ParseStatus::Invalid;
	}
	const auto tailSize = qsizetype(std::min<qint64>(
		file.size(),
		22 + 65535));
	auto tail = QByteArray();
	if (!ReadAt(file, quint64(file.size() - tailSize), tailSize, &tail)) {
		return ParseStatus::Invalid;
	}
	auto end = qsizetype(-1);
	for (auto i = tail.size() - 22; i >= 0; --i) {
		if (U32(tail.constData() + i) != kZipEndSignature) {
			continue;
		}
		const auto commentSize = U16(tail.constData() + i + 20);
		if (i + 22 + commentSize == tail.size()) {
			end = i;
			break;
		}
	}
	if (end < 0) {
		return ParseStatus::Invalid;
	}
	const auto *endRecord = tail.constData() + end;
	if (U16(endRecord + 4) != 0 || U16(endRecord + 6) != 0
		|| U16(endRecord + 8) != U16(endRecord + 10)) {
		return ParseStatus::Invalid;
	}
	const auto entryCount = U16(endRecord + 10);
	const auto centralSize = U32(endRecord + 12);
	const auto centralOffset = U32(endRecord + 16);
	const auto endOffset = quint64(file.size() - tailSize + end);
	if (entryCount == 0xffff || centralSize == 0xffffffff
		|| centralOffset == 0xffffffff || entryCount > kMaximumEntryCount
		|| quint64(centralOffset) + centralSize != endOffset) {
		return entryCount > kMaximumEntryCount
			? ParseStatus::LimitExceeded
			: ParseStatus::Invalid;
	}
	auto offset = quint64(centralOffset);
	auto totalExpanded = quint64(0);
	auto localOffsets = std::set<quint32>();
	result->entries.clear();
	result->entries.reserve(entryCount);
	for (auto index = 0; index != entryCount; ++index) {
		if (Cancelled(isCancelled)) {
			return ParseStatus::Cancelled;
		}
		auto header = QByteArray();
		if (!ReadAt(file, offset, 46, &header)
			|| U32(header.constData()) != kZipCentralSignature) {
			return ParseStatus::Invalid;
		}
		const auto madeBy = uchar(header[5]);
		const auto flags = U16(header.constData() + 8);
		const auto method = U16(header.constData() + 10);
		const auto crc = U32(header.constData() + 16);
		const auto compressedSize = U32(header.constData() + 20);
		const auto expandedSize = U32(header.constData() + 24);
		const auto nameSize = U16(header.constData() + 28);
		const auto extraSize = U16(header.constData() + 30);
		const auto commentSize = U16(header.constData() + 32);
		const auto disk = U16(header.constData() + 34);
		const auto external = U32(header.constData() + 38);
		const auto localOffset = U32(header.constData() + 42);
		const auto recordSize = quint64(46) + nameSize + extraSize + commentSize;
		if (quint64(centralOffset) + centralSize - offset < recordSize
			|| disk != 0 || compressedSize == 0xffffffff
			|| expandedSize == 0xffffffff || localOffset == 0xffffffff
			|| !localOffsets.insert(localOffset).second
			|| method != 0 && method != 8
			|| (flags & ~quint16(0x080e)) != 0
			|| ((flags & 0x0006) != 0 && method != 8)) {
			return ParseStatus::Invalid;
		}
		if (quint64(expandedSize) > kMaximumExpandedSize - totalExpanded) {
			return ParseStatus::LimitExceeded;
		}
		totalExpanded += expandedSize;
		auto details = QByteArray();
		if (!ReadAt(file, offset + 46, nameSize + extraSize + commentSize,
					&details)) {
			return ParseStatus::Invalid;
		}
		const auto rawName = details.first(nameSize);
		const auto extra = details.mid(nameSize, extraSize);
		if (!ValidateExtraFields(extra)) {
			return ParseStatus::Invalid;
		}
		const auto trailingSlash = rawName.endsWith('/');
		const auto creatorIsUnix = madeBy == 3;
		const auto unixMode = creatorIsUnix ? (external >> 16) & 0xffff : 0;
		const auto fileType = unixMode & S_IFMT;
		auto type = EntryType::Regular;
		if (fileType == S_IFLNK) {
			type = EntryType::Symlink;
		} else if (fileType == S_IFDIR || trailingSlash
				   || (!creatorIsUnix && (external & 0x10))) {
			type = EntryType::Directory;
		} else if (fileType != 0 && fileType != S_IFREG) {
			return ParseStatus::Invalid;
		}
		if ((type == EntryType::Directory) != trailingSlash && trailingSlash) {
			return ParseStatus::Invalid;
		}
		if (type == EntryType::Directory
			&& (compressedSize != 0 || expandedSize != 0)) {
			return ParseStatus::Invalid;
		}
		if (type == EntryType::Symlink
			&& (expandedSize == 0 || expandedSize > kMaximumSymlinkSize
				|| compressedSize > kMaximumSymlinkSize)) {
			return ParseStatus::Invalid;
		}
		if ((flags & 0x0800) == 0
			&& std::any_of(rawName.cbegin(), rawName.cend(), [](char value) {
				return uchar(value) >= 0x80;
			})) {
			return ParseStatus::Invalid;
		}
		auto entry = ArchiveEntry();
		entry.rawName = rawName;
		entry.type = type;
		entry.mode = unixMode & 0777;
		if (!entry.mode && type == EntryType::Directory) {
			entry.mode = 0755;
		} else if (!entry.mode && type == EntryType::Regular) {
			entry.mode = 0644;
		}
		entry.flags = flags;
		entry.method = method;
		entry.crc = crc;
		entry.compressedSize = compressedSize;
		entry.expandedSize = expandedSize;
		entry.localOffset = localOffset;
		if (!ParseName(rawName, type == EntryType::Directory,
					   &entry.components)) {
			return ParseStatus::Invalid;
		}
		result->entries.push_back(std::move(entry));
		offset += recordSize;
	}
	if (offset != quint64(centralOffset) + centralSize
		|| !AddEntryPaths(result)) {
		return ParseStatus::Invalid;
	}
	auto intervals = std::vector<PathInterval>();
	intervals.reserve(result->entries.size());
	for (auto &entry : result->entries) {
		if (Cancelled(isCancelled)) {
			return ParseStatus::Cancelled;
		}
		auto local = QByteArray();
		if (!ReadAt(file, entry.localOffset, 30, &local)
			|| U32(local.constData()) != kZipLocalSignature
			|| U16(local.constData() + 6) != entry.flags
			|| U16(local.constData() + 8) != entry.method) {
			return ParseStatus::Invalid;
		}
		const auto nameSize = U16(local.constData() + 26);
		const auto extraSize = U16(local.constData() + 28);
		auto details = QByteArray();
		if (!ReadAt(file, quint64(entry.localOffset) + 30,
					nameSize + extraSize, &details)
			|| details.first(nameSize) != entry.rawName
			|| !ValidateExtraFields(details.mid(nameSize, extraSize))) {
			return ParseStatus::Invalid;
		}
		const auto localCrc = U32(local.constData() + 14);
		const auto localCompressedSize = U32(local.constData() + 18);
		const auto localExpandedSize = U32(local.constData() + 22);
		if ((entry.flags & 0x0008) == 0
			&& (localCrc != entry.crc
				|| localCompressedSize != entry.compressedSize
				|| localExpandedSize != entry.expandedSize)) {
			return ParseStatus::Invalid;
		}
		entry.dataOffset = quint64(entry.localOffset) + 30 + nameSize
			+ extraSize;
		entry.recordEnd = entry.dataOffset + entry.compressedSize;
		if (entry.recordEnd > centralOffset) {
			return ParseStatus::Invalid;
		}
		if (entry.flags & 0x0008) {
			auto descriptor = QByteArray();
			if (!ReadAt(file, entry.recordEnd, 4, &descriptor)) {
				return ParseStatus::Invalid;
			}
			const auto hasSignature
				= U32(descriptor.constData()) == kZipDescriptorSignature;
			auto values = QByteArray();
			const auto valuesOffset = entry.recordEnd + (hasSignature ? 4 : 0);
			if (!ReadAt(file, valuesOffset, 12, &values)
				|| U32(values.constData()) != entry.crc
				|| U32(values.constData() + 4) != entry.compressedSize
				|| U32(values.constData() + 8) != entry.expandedSize) {
				return ParseStatus::Invalid;
			}
			entry.recordEnd = valuesOffset + 12;
		}
		if (entry.recordEnd > centralOffset) {
			return ParseStatus::Invalid;
		}
		intervals.push_back({ entry.localOffset, entry.recordEnd });
	}
	std::sort(intervals.begin(), intervals.end(), [](const auto &left,
												 const auto &right) {
		return left.start < right.start;
	});
	for (auto i = size_t(1); i < intervals.size(); ++i) {
		if (intervals[i].start < intervals[i - 1].end) {
			return ParseStatus::Invalid;
		}
	}
	result->expandedSize = totalExpanded;
	return ParseStatus::Valid;
}

[[nodiscard]] bool ValidateSymlinkTarget(const ArchiveEntry &entry,
										 const QByteArray &target) {
	if (target.isEmpty() || target.startsWith('/') || target.contains('\\')
		|| target.contains(':') || !ValidUtf8(target)) {
		return false;
	}
	auto depth = qsizetype(entry.components.size()) - 2;
	for (const auto &component : target.split('/')) {
		if (component.isEmpty() || component == ".") {
			continue;
		}
		if (component == "..") {
			if (depth == 0) {
				return false;
			}
			--depth;
		} else {
			++depth;
		}
	}
	return depth > 0;
}

using SymlinkMap = std::map<QString, const ArchiveEntry *>;

[[nodiscard]] bool ResolveSymlinkComponents(
		const QByteArray &target,
		const SymlinkMap &symlinks,
		std::vector<QString> *resolved,
		std::set<QString> *active,
		int depth) {
	if (!resolved || !active || depth > kMaximumPathDepth) {
		return false;
	}
	for (const auto &part : target.split('/')) {
		if (part.isEmpty() || part == ".") {
			continue;
		} else if (part == "..") {
			if (resolved->size() <= 1) {
				return false;
			}
			resolved->pop_back();
			continue;
		}
		if (part.size() > 255 || resolved->size() >= kMaximumPathDepth) {
			return false;
		}
		resolved->push_back(QString::fromUtf8(part));
		const auto key = PathKey(*resolved, qsizetype(resolved->size()));
		const auto link = symlinks.find(key);
		if (link == symlinks.end()) {
			continue;
		}
		if (!active->insert(key).second) {
			return false;
		}
		resolved->pop_back();
		const auto valid = ResolveSymlinkComponents(
			link->second->symlinkTarget,
			symlinks,
			resolved,
			active,
			depth + 1);
		active->erase(key);
		if (!valid) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidateSymlinkGraph(const ParsedArchive &archive) {
	auto symlinks = SymlinkMap();
	for (const auto &entry : archive.entries) {
		if (entry.type == EntryType::Symlink) {
			symlinks.emplace(
				PathKey(entry.components, qsizetype(entry.components.size())),
				&entry);
		}
	}
	for (const auto &entry : archive.entries) {
		if (entry.type != EntryType::Symlink) {
			continue;
		}
		auto resolved = std::vector<QString>(
			entry.components.begin(), entry.components.end() - 1);
		auto active = std::set<QString>();
		const auto key = PathKey(
			entry.components, qsizetype(entry.components.size()));
		active.insert(key);
		if (!ResolveSymlinkComponents(
				entry.symlinkTarget, symlinks, &resolved, &active, 0)
			|| resolved.size() <= 1) {
			return false;
		}
	}
	return true;
}

template <typename Consumer>
[[nodiscard]] bool ReadEntry(QFile &file, const ArchiveEntry &entry,
							 Consumer &&consume,
							 const std::function<bool()> &isCancelled) {
	if (!file.seek(qint64(entry.dataOffset))) {
		return false;
	}
	if (entry.method == 0 && entry.compressedSize != entry.expandedSize) {
		return false;
	}
	auto crc = crc32(0L, Z_NULL, 0);
	auto expanded = quint64(0);
	auto compressedRemaining = quint64(entry.compressedSize);
	auto input = std::array<Bytef, 64 * 1024>();
	auto output = std::array<Bytef, 64 * 1024>();
	auto stream = z_stream();
	if (entry.method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
		return false;
	}
	auto streamFinished = entry.method == 0;
	auto valid = true;
	while (valid && (compressedRemaining || !streamFinished)) {
		if (Cancelled(isCancelled)) {
			valid = false;
			break;
		}
		if (entry.method == 8 && stream.avail_in == 0 && compressedRemaining) {
			const auto readSize = qsizetype(std::min<quint64>(
				compressedRemaining,
				input.size()));
			const auto read = file.read(
				reinterpret_cast<char *>(input.data()), readSize);
			if (read != readSize) {
				valid = false;
				break;
			}
			compressedRemaining -= quint64(readSize);
			stream.next_in = input.data();
			stream.avail_in = uInt(readSize);
		}
		if (entry.method == 0) {
			const auto readSize = qsizetype(std::min<quint64>(
				compressedRemaining,
				input.size()));
			const auto read = file.read(
				reinterpret_cast<char *>(input.data()), readSize);
			if (read != readSize) {
				valid = false;
				break;
			}
			compressedRemaining -= quint64(readSize);
			if (readSize == 0) {
				streamFinished = true;
				continue;
			}
			const auto bytes = QByteArrayView(
				reinterpret_cast<const char *>(input.data()), readSize);
			if (quint64(bytes.size()) > entry.expandedSize - expanded
				|| !consume(bytes)) {
				valid = false;
				break;
			}
			crc = crc32(crc, input.data(), uInt(readSize));
			expanded += quint64(readSize);
			continue;
		}
		stream.next_out = output.data();
		stream.avail_out = uInt(output.size());
		const auto status = inflate(&stream, Z_NO_FLUSH);
		const auto produced = output.size() - stream.avail_out;
		if (produced) {
			const auto bytes = QByteArrayView(
				reinterpret_cast<const char *>(output.data()),
				qsizetype(produced));
			if (quint64(bytes.size()) > entry.expandedSize - expanded
				|| !consume(bytes)) {
				valid = false;
				break;
			}
			crc = crc32(crc, output.data(), uInt(produced));
			expanded += quint64(produced);
		}
		if (status == Z_STREAM_END) {
			if (compressedRemaining || stream.avail_in) {
				valid = false;
				break;
			}
			streamFinished = true;
		} else if (status != Z_OK || (produced == 0 && stream.avail_in == 0
										 && !compressedRemaining)) {
			valid = false;
		}
	}
	if (entry.method == 8) {
		valid = (inflateEnd(&stream) == Z_OK) && valid;
	}
	return valid && streamFinished && !compressedRemaining
		   && expanded == entry.expandedSize && quint32(crc) == entry.crc;
}

[[nodiscard]] bool OpenChildDirectory(int parent, const QString &name,
									 bool create, FileDescriptor *result) {
	const auto bytes = QFile::encodeName(name);
	if (bytes.isEmpty()) {
		return false;
	}
	if (create && ::mkdirat(parent, bytes.constData(), 0700) != 0
		&& errno != EEXIST) {
		return false;
	}
	const auto opened = ::openat(
		parent,
		bytes.constData(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (opened < 0) {
		return false;
	}
	auto info = struct stat();
	if (::fstat(opened, &info) != 0 || !S_ISDIR(info.st_mode)
		|| info.st_uid != ::geteuid()) {
		::close(opened);
		return false;
	}
	*result = FileDescriptor(opened);
	return true;
}

[[nodiscard]] bool OpenDirectoryPath(int root,
									 const std::vector<QString> &components,
									 qsizetype count, bool create,
									 FileDescriptor *result) {
	if (count < 0 || count + 1 > qsizetype(components.size())) {
		return false;
	}
	auto current = FileDescriptor(::dup(root));
	if (current.value < 0) {
		return false;
	}
	for (auto i = qsizetype(0); i != count; ++i) {
		auto next = FileDescriptor();
		if (!OpenChildDirectory(
				current.value, components[i + 1], create, &next)) {
			return false;
		}
		current = std::move(next);
	}
	*result = std::move(current);
	return true;
}

[[nodiscard]] bool EmptyDirectory(int directory) {
	auto duplicate = ::dup(directory);
	if (duplicate < 0) {
		return false;
	}
	auto stream = ::fdopendir(duplicate);
	if (!stream) {
		::close(duplicate);
		return false;
	}
	::rewinddir(stream);
	auto empty = true;
	while (const auto entry = ::readdir(stream)) {
		const auto name = QByteArray(entry->d_name);
		if (name != "." && name != "..") {
			empty = false;
			break;
		}
	}
	::closedir(stream);
	return empty;
}

[[nodiscard]] bool RemoveTreeAt(int parent, const char *name, int depth = 0) {
	if (depth > 256) {
		return false;
	}
	auto info = struct stat();
	if (::fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT;
	}
	if (!S_ISDIR(info.st_mode)) {
		return ::unlinkat(parent, name, 0) == 0;
	}
	const auto opened = ::openat(
		parent,
		name,
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (opened < 0) {
		return false;
	}
	auto directory = FileDescriptor(opened);
	auto duplicate = ::dup(opened);
	if (duplicate < 0) {
		return false;
	}
	auto stream = ::fdopendir(duplicate);
	if (!stream) {
		::close(duplicate);
		return false;
	}
	auto removed = true;
	while (const auto entry = ::readdir(stream)) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		if (!RemoveTreeAt(opened, entry->d_name, depth + 1)) {
			removed = false;
			break;
		}
	}
	::closedir(stream);
	return removed && ::unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

[[nodiscard]] bool WriteAll(int file, QByteArrayView bytes) {
	for (auto offset = qsizetype(0); offset < bytes.size();) {
		const auto written = ::write(
			file,
			bytes.data() + offset,
			size_t(bytes.size() - offset));
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return false;
		}
		offset += qsizetype(written);
	}
	return true;
}

[[nodiscard]] bool ReadAndValidateSymlinks(
		QFile &file,
		ParsedArchive *archive,
		const std::function<bool()> &isCancelled) {
	for (auto &entry : archive->entries) {
		if (entry.type != EntryType::Symlink) {
			continue;
		}
		if (!ReadEntry(file, entry, [&](QByteArrayView bytes) {
					entry.symlinkTarget.append(bytes.data(), bytes.size());
					return entry.symlinkTarget.size() <= kMaximumSymlinkSize;
				},
				isCancelled)
			|| !ValidateSymlinkTarget(entry, entry.symlinkTarget)) {
			return false;
		}
	}
	return ValidateSymlinkGraph(*archive);
}

[[nodiscard]] TeagramUpdateArchiveResult Rejected(
		TeagramUpdateArchiveReason reason) {
	return { reason, {} };
}

} // namespace

TeagramUpdateArchiveResult ExtractAuthenticatedTeagramUpdateArchive(
	const TeagramUpdateCandidate &candidate,
	const QString &privateStagingDirectory,
	const std::function<bool()> &isCancelled,
	const std::function<void(int)> &progress) {
	if (Cancelled(isCancelled)) {
		return Rejected(TeagramUpdateArchiveReason::Cancelled);
	}
	const auto stageInfo = QFileInfo(privateStagingDirectory);
	const auto canonicalStage = stageInfo.canonicalFilePath();
	if (!stageInfo.isDir() || stageInfo.isSymLink() || canonicalStage.isEmpty()
		|| QDir::cleanPath(canonicalStage)
			!= QDir::cleanPath(stageInfo.absoluteFilePath())) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	const auto stagePath = QFile::encodeName(canonicalStage);
	auto stage = FileDescriptor(::open(
		stagePath.constData(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
	if (stage.value < 0) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	auto stageInfoStat = struct stat();
	if (::fstat(stage.value, &stageInfoStat) != 0
		|| !S_ISDIR(stageInfoStat.st_mode)
		|| stageInfoStat.st_uid != ::geteuid()
		|| (stageInfoStat.st_mode & 0077) != 0
		|| !EmptyDirectory(stage.value)) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	if (Cancelled(isCancelled)) {
		return Rejected(TeagramUpdateArchiveReason::Cancelled);
	}
	const auto sourcePath = QFile::encodeName(candidate.archivePath());
	auto sourceHandle = FileDescriptor(::open(
		sourcePath.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (sourceHandle.value < 0) {
		return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
	}
	auto sourceInfo = struct stat();
	if (::fstat(sourceHandle.value, &sourceInfo) != 0
		|| !S_ISREG(sourceInfo.st_mode)
		|| sourceInfo.st_uid != ::geteuid()
		|| sourceInfo.st_nlink != 1
		|| (sourceInfo.st_mode & 0077) != 0
		|| sourceInfo.st_size < 0
		|| quint64(sourceInfo.st_size) != candidate.metadata().assetSize()
		|| quint64(sourceInfo.st_size) > kMaximumArchiveSize) {
		return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
	}
	auto source = QFile();
	if (!source.open(
			sourceHandle.value,
			QIODevice::ReadOnly,
			QFileDevice::AutoCloseHandle)) {
		return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
	}
	sourceHandle.value = -1;
	auto snapshot = QTemporaryFile(
		QDir(canonicalStage).filePath(u".archive-XXXXXX"_q));
	if (!snapshot.open()) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	const auto snapshotPath = QFile::encodeName(snapshot.fileName());
	if (::unlink(snapshotPath.constData()) != 0) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	auto copied = quint64(0);
	auto buffer = QByteArray(64 * 1024, Qt::Uninitialized);
	while (copied < candidate.metadata().assetSize()) {
		if (Cancelled(isCancelled)) {
			return Rejected(TeagramUpdateArchiveReason::Cancelled);
		}
		const auto readSize = qsizetype(std::min<quint64>(
			candidate.metadata().assetSize() - copied,
			quint64(buffer.size())));
		const auto read = source.read(buffer.data(), readSize);
		if (read <= 0 || snapshot.write(buffer.constData(), read) != read) {
			return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
		}
		copied += quint64(read);
	}
	if (copied != candidate.metadata().assetSize()
		|| quint64(snapshot.size()) != candidate.metadata().assetSize()
		|| !snapshot.flush()
		|| !snapshot.seek(0)) {
		return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
	}
	const auto verified = VerifyAuthenticatedTeagramUpdateArchive(
		candidate.metadata(), snapshot, isCancelled);
	if (!verified.eligible()) {
		return Rejected(verified.reason
							== TeagramUpdateVerificationReason::Cancelled
			? TeagramUpdateArchiveReason::Cancelled
			: TeagramUpdateArchiveReason::ArchiveRejected);
	}
	auto parsed = ParsedArchive();
	const auto parseStatus = ParseArchive(snapshot, isCancelled, &parsed);
	if (parseStatus == ParseStatus::Cancelled) {
		return Rejected(TeagramUpdateArchiveReason::Cancelled);
	} else if (parseStatus == ParseStatus::LimitExceeded) {
		return Rejected(TeagramUpdateArchiveReason::LimitExceeded);
	} else if (parseStatus != ParseStatus::Valid) {
		return Rejected(TeagramUpdateArchiveReason::ArchiveRejected);
	}
	if (!ReadAndValidateSymlinks(snapshot, &parsed, isCancelled)) {
		return Rejected(Cancelled(isCancelled)
			? TeagramUpdateArchiveReason::Cancelled
			: TeagramUpdateArchiveReason::ArchiveRejected);
	}
	if (!EmptyDirectory(stage.value)) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	if (::mkdirat(stage.value, "Teagram.app", 0700) != 0) {
		return Rejected(TeagramUpdateArchiveReason::StagingFailure);
	}
	auto app = FileDescriptor();
	if (!OpenChildDirectory(stage.value, u"Teagram.app"_q, false, &app)) {
		return Rejected(RemoveTreeAt(stage.value, "Teagram.app")
			? TeagramUpdateArchiveReason::StagingFailure
			: TeagramUpdateArchiveReason::WriteFailure);
	}
	auto created = true;
	for (const auto &item : parsed.directories) {
		const auto &directory = item.second;
		if (directory.components.size() <= 1) {
			continue;
		}
		FileDescriptor opened;
		if (!OpenDirectoryPath(
				app.value,
				directory.components,
				qsizetype(directory.components.size()) - 1,
				true,
				&opened)) {
			created = false;
			break;
		}
	}
	if (!created) {
		app = FileDescriptor();
		return Rejected(RemoveTreeAt(stage.value, "Teagram.app")
			? TeagramUpdateArchiveReason::WriteFailure
			: TeagramUpdateArchiveReason::StagingFailure);
	}
	auto extracted = quint64(0);
	for (const auto &entry : parsed.entries) {
		if (entry.type != EntryType::Regular) {
			continue;
		}
		if (Cancelled(isCancelled)) {
			created = false;
			break;
		}
		const auto parts = qsizetype(entry.components.size());
		FileDescriptor parent;
		if (!OpenDirectoryPath(app.value, entry.components, parts - 2,
								false, &parent)) {
			created = false;
			break;
		}
		const auto name = QFile::encodeName(entry.components.back());
		const auto output = ::openat(
			parent.value,
			name.constData(),
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
			0600);
		if (output < 0) {
			created = false;
			break;
		}
		auto destination = FileDescriptor(output);
		auto info = struct stat();
		if (::fstat(destination.value, &info) != 0 || !S_ISREG(info.st_mode)
			|| info.st_uid != ::geteuid() || info.st_nlink != 1) {
			created = false;
			break;
		}
		if (!ReadEntry(file, entry, [&](QByteArrayView bytes) {
					if (!WriteAll(destination.value, bytes)) {
						return false;
					}
					extracted += quint64(bytes.size());
					if (progress && parsed.expandedSize) {
						progress(int((100 * extracted) / parsed.expandedSize));
					}
					return !Cancelled(isCancelled);
				},
				isCancelled)
			|| ::fchmod(destination.value, entry.mode & 0777) != 0
			|| ::fsync(destination.value) != 0) {
			created = false;
			break;
		}
	}
	if (created) {
		for (const auto &entry : parsed.entries) {
			if (entry.type != EntryType::Symlink) {
				continue;
			}
			const auto parts = qsizetype(entry.components.size());
			FileDescriptor parent;
			if (!OpenDirectoryPath(app.value, entry.components, parts - 2,
									false, &parent)) {
				created = false;
				break;
			}
			const auto name = QFile::encodeName(entry.components.back());
			if (::symlinkat(
					entry.symlinkTarget.constData(),
					parent.value,
					name.constData()) != 0) {
				created = false;
				break;
			}
		}
	}
	if (created) {
		auto directories = std::vector<std::pair<QString, DirectoryEntry>>(
			parsed.directories.begin(), parsed.directories.end());
		std::sort(directories.begin(), directories.end(), [](const auto &left,
														 const auto &right) {
			return left.second.components.size()
				> right.second.components.size();
		});
		for (const auto &item : directories) {
			const auto &directory = item.second;
			const auto count = qsizetype(directory.components.size());
			if (count == 1) {
				if (::fchmod(app.value, directory.mode & 0777) != 0
					|| ::fsync(app.value) != 0) {
					created = false;
					break;
				}
				continue;
			}
			FileDescriptor opened;
			if (!OpenDirectoryPath(app.value, directory.components,
								   count - 1, false, &opened)
				|| ::fchmod(opened.value, directory.mode & 0777) != 0
				|| ::fsync(opened.value) != 0) {
				created = false;
				break;
			}
		}
	}
	if (Cancelled(isCancelled)) {
		created = false;
	}
	if (!created) {
		app = FileDescriptor();
		const auto removed = RemoveTreeAt(stage.value, "Teagram.app");
		return Rejected(removed && Cancelled(isCancelled)
			? TeagramUpdateArchiveReason::Cancelled
			: removed ? TeagramUpdateArchiveReason::WriteFailure
					   : TeagramUpdateArchiveReason::StagingFailure);
	}
	if (progress) {
		progress(100);
	}
	return {
		TeagramUpdateArchiveReason::Extracted,
		QDir(canonicalStage).filePath(u"Teagram.app"_q),
	};
}

} // namespace Core
