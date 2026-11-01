/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/local_url_conversion.h"
#include "core/sticker_import.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtGui/QImageWriter>

#include <zlib.h>

#include <vector>

namespace {

struct ZipEntry {
	QByteArray name;
	QByteArray data;
	quint32 externalAttributes = 0;
	quint16 method = 0;
	quint32 advertisedExpandedSize = 0;
	bool localCrcMismatch = false;
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

[[nodiscard]] QByteArray Gzip(const QByteArray &data) {
	auto stream = z_stream();
	if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
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

[[nodiscard]] QByteArray ValidStickerTGS() {
	return Gzip(R"({"tgs":1,"w":512,"h":512,"fr":30,"ip":0,"op":60})");
}

[[nodiscard]] QByteArray WebpImage(QSize size) {
	auto image = QImage(size, QImage::Format_ARGB32);
	image.fill(Qt::transparent);
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly)) {
		return {};
	}
	auto writer = QImageWriter(&buffer, "webp");
	return writer.write(image) ? result : QByteArray();
}

[[nodiscard]] QByteArray MakeZip(const std::vector<ZipEntry> &entries) {
	auto archive = QByteArray();
	auto directory = QByteArray();
	for (const auto &entry : entries) {
		const auto compressed
			= (entry.method == 8) ? DeflateRaw(entry.data) : entry.data;
		const auto expandedSize = entry.advertisedExpandedSize
									  ? entry.advertisedExpandedSize
									  : quint32(entry.data.size());
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
		Append32(archive, quint32(crc) ^ (entry.localCrcMismatch ? 1U : 0U));
		Append32(archive, quint32(compressed.size()));
		Append32(archive, expandedSize);
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
		Append32(directory, expandedSize);
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

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

TEST_CASE(StickerImportZipReadsUnsafeNamesOnlyAsLabels) {
	const auto zip = MakeZip(
		{{"../../outside.tgs", QByteArray("\x1f\x8b", 2) + "tgs-bytes"}});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK_EQ(int(result.files.size()), 1);
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
	CHECK_EQ(int(result.files.size()), 1);
	if (!result.files.empty()) {
		CHECK_EQ(result.files.front().bytes,
				 QByteArray("\x1f\x8b", 2) + "tgs-bytes");
	}
}

TEST_CASE(StickerImportZipRejectsMalformedEndOfCentralDirectory) {
	const auto archive = QByteArray("PK\x05\x06", 4) + QByteArray(19, '\0');
	const auto result
		= Core::ScanStickerImportZip(archive, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::InvalidArchive);
}

TEST_CASE(StickerImportZipRejectsTruncatedEndOfCentralDirectory) {
	auto archive = MakeZip({});
	archive.chop(1);
	const auto result
		= Core::ScanStickerImportZip(archive, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::InvalidArchive);
}

TEST_CASE(StickerImportZipRejectsLocalHeaderCrcMismatch) {
	const auto zip = MakeZip({
		{"sticker.tgs", ValidStickerTGS(), 0, 0, 0, true},
	});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::InvalidArchive);
}

TEST_CASE(StickerImportValidatesMediaFormats) {
	const auto scan = [](const QByteArray &name, const QByteArray &bytes,
						 Core::StickerImportType type) {
		return Core::ScanStickerImportZip(
			MakeZip({{name, bytes}}), type);
	};
	const auto accepted = [](const Core::StickerImportScan &result) {
		return (result.problem == Core::StickerImportProblem::None)
			&& (result.files.size() == 1);
	};
	const auto rejectedAs = [](const Core::StickerImportScan &result,
								Core::StickerImportProblem problem) {
		return (result.files.empty()) && (result.issues.size() == 1)
			&& (result.issues.front().problem == problem);
	};

	const auto stickerWebp = WebpImage(QSize(512, 512));
	const auto emojiWebp = WebpImage(QSize(100, 100));
	CHECK(!stickerWebp.isEmpty());
	CHECK(!emojiWebp.isEmpty());
	CHECK(accepted(scan("sticker.webp", stickerWebp,
						Core::StickerImportType::Stickers)));
	CHECK(accepted(scan("emoji.webp", emojiWebp,
						Core::StickerImportType::Emoji)));
	CHECK(rejectedAs(scan("wrong-sticker-size.webp", emojiWebp,
						  Core::StickerImportType::Stickers),
					 Core::StickerImportProblem::InvalidDimensions));
	CHECK(rejectedAs(scan("wrong-emoji-size.webp", stickerWebp,
						  Core::StickerImportType::Emoji),
					 Core::StickerImportProblem::InvalidDimensions));

	const auto tgs = ValidStickerTGS();
	CHECK(accepted(scan("sticker.tgs", tgs,
						Core::StickerImportType::Stickers)));
	CHECK(rejectedAs(scan("invalid.tgs", QByteArray("not gzip"),
						  Core::StickerImportType::Stickers),
					 Core::StickerImportProblem::InvalidMedia));

	const auto webm = QByteArray("\x1a\x45\xdf\xa3", 4) + "webm";
	CHECK(accepted(scan("sticker.webm", webm,
						Core::StickerImportType::Stickers)));
	CHECK(rejectedAs(scan("invalid.webm", QByteArray("not webm"),
						  Core::StickerImportType::Stickers),
					 Core::StickerImportProblem::InvalidMedia));
}

TEST_CASE(StickerImportFormatDetailsPairMimeAndExtension) {
	const auto webp
		= Core::StickerImportFormatDetails(Core::StickerImportFormat::Webp);
	CHECK_EQ(webp.mimeType, u"image/webp"_q);
	CHECK_EQ(webp.extension, u"webp"_q);
	const auto tgs
		= Core::StickerImportFormatDetails(Core::StickerImportFormat::Tgs);
	CHECK_EQ(tgs.mimeType, u"application/x-tgsticker"_q);
	CHECK_EQ(tgs.extension, u"tgs"_q);
	const auto webm
		= Core::StickerImportFormatDetails(Core::StickerImportFormat::Webm);
	CHECK_EQ(webm.mimeType, u"video/webm"_q);
	CHECK_EQ(webm.extension, u"webm"_q);
}

TEST_CASE(StickerImportZipSkipsSymlinkEntries) {
	const auto mode = quint32(0120000 | 0777) << 16;
	const auto zip = MakeZip(
		{{"linked.tgs", QByteArray("\x1f\x8b", 2) + "tgs-bytes", mode}});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.files.empty());
	CHECK_EQ(int(result.issues.size()), 1);
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

TEST_CASE(StickerImportZipStopsWhenInflatedEntryExceedsItsLimit) {
	const auto zip = MakeZip({
		{"expanded.webp", QByteArray(512 * 1024 + 1, 'x'), 0, 8, 512 * 1024},
	});
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.files.empty());
	CHECK_EQ(int(result.issues.size()), 1);
	if (!result.issues.empty()) {
		CHECK(result.issues.front().problem
			  == Core::StickerImportProblem::InvalidArchive);
	}
}

TEST_CASE(StickerImportZipStopsBeforeAggregateExpansionLimit) {
	auto entries = std::vector<ZipEntry>();
	for (auto i = 0; i != 111; ++i) {
		entries.push_back({
			QByteArray("item") + QByteArray::number(i) + ".webp",
			"x",
			0,
			8,
			1024 * 1024,
		});
	}
	const auto zip = MakeZip(entries);
	const auto result
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::TotalSizeLimit);
	CHECK(result.files.empty());
}

