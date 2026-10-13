/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <functional>

namespace Core {

[[nodiscard]] bool WriteTeagramUpdateMarker(const QString &parent,
											const char *name, quint64 build);

class TeagramUpdateInstallLock final {
  public:
	explicit TeagramUpdateInstallLock(const QString &installedBundlePath);
	~TeagramUpdateInstallLock();

	TeagramUpdateInstallLock(TeagramUpdateInstallLock &&other) noexcept;
	TeagramUpdateInstallLock &
	operator=(TeagramUpdateInstallLock &&other) noexcept;
	TeagramUpdateInstallLock(const TeagramUpdateInstallLock &) = delete;
	TeagramUpdateInstallLock &operator=(const TeagramUpdateInstallLock &)
		= delete;

	[[nodiscard]] bool acquired() const;
	[[nodiscard]] const QString &directory() const;

  private:
	QString _directory;
	int _descriptor = -1;
};

enum class TeagramUpdateReplacementReason {
	Replaced,
	Busy,
	InvalidPath,
	PreviousBundleExists,
	Cancelled,
	InjectedFailure,
	ReplacementFailed,
	RecoveryRequired,
};

struct TeagramUpdateReplacementResult {
	TeagramUpdateReplacementReason reason
		= TeagramUpdateReplacementReason::InvalidPath;
	QString previousBundlePath;
	bool committed = false;
};

enum class TeagramUpdateStartupRecoveryReason {
	NoPending,
	RestoredPrevious,
	IncompleteCandidateRemoved,
	PreviousRetained,
	PreviousRemoved,
	Busy,
	Failed,
};

struct TeagramUpdateStartupRecoveryResult {
	TeagramUpdateStartupRecoveryReason reason
		= TeagramUpdateStartupRecoveryReason::NoPending;
	QString previousBundlePath;
};

#ifdef TDESKTOP_UNIT_TESTS
enum class TeagramUpdateReplacementFailurePoint {
	BeforePreviousMove,
	AfterPreviousMove,
	BeforeCandidateMove,
	RollbackSwapFailure,
	RollbackSyncFailure,
	CommittedSyncFailure,
};

using TeagramUpdateReplacementFailureInjector
	= std::function<bool(TeagramUpdateReplacementFailurePoint)>;
#endif // TDESKTOP_UNIT_TESTS

[[nodiscard]] TeagramUpdateReplacementResult CommitTeagramUpdateReplacement(
	const TeagramUpdateInstallLock &lock, const QString &installedBundlePath,
	const QString &stagedBundlePath, const QString &previousBundlePath,
	const std::function<bool()> &isCancelled = {}
#ifdef TDESKTOP_UNIT_TESTS
	,
	const TeagramUpdateReplacementFailureInjector &failureInjector = {}
#endif // TDESKTOP_UNIT_TESTS
);

[[nodiscard]] TeagramUpdateStartupRecoveryResult
RecoverTeagramUpdateReplacement(const TeagramUpdateInstallLock &lock,
								const QString &installedBundlePath,
								const QString &stagingDirectory,
								const QString &previousBundlePath,
								const QString &runningBundlePath,
								bool stagingBundleIsPrevious, bool startupReady,
								bool noOtherInstance);

} // namespace Core
