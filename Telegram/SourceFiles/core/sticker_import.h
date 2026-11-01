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
	StickerImportFormat format;
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

} // namespace Core
