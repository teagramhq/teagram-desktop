/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <algorithm>
#include <optional>

namespace Storage::details {

[[nodiscard]] inline int
UploadPartSizeWithinServerLimit(int requestedSize,
								std::optional<int64> maxBytes) {
	return (maxBytes && *maxBytes > 0)
			   ? int(std::min<int64>(requestedSize, *maxBytes))
			   : requestedSize;
}

[[nodiscard]] inline bool ExceedsUploadMaxBytes(int64 fileSize,
												std::optional<int64> maxBytes) {
	return maxBytes && *maxBytes >= 0 && fileSize > *maxBytes;
}

} // namespace Storage::details
