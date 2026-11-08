/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_telegram_sticker_pack.h"
#include "api/api_sticker_set_import_requests.h"
#include "boxes/sticker_import_feedback.h"
#include "core/local_url_conversion.h"
#include "core/sticker_import.h"
#include "core/telegram_sticker_import.h"
#include "logs.h"
#include "mtproto/details/mtproto_serialized_request.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaObject>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtCore/QVariant>
#include <QtGui/QImage>
#include <QtGui/QImageWriter>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>

namespace Api {
struct TelegramStickerPackDownloaderTestPeer {
	static void SetDownloadedBytes(TelegramStickerPackDownloader &downloader,
								   quint64 bytes) {
		downloader._totalDownloaded = bytes;
	}
};
} // namespace Api

namespace {

struct ZipEntry {
	QByteArray name;
	QByteArray data;
	quint32 externalAttributes = 0;
	quint16 method = 0;
	quint32 advertisedExpandedSize = 0;
	bool localCrcMismatch = false;
};

struct StickerImportResponse {
	int status = 200;
	QByteArray body;
	Fn<void()> beforeFinish = nullptr;
	QUrl responseUrl;
	std::optional<qint64> contentLength;
	QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
	QString errorString;
};

class StickerImportNetworkReply final : public QNetworkReply {
  public:
	StickerImportNetworkReply(const QNetworkRequest &request,
							  QNetworkAccessManager::Operation operation,
							  const StickerImportResponse &response,
							  QObject *parent);
	void abort() override;
	bool isSequential() const override;
	qint64 bytesAvailable() const override;

  protected:
	qint64 readData(char *data, qint64 maximum) override;

  private:
	QByteArray _body;
	Fn<void()> _beforeFinish;
	qsizetype _offset = 0;
	bool _aborted = false;
};

class StickerImportNetwork final : public QNetworkAccessManager {
  public:
	using Response = StickerImportResponse;
	using Respond = Fn<Response(const QUrl &)>;

	explicit StickerImportNetwork(Respond respond);

	QVector<QUrl> requests;

  protected:
	QNetworkReply *createRequest(Operation operation,
								 const QNetworkRequest &request,
								 QIODevice *) override;

  private:
	Respond _respond;
};

StickerImportNetworkReply::StickerImportNetworkReply(
	const QNetworkRequest &request, QNetworkAccessManager::Operation operation,
	const StickerImportResponse &response, QObject *parent)
	: QNetworkReply(parent), _body(response.body),
	  _beforeFinish(response.beforeFinish) {
	setRequest(request);
	setUrl(response.responseUrl.isEmpty() ? request.url()
										  : response.responseUrl);
	setOperation(operation);
	setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
	if (response.contentLength) {
		setHeader(QNetworkRequest::ContentLengthHeader,
				  QVariant::fromValue(*response.contentLength));
	}
	open(QIODevice::ReadOnly | QIODevice::Unbuffered);
	if (response.networkError != QNetworkReply::NoError) {
		setError(response.networkError, response.errorString);
	}
	QTimer::singleShot(0, this, [this] {
		Q_EMIT metaDataChanged();
		if (_aborted) {
			return;
		}
		Q_EMIT readyRead();
		if (_beforeFinish) {
			_beforeFinish();
		}
		setFinished(true);
		Q_EMIT finished();
	});
}

void StickerImportNetworkReply::abort() {
	if (_aborted) {
		return;
	}
	_aborted = true;
	setError(QNetworkReply::OperationCanceledError, QString());
	setFinished(true);
	Q_EMIT finished();
}

bool StickerImportNetworkReply::isSequential() const { return true; }

qint64 StickerImportNetworkReply::bytesAvailable() const {
	return qint64(_body.size() - _offset) + QNetworkReply::bytesAvailable();
}

qint64 StickerImportNetworkReply::readData(char *data, qint64 maximum) {
	if (_offset >= _body.size()) {
		return -1;
	}
	const auto count
		= std::min<qint64>(maximum, qint64(_body.size() - _offset));
	memcpy(data, _body.constData() + _offset, size_t(count));
	_offset += qsizetype(count);
	return count;
}

StickerImportNetwork::StickerImportNetwork(Respond respond)
	: _respond(std::move(respond)) {}

QNetworkReply *StickerImportNetwork::createRequest(
	Operation operation, const QNetworkRequest &request, QIODevice *) {
	requests.push_back(request.url());
	const auto response = _respond(request.url());
	return new StickerImportNetworkReply(request, operation, response, this);
}

[[nodiscard]] QByteArray JsonBytes(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

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

TEST_CASE(TelegramStickerPackLinksAreParsedAndCanonicalized) {
	const auto stickers = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/Cats_2026"_q);
	CHECK(stickers.has_value());
	CHECK(stickers->type == Core::StickerImportType::Stickers);
	CHECK(stickers->name == u"Cats_2026"_q);
	CHECK(stickers->canonicalUrl() == u"https://t.me/addstickers/Cats_2026"_q);
	auto renamed = *stickers;
	renamed.name = u"Dogs"_q;
	CHECK(renamed.canonicalUrl() == u"https://t.me/addstickers/Dogs"_q);
	const auto emoji
		= Core::ParseTelegramStickerPackLink(u"t.me/addemoji/Emoji_pack"_q);
	CHECK(emoji.has_value());
	CHECK(emoji->type == Core::StickerImportType::Emoji);
	CHECK(emoji->canonicalUrl() == u"https://t.me/addemoji/Emoji_pack"_q);
	for (const auto &invalid : {
			 u"https://evil.example/addstickers/Cats"_q,
			 u"https://t.me/addstickers/Cats?start=secret"_q,
			 u"https://t.me/addstickers/a/other"_q,
			 u"https://t.me/addstickers/../private"_q,
			 u"https://t.me:443/addstickers/Cats"_q,
			 u"https://user@t.me/addstickers/Cats"_q,
			 u"https://t.me/addstickers/Cats#fragment"_q,
			 u"https://t.me/addstickers/"_q,
		 }) {
		CHECK(!Core::ParseTelegramStickerPackLink(invalid));
	}
}

TEST_CASE(TelegramStickerBotTokensAndFilePathsAreValidated) {
	const auto tokenValue = Core::TelegramBotToken::Create(
		u"12345:AbCdEf0123456789012345678901234567890"_q);
	CHECK(tokenValue.has_value());
	CHECK(!Core::TelegramBotToken::Create(u"12345:short"_q));
	if (!tokenValue) {
		return;
	}
	const auto copy = tokenValue->clone();
	CHECK(copy.apiUrl(u"getStickerSet"_q).has_value());
	CHECK(!copy.redact(u"12345:AbCdEf0123456789012345678901234567890"_q)
			   .contains(u"12345:AbCdEf0123456789012345678901234567890"_q));
	CHECK(!Core::TelegramBotToken::Create(
		u"12345:AbCdEf0123456789012345678901234567890\n"_q));
	CHECK(!Core::TelegramBotToken::Create(
		u"12345:AbCdEf0123456789012345678901234567890/extra"_q));
	const auto apiUrl = tokenValue->apiUrl(u"getStickerSet"_q);
	CHECK(apiUrl.has_value());
	if (apiUrl) {
		CHECK(apiUrl->scheme() == u"https"_q);
		CHECK(apiUrl->host() == u"api.telegram.org"_q);
		CHECK(apiUrl->path(QUrl::FullyDecoded).startsWith(u"/bot12345:"_q));
	}
	const auto fileUrl = tokenValue->fileUrl(u"stickers/file_name.webp"_q);
	CHECK(fileUrl.has_value());
	if (fileUrl) {
		CHECK(fileUrl->scheme() == u"https"_q);
		CHECK(fileUrl->host() == u"api.telegram.org"_q);
		CHECK(fileUrl->path(QUrl::FullyDecoded).contains(u"/file/bot12345:"_q));
	}
	CHECK(Core::IsValidTelegramStickerFilePath(u"stickers/file_name.webp"_q));
	for (const auto &invalid : {
			 u"../file.webp"_q,
			 u"stickers/../file.webp"_q,
			 u"https://evil.example/file.webp"_q,
			 u"/file.webp"_q,
			 u"stickers\\file.webp"_q,
			 u"stickers/file.webp?x=1"_q,
			 u""_q,
		 }) {
		CHECK(!Core::IsValidTelegramStickerFilePath(invalid));
		CHECK(!tokenValue->fileUrl(invalid));
	}
}

TEST_CASE(TelegramStickerBotErrorsRedactSecrets) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	const auto value = Core::TelegramBotToken::Create(token);
	CHECK(value.has_value());
	if (!value) {
		return;
	}
	const auto description = u"Request failed for bot%1/getFile"_q.arg(token);
	const auto redacted = value->redact(description);
	CHECK(!redacted.contains(token));
	CHECK(redacted.contains(u"[redacted]"_q));
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(token));
	CHECK(
		!value->redact(u"Failed URL: bot%1"_q.arg(encoded)).contains(encoded));
	const auto otherToken
		= u"98765:"_q + u"ZyXwVu9876543210"_q + u"98765432109876543210"_q;
	CHECK(
		!value->redact(u"Bad token %1"_q.arg(otherToken)).contains(otherToken));
}

