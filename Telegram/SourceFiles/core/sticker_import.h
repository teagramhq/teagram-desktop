/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>

#include <vector>

namespace Core {

enum class StickerImportType {
	Stickers,
	Emoji,
};

enum class StickerImportFormat {
	Webp,
	Tgs,
	Webm,
};

struct StickerImportFormatInfo {
	QString mimeType;
	QString extension;
	quint64 maximumBytes = 0;
};

enum class StickerImportSource {
	Folder,
	Zip,
};

enum class StickerImportProblem {
	None,
	InvalidSource,
	InvalidArchive,
	TooManyEntries,
	TotalSizeLimit,
	NoFiles,
	FileTooLarge,
	UnsupportedFormat,
	InvalidMedia,
	InvalidDimensions,
	TooManyItems,
	ReadFailed,
	SymbolicLink,
};

struct StickerImportFile {
	QString name;
	QByteArray bytes;
	StickerImportFormat format = StickerImportFormat::Webp;
	QSize dimensions;
};

struct StickerImportIssue {
	QString name;
	StickerImportProblem problem = StickerImportProblem::None;
};

struct StickerImportScan {
	std::vector<StickerImportFile> files;
	std::vector<StickerImportIssue> issues;
	StickerImportProblem problem = StickerImportProblem::None;
};

class StickerImportSelection final {
  public:
	void selectSource(StickerImportSource type, QString path);
	void setRightsConfirmed(bool confirmed);

	[[nodiscard]] const QString &path() const;
	[[nodiscard]] StickerImportSource type() const;
	[[nodiscard]] bool rightsConfirmed() const;

  private:
	QString _path;
	StickerImportSource _type = StickerImportSource::Folder;
	bool _rightsConfirmed = false;
};

enum class StickerImportValidation {
	Ready,
	ScanPending,
	RightsNotConfirmed,
	InvalidTitle,
	InvalidShortName,
	InvalidSource,
	InvalidScan,
};

class StickerImportSuggestionGuard final {
  public:
	struct Request {
		quint64 generation = 0;
		QString title;
		QString shortName;
	};

	[[nodiscard]] Request begin(QString title, QString shortName);
	void invalidate();
	[[nodiscard]] bool isCurrent(const Request &request, const QString &title,
								 const QString &shortName) const;

  private:
	quint64 _generation = 0;
};

[[nodiscard]] StickerImportScan ScanStickerImportFolder(const QString &path,
														StickerImportType type);
[[nodiscard]] StickerImportScan ScanStickerImportZip(const QByteArray &bytes,
													 StickerImportType type);
[[nodiscard]] StickerImportScan
ScanStickerImportZipFile(const QString &path, StickerImportType type);
[[nodiscard]] StickerImportFormatInfo
StickerImportFormatDetails(StickerImportFormat format);
[[nodiscard]] QString StickerImportShareLinkPath(StickerImportType type,
												 const QString &shortName);
[[nodiscard]] bool IsValidStickerSetTitle(const QString &title);
[[nodiscard]] bool IsValidStickerSetShortName(const QString &name);
[[nodiscard]] StickerImportValidation
ValidateStickerImportStart(bool scanPending, bool rightsConfirmed,
						   const QString &title, const QString &shortName,
						   bool hasSource, const StickerImportScan &scan);

} // namespace Core
