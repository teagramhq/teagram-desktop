/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

namespace Data::details {

[[nodiscard]] inline bool IsValidDocumentDownloadSize(std::int64_t size) {
	return size >= 0;
}

} // namespace Data::details