struct StickerDownloadFailure {
	std::optional<Api::TelegramStickerPackDownloadError> error;
	QString detail;
	int requestCount = 0;
};

[[nodiscard]] Core::TelegramBotToken
StickerTestToken(QString value
				 = u"12345:AbCdEf0123456789012345678901234567890"_q) {
	auto token = Core::TelegramBotToken::Create(std::move(value));
	Expects(token.has_value());
	return std::move(*token);
}

[[nodiscard]] StickerDownloadFailure RunStickerDownloadFailure(
	StickerImportNetwork::Respond respond, quint64 initialBytes = 0,
	QString linkText = u"https://t.me/addstickers/Cats"_q) {
	const auto link = Core::ParseTelegramStickerPackLink(linkText);
	Expects(link.has_value());
	auto network = StickerImportNetwork(std::move(respond));
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(), &network);
	Api::TelegramStickerPackDownloaderTestPeer::SetDownloadedBytes(
		downloader, initialBytes);
	auto loop = QEventLoop();
	auto result = StickerDownloadFailure();
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	downloader.start(
		[&](QString, Core::StickerImportScan) { loop.quit(); },
		[&](Api::TelegramStickerPackDownloadError error, QString detail) {
			result.error = error;
			result.detail = std::move(detail);
			loop.quit();
		},
		nullptr);
	loop.exec();
	result.requestCount = int(network.requests.size());
	return result;
}

[[nodiscard]] QByteArray StickerSetResponse(QString emoji,
											QString type = u"regular"_q,
											QString customId = QString()) {
	auto sticker = QJsonObject();
	sticker.insert(u"file_id"_q, u"file-1"_q);
	sticker.insert(u"file_unique_id"_q, u"unique-1"_q);
	sticker.insert(u"type"_q, type == u"custom_emoji"_q ? u"custom_emoji"_q
							  : type == u"mask"_q		? u"mask"_q
														: u"regular"_q);
	sticker.insert(u"width"_q, 100);
	sticker.insert(u"height"_q, 100);
	sticker.insert(u"is_animated"_q, true);
	sticker.insert(u"is_video"_q, false);
	sticker.insert(u"emoji"_q, emoji);
	if (!customId.isEmpty()) {
		sticker.insert(u"custom_emoji_id"_q, customId);
	}
	auto set = QJsonObject();
	set.insert(u"name"_q, u"Cats"_q);
	set.insert(u"title"_q, u"Cats"_q);
	if (!type.isNull()) {
		set.insert(u"sticker_type"_q, type);
	}
	set.insert(u"stickers"_q, QJsonArray{sticker});
	auto response = QJsonObject();
	response.insert(u"ok"_q, true);
	response.insert(u"result"_q, set);
	return JsonBytes(response);
}