TEST_CASE(StickerImportFolderDoesNotFollowSymlinkDirectories) {
	const auto temporary = QTemporaryDir();
	CHECK(temporary.isValid());
	if (!temporary.isValid()) {
		return;
	}
	const auto folder = temporary.filePath("pack");
	const auto outside = temporary.filePath("outside");
	if (!QDir().mkpath(folder) || !QDir().mkpath(outside)) {
		CHECK(false);
		return;
	}
	const auto bytes = QByteArray("\x1f\x8b", 2) + "tgs-bytes";
	if (!WriteFile(QDir(folder).filePath("inside.tgs"), bytes)
		|| !WriteFile(QDir(outside).filePath("outside.tgs"), bytes)) {
		CHECK(false);
		return;
	}
	const auto linked = QDir(folder).filePath("linked");
	const auto linkedSuccessfully = QFile::link(outside, linked);
	CHECK(linkedSuccessfully);
	if (!linkedSuccessfully) {
		return;
	}
	const auto result = Core::ScanStickerImportFolder(
		folder, Core::StickerImportType::Stickers);
	CHECK_EQ(int(result.files.size()), 1);
	CHECK_EQ(int(result.issues.size()), 1);
	if (!result.issues.empty()) {
		CHECK(result.issues.front().problem
			  == Core::StickerImportProblem::SymbolicLink);
	}
}

TEST_CASE(StickerImportShortNamesMatchServerRules) {
	CHECK(Core::IsValidStickerSetShortName(u"Cats_2"_q));
	CHECK(Core::IsValidStickerSetShortName(u"a"_q));
	CHECK(!Core::IsValidStickerSetShortName(u"2cats"_q));
	CHECK(!Core::IsValidStickerSetShortName(u"cats.pack"_q));
	CHECK(!Core::IsValidStickerSetShortName(QString(65, 'a')));
}

TEST_CASE(StickerImportTitlesUseUnicodeCharacterLimit) {
	CHECK(Core::IsValidStickerSetTitle(u"Cats"_q));
	CHECK(Core::IsValidStickerSetTitle(QString(64, QChar(0x00E9))));
	CHECK(!Core::IsValidStickerSetTitle(QString(65, QChar(0x00E9))));
	auto astral = QString();
	for (auto i = 0; i != 64; ++i) {
		astral += u"😀"_q;
	}
	CHECK(Core::IsValidStickerSetTitle(astral));
	CHECK(!Core::IsValidStickerSetTitle(astral + u"😀"_q));
}

TEST_CASE(StickerImportSourceAndShareLinkSmoke) {
	const auto sticker = ValidStickerTGS();
	CHECK(!sticker.isEmpty());
	const auto zip = MakeZip({{"cats.tgs", sticker}});
	const auto scan
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK_EQ(int(scan.files.size()), 1);
	if (!scan.files.empty()) {
		CHECK(scan.files.front().format == Core::StickerImportFormat::Tgs);
		CHECK_EQ(scan.files.front().bytes, sticker);
	}
	CHECK(Core::IsValidStickerSetTitle(u"Cats"_q));
	CHECK(Core::IsValidStickerSetShortName(u"cats_a"_q));
	const auto prefix = u"https://server.example/"_q;
	const auto shareLink = prefix
						   + Core::StickerImportShareLinkPath(
							   Core::StickerImportType::Stickers, u"cats_a"_q);
	CHECK_EQ(Core::TryConvertUrlToLocal(shareLink, prefix, true),
			 u"tg://addstickers?set=cats_a"_q);
	const auto emojiLink = prefix
						   + Core::StickerImportShareLinkPath(
							   Core::StickerImportType::Emoji, u"cats_a"_q);
	CHECK_EQ(Core::TryConvertUrlToLocal(emojiLink, prefix, true),
			 u"tg://addemoji?set=cats_a"_q);
}

} // namespace
