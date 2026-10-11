/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_download.h"

namespace Core {

enum class TeagramUpdateArchiveReason {
	Extracted,
	Cancelled,
	ArchiveRejected,
	LimitExceeded,
	StagingFailure,
	WriteFailure,
};

struct TeagramUpdateArchiveResult {
	TeagramUpdateArchiveReason reason
		= TeagramUpdateArchiveReason::ArchiveRejected;
	QString bundlePath;

	[[nodiscard]] bool extracted() const {
		return reason == TeagramUpdateArchiveReason::Extracted
			&& !bundlePath.isEmpty();
	}
};

[[nodiscard]] TeagramUpdateArchiveResult
ExtractAuthenticatedTeagramUpdateArchive(
	const TeagramUpdateCandidate &candidate,
	const QString &privateStagingDirectory,
	const std::function<bool()> &isCancelled = {},
	const std::function<void(int)> &progress = {});

} // namespace Core