TEST_CASE(TelegramStickerPackRejectsMismatchedOrUnsupportedTypes) {
	using Error = Api::TelegramStickerPackDownloadError;
	struct Case {
		QString link;
		QString type;
		Error expected;
	};
	for (const auto &test : {
			 Case{u"https://t.me/addstickers/Cats"_q, u"custom_emoji"_q,
				  Error::UnsupportedPackType},
			 Case{u"https://t.me/addemoji/Cats"_q, u"regular"_q,
				  Error::UnsupportedPackType},
			 Case{u"https://t.me/addstickers/Cats"_q, u"mask"_q,
				  Error::UnsupportedPackType},
			 Case{u"https://t.me/addemoji/Cats"_q, u"mask"_q,
				  Error::UnsupportedPackType},
			 Case{u"https://t.me/addstickers/Cats"_q, u"other"_q,
				  Error::InvalidResponse},
			 Case{u"https://t.me/addstickers/Cats"_q, u""_q,
				  Error::InvalidResponse},
			 Case{u"https://t.me/addstickers/Cats"_q, QString(),
				  Error::InvalidResponse},
		 }) {
		const auto result = RunStickerDownloadFailure(
			[&](const QUrl &) {
				return StickerImportNetwork::Response{
					200, StickerSetResponse(u"🐈"_q, test.type, u"101"_q)};
			},
			0, test.link);
		CHECK(result.error == test.expected);
		CHECK_EQ(result.requestCount, 1);
	}
}

TEST_CASE(TelegramStickerPackRejectsMissingEmojiBeforeFileDownload) {
	const auto result = RunStickerDownloadFailure([](const QUrl &) {
		return StickerImportNetwork::Response{200,
											  StickerSetResponse(QString())};
	});
	CHECK(result.error
		  == Api::TelegramStickerPackDownloadError::InvalidResponse);
	CHECK_EQ(result.requestCount, 1);
}

TEST_CASE(TelegramStickerPackMapsBotApiErrorsAndRedactsToken) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	using Error = Api::TelegramStickerPackDownloadError;
	struct Case {
		int code;
		QString description;
		Error expected;
	};
	for (const auto &test : {
			 Case{401, u"Unauthorized"_q, Error::InvalidToken},
			 Case{404, u"Not Found"_q, Error::InvalidToken},
			 Case{400, u"Bad Request: STICKERSET_INVALID"_q,
				  Error::PackNotFound},
			 Case{500, u"failed "_q + token, Error::BotApi},
			 Case{500, QString(), Error::BotApi},
		 }) {
		const auto result = RunStickerDownloadFailure([&](const QUrl &) {
			auto object = QJsonObject();
			object.insert(u"ok"_q, false);
			object.insert(u"error_code"_q, test.code);
			object.insert(u"description"_q, test.description);
			return StickerImportNetwork::Response{test.code, JsonBytes(object)};
		});
		CHECK(result.error == test.expected);
		CHECK_EQ(result.requestCount, 1);
		CHECK(!result.detail.contains(token));
		if (test.description.contains(token)) {
			CHECK(result.detail.contains(u"[redacted]"_q));
		} else if (test.description.isEmpty()) {
			CHECK(result.detail.contains(u"500"_q));
		}
	}
}

TEST_CASE(TelegramStickerPackInvalidTokenPresentsRecoverableError) {
	const auto result = RunStickerDownloadFailure([](const QUrl &) {
		auto object = QJsonObject();
		object.insert(u"ok"_q, false);
		object.insert(u"error_code"_q, 401);
		object.insert(u"description"_q, u"Unauthorized"_q);
		auto response = StickerImportNetwork::Response{401, JsonBytes(object)};
		response.networkError = QNetworkReply::AuthenticationRequiredError;
		return response;
	});
	CHECK(result.error == Api::TelegramStickerPackDownloadError::InvalidToken);
	if (!result.error) {
		return;
	}
	auto scanPending = true;
	const auto feedback = Ui::StickerImport::TelegramDownloadFailureFeedback(
		*result.error, result.detail, scanPending);
	CHECK(!scanPending);
	CHECK(feedback.line
		  == Ui::StickerImport::TelegramDownloadFailureLine::Status);
	CHECK(feedback.color
		  == Ui::StickerImport::TelegramDownloadFailureColor::Error);
	CHECK(feedback.message
		  == Ui::StickerImport::TelegramDownloadFailureMessage::InvalidToken);
}

TEST_CASE(TelegramStickerPackMissingPackPresentsRecoverableError) {
	const auto result = RunStickerDownloadFailure([](const QUrl &) {
		auto object = QJsonObject();
		object.insert(u"ok"_q, false);
		object.insert(u"error_code"_q, 400);
		object.insert(u"description"_q, u"Bad Request: STICKERSET_INVALID"_q);
		return StickerImportNetwork::Response{400, JsonBytes(object)};
	});
	CHECK(result.error == Api::TelegramStickerPackDownloadError::PackNotFound);
	if (!result.error) {
		return;
	}
	auto scanPending = true;
	const auto feedback = Ui::StickerImport::TelegramDownloadFailureFeedback(
		*result.error, result.detail, scanPending);
	CHECK(!scanPending);
	CHECK(feedback.line
		  == Ui::StickerImport::TelegramDownloadFailureLine::Status);
	CHECK(feedback.color
		  == Ui::StickerImport::TelegramDownloadFailureColor::Error);
	CHECK(feedback.message
		  == Ui::StickerImport::TelegramDownloadFailureMessage::PackNotFound);
}

TEST_CASE(TelegramStickerPackNetworkFailurePresentsRecoverableError) {
	const auto result = RunStickerDownloadFailure([](const QUrl &) {
		auto response = StickerImportNetwork::Response{0, {}};
		response.networkError = QNetworkReply::ConnectionRefusedError;
		return response;
	});
	CHECK(result.error == Api::TelegramStickerPackDownloadError::Network);
	if (!result.error) {
		return;
	}
	auto scanPending = true;
	const auto feedback = Ui::StickerImport::TelegramDownloadFailureFeedback(
		*result.error, result.detail, scanPending);
	CHECK(!scanPending);
	CHECK(feedback.line
		  == Ui::StickerImport::TelegramDownloadFailureLine::Status);
	CHECK(feedback.color
		  == Ui::StickerImport::TelegramDownloadFailureColor::Error);
	CHECK(feedback.message
		  == Ui::StickerImport::TelegramDownloadFailureMessage::NetworkError);
}

