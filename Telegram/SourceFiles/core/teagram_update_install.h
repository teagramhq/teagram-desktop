/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_archive.h"

#include <atomic>
#include <functional>
#include <optional>
#include <utility>

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

#ifdef TDESKTOP_UNIT_TESTS
struct TeagramUpdateInstallerTestHooks {
	QString runningBundleIdentifier;
	QString runningBundlePath;
	std::function<bool(const QString &)> hasArchitecture;
	std::function<bool(const QString &)> hasCleanCodeSeal;
	std::function<bool()> otherInstanceRunning;
	std::function<bool(const QString &)> launchBundle;
};
#endif // TDESKTOP_UNIT_TESTS

class TeagramUpdateInstaller final {
  public:
	TeagramUpdateInstaller() = default;
#ifdef TDESKTOP_UNIT_TESTS
	explicit TeagramUpdateInstaller(TeagramUpdateInstallerTestHooks hooks)
		: _testHooks(std::move(hooks)) {
	}
#endif // TDESKTOP_UNIT_TESTS

	void cancel();

	[[nodiscard]] TeagramUpdateInstallResult install(
		const TeagramUpdateCandidate &candidate,
		TeagramUpdateInstallProgressHandler progress = {});

  private:
	std::atomic_bool _cancelled = false;
	std::atomic_bool _started = false;
#ifdef TDESKTOP_UNIT_TESTS
	std::optional<TeagramUpdateInstallerTestHooks> _testHooks;
#endif // TDESKTOP_UNIT_TESTS
};

void CompleteTeagramUpdateStartup();

} // namespace Core
