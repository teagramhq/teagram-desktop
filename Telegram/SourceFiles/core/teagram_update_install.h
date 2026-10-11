/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_archive.h"

#include <atomic>

namespace Core {

enum class TeagramUpdateInstallStage {
	Verifying,
	Extracting,
	Replacing,
	Relaunching,
};

enum class TeagramUpdateInstallReason {
	RelaunchRequested,
	Busy,
	Cancelled,
	InvalidTarget,
	InstalledBuildChanged,
	CandidateRejected,
	ArchiveRejected,
	ReplacementFailed,
	RecoveryRequired,
	RelaunchFailedPreviousAvailable,
};

struct TeagramUpdateInstallResult {
	TeagramUpdateInstallReason reason
		= TeagramUpdateInstallReason::InvalidTarget;
	QString previousBundlePath;
	bool profileRollbackAvailable = false;
};

using TeagramUpdateInstallProgressHandler
	= std::function<void(TeagramUpdateInstallStage, int)>;

class TeagramUpdateInstaller final {
  public:
	void cancel();

	[[nodiscard]] TeagramUpdateInstallResult install(
		const TeagramUpdateCandidate &candidate,
		TeagramUpdateInstallProgressHandler progress = {});

  private:
	std::atomic_bool _cancelled = false;
	std::atomic_bool _started = false;
};

void CompleteTeagramUpdateStartup();

} // namespace Core