TEST_CASE(TelegramStickerPackRefusesRedirectsAndForeignResponses) {
	using Error = Api::TelegramStickerPackDownloadError;
	for (const auto &foreign : {false, true}) {
		const auto result = RunStickerDownloadFailure([&](const QUrl &) {
			auto response
				= StickerImportNetwork::Response{foreign ? 200 : 302, {}};
			if (foreign) {
				response.responseUrl = QUrl(u"https://other.example/file"_q);
			}
			return response;
		});
		CHECK(result.error == Error::Network);
		CHECK_EQ(result.requestCount, 1);
		CHECK(result.detail.contains(foreign ? u"other.example"_q : u"302"_q));
	}
}

TEST_CASE(TelegramStickerPackNetworkFailureLogsRedactedDiagnostics) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	auto requestUrl = QString();
	Test::Unit::ClearCapturedLogs();
	const auto result = RunStickerDownloadFailure([&](const QUrl &url) {
		auto response = StickerImportNetwork::Response{0, {}};
		response.networkError = QNetworkReply::ConnectionRefusedError;
		requestUrl = url.toString(QUrl::FullyEncoded);
		response.errorString = u"Connection failed: "_q + requestUrl;
		return response;
	});
	const auto logs = Test::Unit::TakeCapturedLogs();
	CHECK(result.error == Api::TelegramStickerPackDownloadError::Network);
	CHECK_EQ(result.requestCount, 1);
	CHECK(logs.contains(u"Network"_q));
	CHECK(logs.contains(u"HTTP 0"_q));
	CHECK(logs.contains(u"Connection failed"_q));
	CHECK(logs.contains(u"[redacted]"_q));
	CHECK(!logs.contains(token));
	CHECK(!logs.contains(requestUrl));
	CHECK(!logs.contains(u"bot12345:"_q));
}

TEST_CASE(TelegramStickerPackStopsAtOversizedResponses) {
	using Error = Api::TelegramStickerPackDownloadError;
	for (const auto &header : {false, true}) {
		const auto result = RunStickerDownloadFailure([&](const QUrl &) {
			auto response = StickerImportNetwork::Response{
				200,
				header ? QByteArray() : QByteArray(4 * 1024 * 1024 + 1, 'x')};
			if (header) {
				response.contentLength = 4 * 1024 * 1024 + 1;
			}
			return response;
		});
		CHECK(result.error == Error::InvalidResponse);
		CHECK_EQ(result.requestCount, 1);
	}
}

TEST_CASE(TelegramStickerPackStopsOnUnsafeOrOversizedFile) {
	using Error = Api::TelegramStickerPackDownloadError;
	for (const auto &mode : {0, 1, 2, 3}) {
		const auto result = RunStickerDownloadFailure([&](const QUrl &url) {
			if (url.path().endsWith(u"/getStickerSet"_q)) {
				return StickerImportNetwork::Response{
					200, StickerSetResponse(u"🐈"_q)};
			} else if (url.path().endsWith(u"/getFile"_q)) {
				auto info = QJsonObject();
				info.insert(u"file_path"_q, mode == 0 ? u"../cat.tgs"_q
													  : u"stickers/cat.tgs"_q);
				info.insert(u"file_size"_q, mode == 1 ? 65537 : 1);
				auto object = QJsonObject();
				object.insert(u"ok"_q, true);
				object.insert(u"result"_q, info);
				return StickerImportNetwork::Response{200, JsonBytes(object)};
			}
			auto response = StickerImportNetwork::Response{
				200, mode == 2 ? QByteArray(65537, 'x') : QByteArray()};
			if (mode == 3) {
				response.contentLength = 65537;
			}
			return response;
		});
		CHECK(result.error
			  == (mode == 0 ? Error::UnsafeFilePath : Error::FileTooLarge));
		CHECK_EQ(result.requestCount, mode >= 2 ? 3 : 2);
	}
}

TEST_CASE(TelegramStickerPackStopsWhenTotalBudgetIsExhausted) {
	const auto result = RunStickerDownloadFailure(
		[](const QUrl &url) {
			if (url.path().endsWith(u"/getStickerSet"_q)) {
				return StickerImportNetwork::Response{
					200, StickerSetResponse(u"🐈"_q)};
			} else if (url.path().endsWith(u"/getFile"_q)) {
				auto info = QJsonObject();
				info.insert(u"file_path"_q, u"stickers/cat.tgs"_q);
				info.insert(u"file_size"_q, 11);
				auto object = QJsonObject();
				object.insert(u"ok"_q, true);
				object.insert(u"result"_q, info);
				return StickerImportNetwork::Response{200, JsonBytes(object)};
			}
			return StickerImportNetwork::Response{200, QByteArray(11, 'x')};
		},
		quint64(110) * 1024 * 1024 - 10);
	CHECK(result.error == Api::TelegramStickerPackDownloadError::TotalTooLarge);
	CHECK_EQ(result.requestCount, 3);
}

TEST_CASE(TelegramStickerPackCancellationStopsCallbacksAndRequests) {
	const auto link = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/Cats"_q);
	CHECK(link.has_value());
	if (!link) {
		return;
	}
	auto network = StickerImportNetwork([](const QUrl &) {
		return StickerImportNetwork::Response{200, StickerSetResponse(u"🐈"_q)};
	});
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(), &network);
	auto callbacks = 0;
	downloader.start(
		[&](QString, Core::StickerImportScan) { ++callbacks; },
		[&](Api::TelegramStickerPackDownloadError, QString) { ++callbacks; },
		[&](int, int) { ++callbacks; });
	downloader.cancel();
	auto loop = QEventLoop();
	QTimer::singleShot(0, &loop, &QEventLoop::quit);
	loop.exec();
	CHECK_EQ(callbacks, 0);
	CHECK_EQ(int(network.requests.size()), 1);
}

