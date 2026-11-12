/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

namespace Data::details {

enum class DocumentDownloadDecision {
	Fail,
	Cache,
	Stream,
};

[[nodiscard]] inline DocumentDownloadDecision
DecideDocumentDownload(std::int64_t size, bool hasDestination,
					   std::int64_t maxFileInMemory) {
	if (size < 0) {
		return DocumentDownloadDecision::Fail;
	} else if (!hasDestination && size > maxFileInMemory) {
		return DocumentDownloadDecision::Cache;
	}
	return DocumentDownloadDecision::Stream;
}

} // namespace Data::details
