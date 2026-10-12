/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_archive.h"
#include "core/teagram_update_replacement.h"

#include <QtCore/QStringList>

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
	RefusedTranslocated,
	RefusedUnwritable,
	RefusedOtherOwner,
	RefusedUnsupportedDisk,
	InstalledBuildChanged,
	CandidateRejected,
	ArchiveRejected,
	ReplacementFailed,
	RecoveryRequired,
	RelaunchFailedPreviousAvailable,
};

enum class TeagramUpdateInstallLocation {
	Allowed,
	Translocated,
	Unwritable,
	OtherOwner,
	UnsupportedDisk,
};

enum class TeagramUpdateRelaunchResult {
	NotPending,
	CandidateStarted,
	CurrentBundleStarted,
	PreviousStarted,
	Failed,
};

struct TeagramUpdateInstallResult {
	TeagramUpdateInstallReason reason
		= TeagramUpdateInstallReason::InvalidTarget;
	QString previousBundlePath;
	bool profileRollbackAvailable = false;
	bool replacementCommitted = false;
};

using TeagramUpdateInstallProgressHandler
	= std::function<void(TeagramUpdateInstallStage, int)>;
using TeagramUpdateBeforeRelaunch
	= std::function<bool(const std::function<bool()> &)>;

#ifdef TDESKTOP_UNIT_TESTS
struct TeagramUpdateInstallerTestHooks {
	QString runningBundleIdentifier;
	QString runningBundlePath;
	std::function<bool(const QString &)> hasArchitecture;
	std::function<bool(const QString &)> hasCleanCodeSeal;
	std::function<bool()> otherInstanceRunning;
	std::function<bool()> requestRestart;
	TeagramUpdateReplacementFailureInjector replacementFailureInjector;
};

[[nodiscard]] TeagramUpdateStartupRecoveryResult
RecoverTeagramUpdateInstallerAtStartupForTesting(
	const QString &installedBundlePath, const QString &stagingDirectory,
	const QString &previousBundlePath, const QString &runningBundlePath,
	const std::function<bool()> &otherInstanceRunning = {});

[[nodiscard]] bool CompleteTeagramUpdateStartupForTesting(
	const QString &runningBundlePath,
	const std::function<bool()> &otherInstanceRunning = {});

[[nodiscard]] TeagramUpdateRelaunchResult
RelaunchTeagramUpdateAfterShutdownForTesting(
	const QString &currentBundlePath, const QString &dataFile,
	const std::function<bool(const QString &, const QStringList &, qint64 *)>
		&launchBundle,
	const std::function<bool()> &otherInstanceRunning = {},
	const std::function<void()> &prepareForLaunch = {},
	const std::function<bool()> &candidateStartupAcknowledged = {},
	const std::function<bool(qint64)> &stopCandidate = {});
#endif // TDESKTOP_UNIT_TESTS

class TeagramUpdateInstaller final {
  public:
	TeagramUpdateInstaller() = default;
#ifdef TDESKTOP_UNIT_TESTS
	explicit TeagramUpdateInstaller(TeagramUpdateInstallerTestHooks hooks)
		: _testHooks(std::move(hooks)) {}
#endif // TDESKTOP_UNIT_TESTS

	void cancel();

	[[nodiscard]] TeagramUpdateInstallResult
	install(const TeagramUpdateCandidate &candidate,
			TeagramUpdateInstallProgressHandler progress = {},
			TeagramUpdateBeforeRelaunch beforeRelaunch = {});

  private:
	std::atomic_bool _cancelled = false;
	std::atomic_bool _started = false;
#ifdef TDESKTOP_UNIT_TESTS
	std::optional<TeagramUpdateInstallerTestHooks> _testHooks;
#endif // TDESKTOP_UNIT_TESTS
};

void CompleteTeagramUpdateStartup();

[[nodiscard]] TeagramUpdateInstallLocation CheckTeagramUpdateInstallLocation();
[[nodiscard]] bool HasPreviousTeagramUpdateBundle();
void ShowPreviousTeagramUpdateBundleInFinder();

[[nodiscard]] TeagramUpdateRelaunchResult
RelaunchTeagramUpdateAfterShutdown(const QString &dataFile,
								   const std::function<void()> &prepareForLaunch
								   = {});

} // namespace Core