TEST_CASE(TelegramStickerPackCancellationDuringProgressStopsRequests) {
	const auto link = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/Cats"_q);
	CHECK(link.has_value());
	if (!link) {
		return;
	}
	auto network = StickerImportNetwork([](const QUrl &) {
		return StickerImportNetwork::Response{200, StickerSetResponse(u"🐈"_q)};
	});
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(), &network);
	auto loop = QEventLoop();
	auto completed = 0;
	auto progressCalls = 0;
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	downloader.start(
		[&](QString, Core::StickerImportScan) { ++completed; },
		[&](Api::TelegramStickerPackDownloadError, QString) { ++completed; },
		[&](int, int) {
			++progressCalls;
			downloader.cancel();
			loop.quit();
		});
	loop.exec();
	CHECK_EQ(progressCalls, 1);
	CHECK_EQ(completed, 0);
	CHECK_EQ(int(network.requests.size()), 1);
}

TEST_CASE(TelegramStickerPackImportFlowKeepsTokenOffTeagramAndYieldsToUi) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	const auto link = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/Cats"_q);
	CHECK(link.has_value());
	if (!link) {
		return;
	}
	const auto sticker = ValidStickerTGS();
	auto set = QJsonObject();
	set.insert(u"name"_q, u"Cats"_q);
	set.insert(u"title"_q, u"Cats "_q + token);
	set.insert(u"sticker_type"_q, u"regular"_q);
	auto stickerInfo = QJsonObject();
	stickerInfo.insert(u"file_id"_q, u"file-1"_q);
	stickerInfo.insert(u"file_unique_id"_q, u"unique-1"_q);
	stickerInfo.insert(u"type"_q, u"regular"_q);
	stickerInfo.insert(u"width"_q, 512);
	stickerInfo.insert(u"height"_q, 512);
	stickerInfo.insert(u"is_animated"_q, true);
	stickerInfo.insert(u"is_video"_q, false);
	stickerInfo.insert(u"emoji"_q, token);
	set.insert(u"stickers"_q, QJsonArray{stickerInfo});
	auto setResult = QJsonObject();
	setResult.insert(u"ok"_q, true);
	setResult.insert(u"result"_q, set);
	auto fileInfo = QJsonObject();
	fileInfo.insert(u"file_path"_q, u"stickers/cat.tgs"_q);
	fileInfo.insert(u"file_size"_q, double(sticker.size()));
	auto fileResultObject = QJsonObject();
	fileResultObject.insert(u"ok"_q, true);
	fileResultObject.insert(u"result"_q, fileInfo);
	auto loop = QEventLoop();
	auto downloadCompleted = false;
	auto createRequestCaptured = false;
	auto markerRanBeforeCompletion = false;
	auto network = StickerImportNetwork([&](const QUrl &url) {
		if (url.path().endsWith(u"/getStickerSet"_q)) {
			return StickerImportNetwork::Response{200, JsonBytes(setResult)};
		} else if (url.path().endsWith(u"/getFile"_q)) {
			return StickerImportNetwork::Response{200,
												  JsonBytes(fileResultObject)};
		}
		auto response = StickerImportNetwork::Response{200, sticker};
		response.beforeFinish = [&] {
			QMetaObject::invokeMethod(
				&loop, [&] { markerRanBeforeCompletion = !downloadCompleted; },
				Qt::QueuedConnection);
		};
		return response;
	});
	auto title = QString();
	auto outgoing = std::vector<QByteArray>();
	auto uploadPayloads = std::vector<QByteArray>();
	auto failure = QString();
	const auto uploadedDocument = MTP_document(
		MTP_flags(0), MTP_long(1), MTP_long(2),
		MTP_bytes(QByteArray("file-reference")), MTP_int(1),
		MTP_string(u"application/x-tgsticker"_q), MTP_long(sticker.size()),
		MTP_vector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
		MTP_vector<MTPDocumentAttribute>());
	const auto uploadedMedia = MTP_messageMediaDocument(
		MTP_flags(MTPDmessageMediaDocument::Flag::f_document), uploadedDocument,
		MTPVector<MTPDocument>(), MTPPhoto(), MTPint(), MTPint());
	auto transport = Api::details::StickerSetImportFlowCallbacks{
		.upload =
			[&](const Core::StickerImportFile &file, int, int,
				Fn<void(MTPInputFile)> done, Fn<void()>, Fn<void(int)>) {
				uploadPayloads.push_back(file.bytes);
				done(MTP_inputFile(MTP_long(1), MTP_int(1),
								   MTP_string(u"sticker_1.tgs"_q),
								   MTP_string(u"checksum"_q)));
			},
		.uploadMedia =
			[&](MTPmessages_UploadMedia request, Fn<void(MTPMessageMedia)> done,
				Fn<void(QString)>) {
				outgoing.push_back(SerializeRequest(request));
				done(uploadedMedia);
			},
		.createSet =
			[&](MTPstickers_CreateStickerSet request,
				Fn<void(MTPmessages_StickerSet)>, Fn<void(QString)>) {
				outgoing.push_back(SerializeRequest(request));
				createRequestCaptured = true;
				loop.quit();
			},
	};
	auto importer = std::unique_ptr<Api::details::StickerSetImportFlow>();
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(token), &network);
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	downloader.start(
		[&](QString packTitle, Core::StickerImportScan scan) {
			downloadCompleted = true;
			title = std::move(packTitle);
			CHECK_EQ(int(scan.files.size()), 1);
			importer = std::make_unique<Api::details::StickerSetImportFlow>(
				std::move(scan), title, u"cats_a"_q,
				Data::StickersType::Stickers,
				Core::StickerImportSource::Telegram, link->name,
				std::move(transport));
			importer->start([](const MTPmessages_StickerSet &) {},
							[&](QString detail) {
								failure = std::move(detail);
								loop.quit();
							});
		},
		[&](Api::TelegramStickerPackDownloadError, QString detail) {
			failure = std::move(detail);
			loop.quit();
		},
		nullptr);
	loop.exec();
	if (importer) {
		importer->cancel();
	}
	CHECK(downloadCompleted);
	CHECK(createRequestCaptured);
	CHECK(failure.isEmpty());
	CHECK(!title.contains(token));
	CHECK_EQ(int(network.requests.size()), 3);
	CHECK_EQ(int(outgoing.size()), 2);
	CHECK(markerRanBeforeCompletion);
	for (const auto &request : outgoing) {
		CHECK(!request.contains(token.toUtf8()));
	}
	for (const auto &payload : uploadPayloads) {
		CHECK(!payload.contains(token.toUtf8()));
	}
}

