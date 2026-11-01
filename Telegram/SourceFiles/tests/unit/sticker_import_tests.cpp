/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/sticker_import.h"

#include <zlib.h>

#include <vector>

namespace {

struct ZipEntry {
	QByteArray name;
	QByteArray data;
	quint32 externalAttributes = 0;
	quint16 method = 0;
};

void Append16(QByteArray &bytes, quint16 value) {
	bytes.append(char(value & 0xFF));
	bytes.append(char(value >> 8));
}

void Append32(QByteArray &bytes, quint32 value) {
	Append16(bytes, quint16(value & quint32(0xFFFF)));
	Append16(bytes, quint16(value >> 16));
}

[[nodiscard]] QByteArray DeflateRaw(const QByteArray &data) {
	auto stream = z_stream();
	if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
					 Z_DEFAULT_STRATEGY)
		!= Z_OK) {
		return QByteArray();
	}
	auto result
		= QByteArray(int(deflateBound(&stream, uLong(data.size()))), '\0');
	stream.next_in
		= reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
	stream.avail_in = uInt(data.size());
	stream.next_out = reinterpret_cast<Bytef *>(result.data());
	stream.avail_out = uInt(result.size());
	const auto status = deflate(&stream, Z_FINISH);
	result.resize(qsizetype(stream.total_out));
	deflateEnd(&stream);
	return (status == Z_STREAM_END) ? result : QByteArray();
}

[[nodiscard]] QByteArray MakeZip(const std::vector<ZipEntry> &entries) {
	auto archive = QByteArray();
	auto directory = QByteArray();
	for (const auto &entry : entries) {
		const auto compressed
			= (entry.method == 8) ? DeflateRaw(entry.data) : entry.data;
		const auto crc
			= crc32(0, reinterpret_cast<const Bytef *>(entry.data.constData()),
					uInt(entry.data.size()));
		const auto offset = quint32(archive.size());
		Append32(archive, 0x04034B50);
		Append16(archive, 20);
		Append16(archive, 0);
		Append16(archive, entry.method);
		Append16(archive, 0);
		Append16(archive, 0);
		Append32(archive, quint32(crc));
		Append32(archive, quint32(compressed.size()));
		Append32(archive, quint32(entry.data.size()));
		Append16(archive, quint16(entry.name.size()));
		Append16(archive, 0);
		archive.append(entry.name);
		archive.append(compressed);

		Append32(directory, 0x02014B50);
		Append16(directory, 0x0314);
		Append16(directory, 20);
		Append16(directory, 0);
		Append16(directory, entry.method);
		Append16(directory, 0);
		Append16(directory, 0);
		Append32(directory, quint32(crc));
		Append32(directory, quint32(compressed.size()));
		Append32(directory, quint32(entry.data.size()));
		Append16(directory, quint16(entry.name.size()));
		Append16(directory, 0);
		Append16(directory, 0);
		Append16(directory, 0);
		Append16(directory, 0);
		Append32(directory, entry.externalAttributes);
		Append32(directory, offset);
		directory.append(entry.name);
	}
	const auto directoryOffset = quint32(archive.size());
	archive.append(directory);
	Append32(archive, 0x06054B50);
	Append16(archive, 0);
	Append16(archive, 0);
	Append16(archive, quint16(entries.size()));
	Append16(archive, quint16(entries.size()));
	Append32(archive, quint32(directory.size()));
	Append32(archive, directoryOffset);
	Append16(archive, 0);
	return archive;
}

TEST_CASE(StickerImportZipReadsUnsafeNamesOnlyAsLabels) {
	const auto zip = MakeZip(
		{{"../../outside.tgs", QByteArray("\x1f\x8b", 2) + "tgs-bytes"}});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK_EQ(result.files.size(), 1);
	if (!result.files.empty()) {
		CHECK_EQ(result.files.front().name, u"outside.tgs"_q);
		CHECK_EQ(result.files.front().bytes,
				 QByteArray("\x1f\x8b", 2) + "tgs-bytes");
	}
}

TEST_CASE(StickerImportZipInflatesDeflatedEntries) {
	const auto zip = MakeZip(
		{{"compressed.tgs", QByteArray("\x1f\x8b", 2) + "tgs-bytes", 0, 8}});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK_EQ(result.files.size(), 1);
	if (!result.files.empty()) {
		CHECK_EQ(result.files.front().bytes,
				 QByteArray("\x1f\x8b", 2) + "tgs-bytes");
	}
}

TEST_CASE(StickerImportZipSkipsSymlinkEntries) {
	const auto mode = quint32(0120000 | 0777) << 16;
	const auto zip = MakeZip(
		{{"linked.tgs", QByteArray("\x1f\x8b", 2) + "tgs-bytes", mode}});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.files.empty());
	CHECK_EQ(result.issues.size(), 1);
	if (!result.issues.empty()) {
		CHECK(result.issues.front().problem
			  == Core::StickerImportProblem::SymbolicLink);
	}
}

TEST_CASE(StickerImportZipRejectsEntryCountAboveLimit) {
	auto entries = std::vector<ZipEntry>();
	for (auto i = 0; i != 202; ++i) {
		entries.push_back({QByteArray("item") + QByteArray::number(i) + ".tgs",
						   QByteArray("\x1f\x8b", 2) + "tgs-bytes"});
	}
	const auto zip = MakeZip(entries);
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::TooManyEntries);
	CHECK(result.files.empty());
}

TEST_CASE(StickerImportShortNamesMatchServerRules) {
	CHECK(Core::IsValidStickerSetShortName(u"Cats_2"_q));
	CHECK(Core::IsValidStickerSetShortName(u"a"_q));
	CHECK(!Core::IsValidStickerSetShortName(u"2cats"_q));
	CHECK(!Core::IsValidStickerSetShortName(u"cats.pack"_q));
	CHECK(!Core::IsValidStickerSetShortName(QString(65, 'a')));
}

} // namespace
