/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_telegram_sticker_pack_import.h"
#include "api/api_sticker_set_import_requests.h"
#include "core/local_url_conversion.h"
#include "core/sticker_import.h"
#include "mtproto/details/mtproto_serialized_request.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtGui/QImageWriter>

#include <zlib.h>

#include <tuple>
#include <utility>
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

template <typename Request>
[[nodiscard]] QByteArray SerializeRequest(const Request &request) {
	const auto serialized = MTP::details::SerializedRequest::Serialize(request);
	const auto &buffer = *serialized;
	return QByteArray(reinterpret_cast<const char *>(buffer.constData()),
					  int(buffer.size() * sizeof(mtpPrime)));
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
	const auto result = Core::ScanStickerImportZip(
		archive, Core::StickerImportType::Stickers);
	CHECK(result.problem == Core::StickerImportProblem::InvalidArchive);
}

TEST_CASE(StickerImportZipRejectsTruncatedEndOfCentralDirectory) {
	auto archive = MakeZip({});
	archive.chop(1);
	const auto result = Core::ScanStickerImportZip(
		archive, Core::StickerImportType::Stickers);
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
		return Core::ScanStickerImportZip(MakeZip({{name, bytes}}), type);
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
	CHECK(accepted(
		scan("sticker.webp", stickerWebp, Core::StickerImportType::Stickers)));
	CHECK(accepted(
		scan("emoji.webp", emojiWebp, Core::StickerImportType::Emoji)));
	CHECK(rejectedAs(scan("wrong-sticker-size.webp", emojiWebp,
						  Core::StickerImportType::Stickers),
					 Core::StickerImportProblem::InvalidDimensions));
	CHECK(rejectedAs(scan("wrong-emoji-size.webp", stickerWebp,
						  Core::StickerImportType::Emoji),
					 Core::StickerImportProblem::InvalidDimensions));

	const auto tgs = ValidStickerTGS();
	CHECK(
		accepted(scan("sticker.tgs", tgs, Core::StickerImportType::Stickers)));
	CHECK(rejectedAs(scan("invalid.tgs", QByteArray("not gzip"),
						  Core::StickerImportType::Stickers),
					 Core::StickerImportProblem::InvalidMedia));

	const auto webm = QByteArray("\x1a\x45\xdf\xa3", 4) + "webm";
	CHECK(accepted(
		scan("sticker.webm", webm, Core::StickerImportType::Stickers)));
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

TEST_CASE(StickerImportSuggestionsIgnoreDelayedResponses) {
	auto guard = Core::StickerImportSuggestionGuard();
	const auto request = guard.begin(u"Cats"_q, u"cats_a"_q);
	CHECK(guard.isCurrent(request, u"Cats"_q, u"cats_a"_q));
	CHECK(!guard.isCurrent(request, u"Dogs"_q, u"cats_a"_q));
	CHECK(!guard.isCurrent(request, u"Cats"_q, u"typed_name"_q));
	guard.invalidate();
	CHECK(!guard.isCurrent(request, u"Cats"_q, u"cats_a"_q));
	const auto newerRequest = guard.begin(u"Cats"_q, u"cats_a"_q);
	CHECK(!guard.isCurrent(request, u"Cats"_q, u"cats_a"_q));
	CHECK(guard.isCurrent(newerRequest, u"Cats"_q, u"cats_a"_q));
}

TEST_CASE(StickerImportConsentZipShareInstallSmoke) {
	const auto sticker = ValidStickerTGS();
	CHECK(!sticker.isEmpty());
	const auto zip = MakeZip({{"cats.tgs", sticker}});
	const auto scan
		= Core::ScanStickerImportZip(zip, Core::StickerImportType::Stickers);
	CHECK_EQ(int(scan.files.size()), 1);
	const auto temporary = QTemporaryDir();
	CHECK(temporary.isValid());
	if (!temporary.isValid()) {
		return;
	}
	const auto folderPath = temporary.filePath(u"folder"_q);
	const auto folderCreated = QDir().mkpath(folderPath);
	CHECK(folderCreated);
	if (!folderCreated) {
		return;
	}
	const auto folderFile = QDir(folderPath).filePath(u"cats.tgs"_q);
	const auto folderWritten = WriteFile(folderFile, sticker);
	CHECK(folderWritten);
	const auto zipPath = temporary.filePath(u"cats.zip"_q);
	const auto zipWritten = WriteFile(zipPath, zip);
	CHECK(zipWritten);
	if (!folderWritten || !zipWritten) {
		return;
	}
	const auto folderScan = Core::ScanStickerImportFolder(
		folderPath, Core::StickerImportType::Stickers);
	CHECK_EQ(int(folderScan.files.size()), 1);
	auto selection = Core::StickerImportSelection();
	selection.selectSource(Core::StickerImportSource::Folder, folderPath);
	selection.setRightsConfirmed(true);
	CHECK(selection.type() == Core::StickerImportSource::Folder);
	CHECK(Core::ValidateStickerImportStart(
			  false, selection.rightsConfirmed(), u"Cats"_q, u"cats_a"_q,
			  !selection.path().isEmpty(), folderScan)
		  == Core::StickerImportValidation::Ready);
	selection.selectSource(Core::StickerImportSource::Zip, zipPath);
	CHECK(selection.type() == Core::StickerImportSource::Zip);
	CHECK(!selection.rightsConfirmed());
	CHECK(Core::ValidateStickerImportStart(false, selection.rightsConfirmed(),
										   u"Cats"_q, u"cats_a"_q,
										   !selection.path().isEmpty(), scan)
		  == Core::StickerImportValidation::RightsNotConfirmed);
	selection.setRightsConfirmed(true);
	CHECK(Core::ValidateStickerImportStart(false, selection.rightsConfirmed(),
										   u"Cats"_q, u"cats_a"_q,
										   !selection.path().isEmpty(), scan)
		  == Core::StickerImportValidation::Ready);
	CHECK(Core::ValidateStickerImportStart(false, false, u"Cats"_q, u"cats_a"_q,
										   true, scan)
		  == Core::StickerImportValidation::RightsNotConfirmed);
	CHECK(Core::ValidateStickerImportStart(false, true, u"Cats"_q, u"cats_a"_q,
										   true, scan)
		  == Core::StickerImportValidation::Ready);
	if (!scan.files.empty()) {
		CHECK(scan.files.front().format == Core::StickerImportFormat::Tgs);
		CHECK_EQ(scan.files.front().bytes, sticker);
	}
	if (!scan.files.empty()) {
		const auto inputFile = MTP_inputFile(MTP_long(1), MTP_int(1),
											 MTP_string(u"sticker_1.tgs"_q),
											 MTP_string(u"checksum"_q));
		const auto upload = Api::details::StickerImportUploadMediaRequest(
			inputFile, scan.files.front(), 0);
		auto attributes = QVector<MTPDocumentAttribute>();
		attributes.push_back(
			MTP_documentAttributeFilename(MTP_string(u"sticker_1.tgs"_q)));
		attributes.push_back(MTP_documentAttributeAnimated());
		attributes.push_back(MTP_documentAttributeSticker(
			MTP_flags(0), MTP_string(u"⭐"_q), MTP_inputStickerSetEmpty(),
			MTPMaskCoords()));
		const auto expectedUpload = MTPmessages_UploadMedia(
			MTP_flags(0), MTPstring(), MTP_inputPeerSelf(),
			MTP_inputMediaUploadedDocument(
				MTP_flags(0), inputFile, MTPInputFile(),
				MTP_string(u"application/x-tgsticker"_q),
				MTP_vector<MTPDocumentAttribute>(std::move(attributes)),
				MTP_vector<MTPInputDocument>(), MTPInputPhoto(), MTP_int(0),
				MTP_int(0)));
		CHECK_EQ(SerializeRequest(upload), SerializeRequest(expectedUpload));
	}
	CHECK(Core::IsValidStickerSetTitle(u"Cats"_q));
	CHECK(Core::IsValidStickerSetShortName(u"cats_a"_q));
	const auto fileReference = MTP_bytes(QByteArray("file-reference"));
	const auto uploadedDocument = MTP_document(
		MTP_flags(0), MTP_long(1), MTP_long(2), fileReference, MTP_int(1),
		MTP_string(u"application/x-tgsticker"_q), MTP_long(sticker.size()),
		MTP_vector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
		MTP_vector<MTPDocumentAttribute>());
	const auto uploadedMedia = MTP_messageMediaDocument(
		MTP_flags(MTPDmessageMediaDocument::Flag::f_document), uploadedDocument,
		MTPVector<MTPDocument>(), MTPPhoto(), MTPint(), MTPint());
	const auto itemFromUpload
		= Api::details::StickerImportSetItemFromUploadMedia(uploadedMedia);
	CHECK(itemFromUpload.has_value());
	if (!itemFromUpload) {
		return;
	}
	const auto item = *itemFromUpload;
	const auto expectedItem = MTP_inputStickerSetItem(
		MTP_flags(0),
		MTP_inputDocument(MTP_long(1), MTP_long(2), fileReference),
		MTP_string(u"⭐"_q), MTPMaskCoords(), MTPstring());
	for (const auto &[source, sourceText, type, emoji] : {
			 std::tuple{
				 Core::StickerImportSource::Folder,
				 u"folder"_q,
				 Data::StickersType::Stickers,
				 false,
			 },
			 std::tuple{
				 Core::StickerImportSource::Zip,
				 u"zip"_q,
				 Data::StickersType::Stickers,
				 false,
			 },
			 std::tuple{
				 Core::StickerImportSource::Zip,
				 u"zip"_q,
				 Data::StickersType::Emoji,
				 true,
			 },
		 }) {
		auto items = QVector<MTPInputStickerSetItem>{item};
		auto expectedItems = QVector<MTPInputStickerSetItem>{expectedItem};
		const auto request = Api::details::StickerImportCreateSetRequest(
			u"Cats"_q, u"cats_a"_q, type, source, items);
		auto flags = MTPstickers_CreateStickerSet::Flags(
			MTPstickers_CreateStickerSet::Flag::f_software);
		if (emoji) {
			flags |= MTPstickers_CreateStickerSet::Flag::f_emojis;
		}
		const auto expected = MTPstickers_CreateStickerSet(
			MTP_flags(flags), MTP_inputUserSelf(), MTP_string(u"Cats"_q),
			MTP_string(u"cats_a"_q), MTPInputDocument(),
			MTP_vector<MTPInputStickerSetItem>(std::move(expectedItems)),
			MTP_string(sourceText));
		CHECK_EQ(SerializeRequest(request), SerializeRequest(expected));
	}
	const auto prefix = u"https://server.example/"_q;
	const auto shareLink = prefix
						   + Core::StickerImportShareLinkPath(
							   Core::StickerImportType::Stickers, u"cats_a"_q);
	const auto localStickerLink
		= Core::TryConvertUrlToLocal(shareLink, prefix, true);
	CHECK_EQ(localStickerLink, u"tg://addstickers?set=cats_a"_q);
	const auto stickerTarget
		= Core::StickerSetInstallTargetFromLocalUrl(localStickerLink);
	CHECK(stickerTarget.has_value());
	if (stickerTarget) {
		CHECK_EQ(stickerTarget->shortName, u"cats_a"_q);
		CHECK(stickerTarget->type == Core::StickerImportType::Stickers);
		const auto install = Api::details::StickerImportInstallSetRequest(
			MTP_inputStickerSetShortName(MTP_string(stickerTarget->shortName)));
		const auto expectedInstall = MTPmessages_InstallStickerSet(
			MTP_inputStickerSetShortName(MTP_string(u"cats_a"_q)),
			MTP_bool(false));
		CHECK_EQ(SerializeRequest(install), SerializeRequest(expectedInstall));
	}
	const auto emojiLink = prefix
						   + Core::StickerImportShareLinkPath(
							   Core::StickerImportType::Emoji, u"cats_a"_q);
	const auto localEmojiLink
		= Core::TryConvertUrlToLocal(emojiLink, prefix, true);
	CHECK_EQ(localEmojiLink, u"tg://addemoji?set=cats_a"_q);
	const auto emojiTarget
		= Core::StickerSetInstallTargetFromLocalUrl(localEmojiLink);
	CHECK(emojiTarget.has_value());
	if (emojiTarget) {
		CHECK_EQ(emojiTarget->shortName, u"cats_a"_q);
		CHECK(emojiTarget->type == Core::StickerImportType::Emoji);
	}
}

TEST_CASE(TelegramStickerPackLinksAreCanonicalAndPublic) {
	const auto stickers = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/SomePack"_q);
	CHECK(stickers.has_value());
	if (stickers) {
		CHECK_EQ(stickers->name, u"SomePack"_q);
		CHECK(stickers->type == Core::StickerImportType::Stickers);
		CHECK_EQ(stickers->sourceUrl, u"https://t.me/addstickers/SomePack"_q);
	}
	const auto emoji
		= Core::ParseTelegramStickerPackLink(u"t.me/addemoji/SomePack"_q);
	CHECK(emoji.has_value());
	if (emoji) {
		CHECK(emoji->type == Core::StickerImportType::Emoji);
		CHECK_EQ(emoji->sourceUrl, u"https://t.me/addemoji/SomePack"_q);
	}
	for (const auto &value : {
			 u"http://t.me/addstickers/SomePack"_q,
			 u"https://t.me.evil/addstickers/SomePack"_q,
			 u"https://t.me/addstickers/SomePack?start=secret"_q,
			 u"https://t.me/addstickers/%2e%2e"_q,
			 u"https://t.me/addstickers/a-b"_q,
		 }) {
		CHECK(!Core::ParseTelegramStickerPackLink(value));
	}
}

TEST_CASE(TelegramBotTokensAndFilePathsAreValidatedBeforeUrlBuild) {
	const auto token = u"123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"_q;
	CHECK(Api::details::IsValidTelegramBotToken(token));
	CHECK(!Api::details::IsValidTelegramBotToken(
		u"123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcde"_q));
	CHECK(!Api::details::IsValidTelegramBotToken(
		u"123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef/"_q));
	CHECK(
		Api::details::IsSafeTelegramStickerFilePath(u"stickers/file_1.tgs"_q));
	CHECK(!Api::details::IsSafeTelegramStickerFilePath(
		u"stickers/../file.tgs"_q));
	CHECK(!Api::details::IsSafeTelegramStickerFilePath(
		u"stickers/%2e%2e/file.tgs"_q));
	CHECK(!Api::details::IsSafeTelegramStickerFilePath(
		u"https%3A%2F%2Felsewhere.example/file.tgs"_q));
	CHECK(!Api::details::IsSafeTelegramStickerFilePath(
		u"https://elsewhere.example/file.tgs"_q));

	const auto method
		= Api::details::TelegramBotApiMethodUrl(token, u"getStickerSet"_q);
	CHECK_EQ(method.scheme(), u"https"_q);
	CHECK_EQ(method.host(), u"api.telegram.org"_q);
	CHECK_EQ(method.path(),
			 u"/bot123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef/getStickerSet"_q);
	const auto file
		= Api::details::TelegramBotApiFileUrl(token, u"stickers/file_1.tgs"_q);
	CHECK(file.has_value());
	if (file) {
		CHECK_EQ(file->scheme(), u"https"_q);
		CHECK_EQ(file->host(), u"api.telegram.org"_q);
		CHECK(file->path().contains(u"/file/bot123456789:"_q));
	}
	CHECK(
		!Api::details::TelegramBotApiFileUrl(token, u"../outside/file.tgs"_q));
}

TEST_CASE(TelegramStickerImportPreservesPackOrderEmojiAndCanonicalSource) {
	const auto token = u"123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"_q;
	const auto scan = Core::ScanTelegramStickerImportFiles(
		{
			{
				.name = u"first.tgs"_q,
				.bytes = ValidStickerTGS(),
				.format = Core::StickerImportFormat::Tgs,
				.emoji = u"🦊"_q,
			},
			{
				.name = u"second.tgs"_q,
				.bytes = ValidStickerTGS(),
				.format = Core::StickerImportFormat::Tgs,
				.emoji = u"🌿"_q,
			},
		},
		Core::StickerImportType::Emoji);
	CHECK(scan.problem == Core::StickerImportProblem::None);
	CHECK_EQ(int(scan.files.size()), 2);
	if (scan.files.size() == 2) {
		CHECK_EQ(scan.files[0].emoji, u"🦊"_q);
		CHECK_EQ(scan.files[1].emoji, u"🌿"_q);
		CHECK(scan.files[0].format == Core::StickerImportFormat::Tgs);
		const auto inputFile = MTP_inputFile(MTP_long(1), MTP_int(1),
											 MTP_string(u"sticker_1.tgs"_q),
											 MTP_string(u"checksum"_q));
		const auto upload = Api::details::StickerImportUploadMediaRequest(
			inputFile, scan.files[0], 0);
		const auto serializedUpload = SerializeRequest(upload);
		CHECK(serializedUpload.contains(u"🦊"_q.toUtf8()));
		CHECK(!serializedUpload.contains(u"⭐"_q.toUtf8()));
		CHECK(!serializedUpload.contains(token.toUtf8()));
		const auto uploadedDocument = MTP_document(
			MTP_flags(0), MTP_long(1), MTP_long(2),
			MTP_bytes(QByteArray("file-reference")), MTP_int(1),
			MTP_string(u"application/x-tgsticker"_q), MTP_long(1),
			MTP_vector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
			MTP_vector<MTPDocumentAttribute>());
		const auto uploadedMedia = MTP_messageMediaDocument(
			MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
			uploadedDocument, MTPVector<MTPDocument>(), MTPPhoto(), MTPint(),
			MTPint());
		const auto item = Api::details::StickerImportSetItemFromUploadMedia(
			uploadedMedia, scan.files[0].emoji);
		CHECK(item.has_value());
		if (item) {
			const auto request = Api::details::StickerImportCreateSetRequest(
				u"Foxes"_q, u"foxes_pack"_q, Data::StickersType::Emoji,
				Core::StickerImportSource::Telegram,
				QVector<MTPInputStickerSetItem>{*item},
				u"https://t.me/addemoji/FoxesPack"_q);
			CHECK(SerializeRequest(request).contains(u"🦊"_q.toUtf8()));
		}
	}

	const auto link = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addemoji/SomePack"_q);
	CHECK(link.has_value());
	if (link && !scan.files.empty()) {
		const auto item = MTP_inputStickerSetItem(
			MTP_flags(0),
			MTP_inputDocument(MTP_long(1), MTP_long(2),
							  MTP_bytes(QByteArray("file-reference"))),
			MTP_string(u"🦊"_q), MTPMaskCoords(), MTPstring());
		const auto request = Api::details::StickerImportCreateSetRequest(
			u"Some Pack"_q, u"some_pack"_q, Data::StickersType::Emoji,
			Core::StickerImportSource::Telegram,
			QVector<MTPInputStickerSetItem>{item}, link->sourceUrl);
		const auto serialized = SerializeRequest(request);
		CHECK(serialized.contains(link->sourceUrl.toUtf8()));
		CHECK(!serialized.contains(token.toUtf8()));
	}
}

TEST_CASE(TelegramEmojiStickersAreMatchedBackToPackOrder) {
	const auto set = Api::details::ParseTelegramStickerSetResponse(
		QByteArray(
			R"({"ok":true,"result":{"name":"EmojiPack","title":"Foxes","sticker_type":"custom_emoji","stickers":[{"file_id":"set-file-1","file_unique_id":"unique-1","type":"custom_emoji","width":512,"height":512,"is_animated":true,"is_video":false,"emoji":"🦊","custom_emoji_id":"101"},{"file_id":"set-file-2","file_unique_id":"unique-2","type":"custom_emoji","width":512,"height":512,"is_animated":false,"is_video":false,"emoji":"🌿","custom_emoji_id":"202"}]}})"),
		Core::StickerImportType::Emoji);
	CHECK(set.has_value());
	if (!set) {
		return;
	}
	CHECK_EQ(set->title, u"Foxes"_q);
	CHECK_EQ(int(set->stickers.size()), 2);
	if (set->stickers.size() == 2) {
		CHECK_EQ(set->stickers[0].customEmojiId, u"101"_q);
		CHECK_EQ(set->stickers[0].emoji, u"🦊"_q);
		CHECK(set->stickers[0].format == Core::StickerImportFormat::Tgs);
		CHECK_EQ(set->stickers[1].customEmojiId, u"202"_q);
		CHECK(set->stickers[1].format == Core::StickerImportFormat::Webp);
	}
	const auto details = Api::details::ParseTelegramCustomEmojiStickersResponse(
		QByteArray(
			R"({"ok":true,"result":[{"file_id":"emoji-file-202","file_unique_id":"emoji-unique-202","type":"custom_emoji","width":100,"height":100,"is_animated":false,"is_video":false,"emoji":"🌿","custom_emoji_id":"202"},{"file_id":"emoji-file-101","file_unique_id":"emoji-unique-101","type":"custom_emoji","width":512,"height":512,"is_animated":true,"is_video":false,"emoji":"🦊","custom_emoji_id":"101"}]})"),
		set->stickers);
	CHECK(details.has_value());
	if (details) {
		CHECK_EQ((*details)[0].fileId, u"emoji-file-101"_q);
		CHECK_EQ((*details)[1].fileId, u"emoji-file-202"_q);
		CHECK_EQ((*details)[0].emoji, u"🦊"_q);
		CHECK((*details)[0].format == Core::StickerImportFormat::Tgs);
	}
}

TEST_CASE(TelegramBotApiErrorsRedactTheToken) {
	const auto token = u"123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"_q;
	const auto text = Api::details::RedactTelegramBotApiError(
		u"Unauthorized: https://api.telegram.org/bot123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef/getStickerSet"_q,
		token);
	CHECK(!text.contains(token));
	CHECK(!text.contains(u"/bot123456789:"_q));
}

} // namespace