TEST_CASE(TelegramCustomEmojiImportPreservesOrderAndSource) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	const auto link
		= Core::ParseTelegramStickerPackLink(u"t.me/addemoji/Emoji_pack"_q);
	CHECK(link.has_value());
	if (!link) {
		return;
	}
	const auto firstBytes = Gzip(
		R"({"tgs":1,"w":100,"h":100,"fr":30,"ip":0,"op":60,"nm":"first"})");
	const auto secondBytes = Gzip(
		R"({"tgs":1,"w":100,"h":100,"fr":30,"ip":0,"op":60,"nm":"second"})");
	const auto sticker = [](QString id, QString file, QString emoji) {
		auto result = QJsonObject();
		result.insert(u"custom_emoji_id"_q, id);
		result.insert(u"file_id"_q, file);
		result.insert(u"file_unique_id"_q, u"unique-"_q + id);
		result.insert(u"type"_q, u"custom_emoji"_q);
		result.insert(u"width"_q, 100);
		result.insert(u"height"_q, 100);
		result.insert(u"is_animated"_q, true);
		result.insert(u"is_video"_q, false);
		result.insert(u"emoji"_q, emoji);
		return result;
	};
	auto set = QJsonObject();
	set.insert(u"name"_q, u"Emoji_pack"_q);
	set.insert(u"title"_q, u"Pack "_q + token);
	set.insert(u"sticker_type"_q, u"custom_emoji"_q);
	set.insert(u"stickers"_q, QJsonArray{
								  sticker(u"101"_q, u"old-first"_q, u"🐱"_q),
								  sticker(u"202"_q, u"old-second"_q, u"🐶"_q),
							  });
	const auto response = [](QJsonValue result) {
		auto object = QJsonObject();
		object.insert(u"ok"_q, true);
		object.insert(u"result"_q, result);
		return JsonBytes(object);
	};
	auto requestedIds = QJsonArray();
	auto requestedFiles = QStringList();
	auto network = StickerImportNetwork([&](const QUrl &url) {
		const auto path = url.path();
		if (path.endsWith(u"/getStickerSet"_q)) {
			return StickerImportNetwork::Response{200, response(set)};
		} else if (path.endsWith(u"/getCustomEmojiStickers"_q)) {
			const auto ids = QUrlQuery(url).queryItemValue(
				u"custom_emoji_ids"_q, QUrl::FullyDecoded);
			requestedIds = QJsonDocument::fromJson(ids.toUtf8()).array();
			return StickerImportNetwork::Response{
				200, response(QJsonArray{
						 sticker(u"202"_q, u"resolved-second"_q, u"🐶"_q),
						 sticker(u"101"_q, u"resolved-first"_q, u"😺"_q),
					 })};
		} else if (path.endsWith(u"/getFile"_q)) {
			const auto fileId = QUrlQuery(url).queryItemValue(
				u"file_id"_q, QUrl::FullyDecoded);
			requestedFiles.push_back(fileId);
			auto file = QJsonObject();
			if (fileId == u"resolved-first"_q) {
				file.insert(u"file_path"_q, u"stickers/first.tgs"_q);
				file.insert(u"file_size"_q, int(firstBytes.size()));
			} else if (fileId == u"resolved-second"_q) {
				file.insert(u"file_path"_q, u"stickers/second.tgs"_q);
				file.insert(u"file_size"_q, int(secondBytes.size()));
			} else {
				return StickerImportNetwork::Response{500, {}};
			}
			return StickerImportNetwork::Response{200, response(file)};
		} else if (path.endsWith(u"/stickers/first.tgs"_q)) {
			return StickerImportNetwork::Response{200, firstBytes};
		} else if (path.endsWith(u"/stickers/second.tgs"_q)) {
			return StickerImportNetwork::Response{200, secondBytes};
		}
		return StickerImportNetwork::Response{500, {}};
	});
	auto loop = QEventLoop();
	auto outgoing = std::vector<QByteArray>();
	auto uploadedBytes = std::vector<QByteArray>();
	auto uploadedEmoji = std::vector<QString>();
	auto failure = QString();
	auto created = false;
	auto transport = Api::details::StickerSetImportFlowCallbacks{
		.upload =
			[&](const Core::StickerImportFile &file, int index, int,
				Fn<void(MTPInputFile)> done, Fn<void()>, Fn<void(int)>) {
				uploadedBytes.push_back(file.bytes);
				uploadedEmoji.push_back(file.emoji);
				done(MTP_inputFile(MTP_long(index + 1), MTP_int(1),
								   MTP_string(u"file.tgs"_q),
								   MTP_string(u"hash"_q)));
			},
		.uploadMedia =
			[&](MTPmessages_UploadMedia request, Fn<void(MTPMessageMedia)> done,
				Fn<void(QString)>) {
				outgoing.push_back(SerializeRequest(request));
				const auto documentId = (outgoing.size() == 1) ? 101 : 202;
				const auto document = MTP_document(
					MTP_flags(0), MTP_long(documentId), MTP_long(777),
					MTP_bytes(QByteArray("reference")), MTP_int(1),
					MTP_string(u"application/x-tgsticker"_q), MTP_long(1),
					MTP_vector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(),
					MTP_int(2), MTP_vector<MTPDocumentAttribute>());
				done(MTP_messageMediaDocument(
					MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
					document, MTPVector<MTPDocument>(), MTPPhoto(), MTPint(),
					MTPint()));
			},
		.createSet =
			[&](MTPstickers_CreateStickerSet request,
				Fn<void(MTPmessages_StickerSet)>, Fn<void(QString)>) {
				outgoing.push_back(SerializeRequest(request));
				created = true;
				loop.quit();
			},
	};
	auto importer = std::unique_ptr<Api::details::StickerSetImportFlow>();
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(token), &network);
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	downloader.start(
		[&](QString title, Core::StickerImportScan scan) {
			CHECK(scan.problem == Core::StickerImportProblem::None);
			CHECK_EQ(int(scan.files.size()), 2);
			importer = std::make_unique<Api::details::StickerSetImportFlow>(
				std::move(scan), std::move(title), u"cats_a"_q,
				Data::StickersType::Emoji, Core::StickerImportSource::Telegram,
				link->name, std::move(transport));
			importer->start([](MTPmessages_StickerSet) {},
							[&](QString error) {
								failure = std::move(error);
								loop.quit();
							});
		},
		[&](Api::TelegramStickerPackDownloadError, QString error) {
			failure = std::move(error);
			loop.quit();
		},
		nullptr);
	loop.exec();
	if (importer) {
		importer->cancel();
	}
	CHECK(created);
	CHECK(failure.isEmpty());
	CHECK_EQ(int(network.requests.size()), 6);
	const auto expectedIds = QJsonArray{u"101"_q, u"202"_q};
	CHECK(requestedIds == expectedIds);
	const auto expectedFiles
		= QStringList{u"resolved-first"_q, u"resolved-second"_q};
	CHECK(requestedFiles == expectedFiles);
	CHECK_EQ(int(uploadedBytes.size()), 2);
	CHECK_EQ(int(uploadedEmoji.size()), 2);
	if (uploadedBytes.size() == 2 && uploadedEmoji.size() == 2) {
		CHECK_EQ(uploadedBytes[0], firstBytes);
		CHECK_EQ(uploadedBytes[1], secondBytes);
		CHECK_EQ(uploadedEmoji[0], u"😺"_q);
		CHECK_EQ(uploadedEmoji[1], u"🐶"_q);
	}
	const auto firstItem = MTP_inputStickerSetItem(
		MTP_flags(0),
		MTP_inputDocument(MTP_long(101), MTP_long(777),
						  MTP_bytes(QByteArray("reference"))),
		MTP_string(u"😺"_q), MTPMaskCoords(), MTPstring());
	const auto secondItem = MTP_inputStickerSetItem(
		MTP_flags(0),
		MTP_inputDocument(MTP_long(202), MTP_long(777),
						  MTP_bytes(QByteArray("reference"))),
		MTP_string(u"🐶"_q), MTPMaskCoords(), MTPstring());
	auto flags = MTPstickers_CreateStickerSet::Flags(
		MTPstickers_CreateStickerSet::Flag::f_software);
	flags |= MTPstickers_CreateStickerSet::Flag::f_emojis;
	const auto expected = MTPstickers_CreateStickerSet(
		MTP_flags(flags), MTP_inputUserSelf(), MTP_string(u"Pack [redacted]"_q),
		MTP_string(u"cats_a"_q), MTPInputDocument(),
		MTP_vector<MTPInputStickerSetItem>(
			QVector<MTPInputStickerSetItem>{firstItem, secondItem}),
		MTP_string(u"https://t.me/addemoji/Emoji_pack"_q));
	CHECK_EQ(int(outgoing.size()), 3);
	if (outgoing.size() == 3) {
		CHECK_EQ(outgoing.back(), SerializeRequest(expected));
	}
	for (const auto &request : outgoing) {
		CHECK(!request.contains(token.toUtf8()));
	}
	for (const auto &bytes : uploadedBytes) {
		CHECK(!bytes.contains(token.toUtf8()));
	}
}

