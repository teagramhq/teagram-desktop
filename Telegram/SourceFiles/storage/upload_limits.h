/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

#include <optional>

namespace Storage::details {

[[nodiscard]] inline bool ExceedsUploadMaxBytes(int64 fileSize,
												std::optional<int64> maxBytes) {
	return maxBytes && *maxBytes >= 0 && fileSize > *maxBytes;
}

[[nodiscard]] inline bool IsFilePartTooBigError(const QString &type) {
	return (type == u"FILE_PART_TOO_BIG"_q);
}

} // namespace Storage::details