TEST_CASE(TelegramStickerPackFailedDownloadLogsRedactBotTokenAndUrl) {
	const auto token = u"12345:AbCdEf0123456789012345678901234567890"_q;
	const auto link = Core::ParseTelegramStickerPackLink(
		u"https://t.me/addstickers/Cats"_q);
	CHECK(link.has_value());
	if (!link) {
		return;
	}
	auto network = StickerImportNetwork([&](const QUrl &url) {
		DEBUG_LOG(("Sticker import debug capture sentinel."));
		auto result = QJsonObject();
		result.insert(u"ok"_q, false);
		result.insert(u"error_code"_q, 500);
		result.insert(u"description"_q,
					  u"failed request: "_q + url.toString(QUrl::FullyEncoded));
		return StickerImportNetwork::Response{500, JsonBytes(result)};
	});
	auto downloader = Api::TelegramStickerPackDownloader(
		*link, StickerTestToken(token), &network);
	auto loop = QEventLoop();
	auto failure = QString();
	Test::Unit::ClearCapturedLogs();
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	downloader.start(
		[](QString, Core::StickerImportScan) {},
		[&](Api::TelegramStickerPackDownloadError, QString detail) {
			failure = std::move(detail);
			loop.quit();
		},
		nullptr);
	loop.exec();
	const auto logs = Test::Unit::TakeCapturedLogs();
	CHECK(!failure.isEmpty());
	CHECK_EQ(int(network.requests.size()), 1);
	CHECK(logs.contains(u"Telegram sticker pack download failed"_q));
	CHECK(logs.contains(u"failed request"_q));
	CHECK(logs.contains(u"Sticker import debug capture sentinel."_q));
	CHECK(failure.contains(u"[redacted]"_q));
	CHECK(logs.contains(u"[redacted]"_q));
	CHECK(!failure.contains(token));
	CHECK(!logs.contains(token));
	if (!network.requests.isEmpty()) {
		CHECK(!logs.contains(
			network.requests.front().toString(QUrl::FullyEncoded)));
	}
}

TEST_CASE(TelegramStickerImportRequestBuildersPreserveStickerEmoji) {
	auto files = std::vector<Core::StickerImportFile>{
		{
			.name = u"sticker.tgs"_q,
			.bytes = ValidStickerTGS(),
			.emoji = u"😺"_q,
		},
	};
	const auto scan = Core::ScanStickerImportFiles(
		std::move(files), Core::StickerImportType::Stickers);
	CHECK_EQ(int(scan.files.size()), 1);
	if (scan.files.empty()) {
		return;
	}
	CHECK(scan.files.front().emoji == u"😺"_q);

	const auto inputFile
		= MTP_inputFile(MTP_long(1), MTP_int(1), MTP_string(u"sticker_1.tgs"_q),
						MTP_string(u"checksum"_q));
	const auto upload = Api::details::StickerImportUploadMediaRequest(
		inputFile, scan.files.front(), 0);
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(
		MTP_documentAttributeFilename(MTP_string(u"sticker_1.tgs"_q)));
	attributes.push_back(MTP_documentAttributeAnimated());
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0), MTP_string(u"😺"_q), MTP_inputStickerSetEmpty(),
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

	const auto item = MTP_inputStickerSetItem(
		MTP_flags(0),
		MTP_inputDocument(MTP_long(1), MTP_long(2),
						  MTP_bytes(QByteArray("file-reference"))),
		MTP_string(u"😺"_q), MTPMaskCoords(), MTPstring());
	const auto request = Api::details::StickerImportCreateSetRequest(
		u"Cats"_q, u"cats_a"_q, Data::StickersType::Stickers,
		Core::StickerImportSource::Telegram,
		QVector<MTPInputStickerSetItem>{item}, u"SomePack"_q);
	const auto expected = MTPstickers_CreateStickerSet(
		MTP_flags(MTPstickers_CreateStickerSet::Flag::f_software),
		MTP_inputUserSelf(), MTP_string(u"Cats"_q), MTP_string(u"cats_a"_q),
		MTPInputDocument(),
		MTP_vector<MTPInputStickerSetItem>(
			QVector<MTPInputStickerSetItem>{item}),
		MTP_string(u"https://t.me/addstickers/SomePack"_q));
	CHECK_EQ(SerializeRequest(request), SerializeRequest(expected));
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

TEST_CASE(FolderAndZipImportFlowPreservesSourceAndDeduplicatesProgress) {
	const auto document
		= MTP_document(MTP_flags(0), MTP_long(1), MTP_long(2),
					   MTP_bytes(QByteArray("file-reference")), MTP_int(1),
					   MTP_string(u"application/x-tgsticker"_q), MTP_long(1),
					   MTP_vector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(),
					   MTP_int(2), MTP_vector<MTPDocumentAttribute>());
	const auto media = MTP_messageMediaDocument(
		MTP_flags(MTPDmessageMediaDocument::Flag::f_document), document,
		MTPVector<MTPDocument>(), MTPPhoto(), MTPint(), MTPint());
	const auto item = MTP_inputStickerSetItem(
		MTP_flags(0),
		MTP_inputDocument(MTP_long(1), MTP_long(2),
						  MTP_bytes(QByteArray("file-reference"))),
		MTP_string(u"🐈"_q), MTPMaskCoords(), MTPstring());
	for (const auto source :
		 {Core::StickerImportSource::Folder, Core::StickerImportSource::Zip}) {
		auto scan = Core::StickerImportScan();
		scan.files.push_back({
			.name = u"cat.tgs"_q,
			.bytes = ValidStickerTGS(),
			.format = Core::StickerImportFormat::Tgs,
			.emoji = u"🐈"_q,
		});
		auto sent = QByteArray();
		auto progressValues = std::vector<int>();
		auto callbacks = Api::details::StickerSetImportFlowCallbacks{
			.upload =
				[&](const Core::StickerImportFile &, int, int,
					Fn<void(MTPInputFile)> done, Fn<void()>,
					Fn<void(int)> progress) {
					progress(10);
					progress(10);
					progress(100);
					progress(100);
					done(MTP_inputFile(MTP_long(1), MTP_int(1),
									   MTP_string(u"cat.tgs"_q),
									   MTP_string(u"checksum"_q)));
				},
			.uploadMedia
			= [&](MTPmessages_UploadMedia, Fn<void(MTPMessageMedia)> done,
				  Fn<void(QString)>) { done(media); },
			.createSet
			= [&](MTPstickers_CreateStickerSet request,
				  Fn<void(MTPmessages_StickerSet)>,
				  Fn<void(QString)>) { sent = SerializeRequest(request); },
		};
		auto flow = Api::details::StickerSetImportFlow(
			std::move(scan), u"Cats"_q, u"cats_a"_q,
			Data::StickersType::Stickers, source, QString(),
			std::move(callbacks));
		flow.start([](MTPmessages_StickerSet) {}, [](QString) {},
				   [&](int percent) { progressValues.push_back(percent); });
		const auto expected = MTPstickers_CreateStickerSet(
			MTP_flags(MTPstickers_CreateStickerSet::Flag::f_software),
			MTP_inputUserSelf(), MTP_string(u"Cats"_q), MTP_string(u"cats_a"_q),
			MTPInputDocument(),
			MTP_vector<MTPInputStickerSetItem>(
				QVector<MTPInputStickerSetItem>{item}),
			MTP_string(source == Core::StickerImportSource::Folder ? u"folder"_q
																   : u"zip"_q));
		CHECK_EQ(sent, SerializeRequest(expected));
		const auto expectedProgress = std::vector<int>{10, 100};
		CHECK(progressValues == expectedProgress);
	}
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
			  !selection.location().isEmpty(), folderScan)
		  == Core::StickerImportValidation::Ready);
	selection.selectSource(Core::StickerImportSource::Zip, zipPath);
	CHECK(selection.type() == Core::StickerImportSource::Zip);
	CHECK(!selection.rightsConfirmed());
	CHECK(Core::ValidateStickerImportStart(
			  false, selection.rightsConfirmed(), u"Cats"_q, u"cats_a"_q,
			  !selection.location().isEmpty(), scan)
		  == Core::StickerImportValidation::RightsNotConfirmed);
	selection.setRightsConfirmed(true);
	CHECK(Core::ValidateStickerImportStart(
			  false, selection.rightsConfirmed(), u"Cats"_q, u"cats_a"_q,
			  !selection.location().isEmpty(), scan)
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

} // namespace
