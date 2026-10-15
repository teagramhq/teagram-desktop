/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#if (defined(Q_OS_MAC) && defined(TDESKTOP_TEAGRAM))                           \
	|| defined(TDESKTOP_UNIT_TESTS)

#include "core/teagram_update_download.h"
#include "core/teagram_update_install.h"
#include "core/teagram_update_state.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QThread>

#include <crl/crl_time.h>
#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <memory>

namespace Core {

class TeagramUpdateInstallJob {
  public:
	virtual ~TeagramUpdateInstallJob() = default;

	virtual void cancel() = 0;
	[[nodiscard]] virtual TeagramUpdateInstallResult
	install(const TeagramUpdateCandidate &candidate,
			TeagramUpdateInstallProgressHandler progress,
			TeagramUpdateBeforeRelaunch beforeRelaunch) = 0;
};

// Everything the manager needs from the running application. Production
// supplies the only implementation with the fixed release source and the
// provisioned public key; unit tests supply fixtures.
class TeagramUpdateManagerEnvironment {
  public:
	virtual ~TeagramUpdateManagerEnvironment() = default;

	[[nodiscard]] virtual quint64 runningBuild() const = 0;
	[[nodiscard]] virtual TeagramUpdateChannel compiledChannel() const = 0;
	[[nodiscard]] virtual TeagramUpdateChannel readChannel() = 0;
	virtual void writeChannel(TeagramUpdateChannel channel) = 0;
	[[nodiscard]] virtual quint64 readHighWater() = 0;
	virtual void writeCommittedHighWater(quint64 build) = 0;
	[[nodiscard]] virtual bool readUpdatedToastPending() = 0;
	[[nodiscard]] virtual bool showUpdatedToast(quint64 build) = 0;
	[[nodiscard]] virtual bool automatic() const = 0;
	virtual void writeAutomatic(bool enabled) = 0;
	[[nodiscard]] virtual TeagramUpdateActiveWork activeWork() const = 0;
	[[nodiscard]] virtual crl::time now() const = 0;
	virtual void restart() = 0;
	[[nodiscard]] virtual TeagramUpdateInstallLocation installLocation() const
		= 0;
	[[nodiscard]] virtual bool supportsMinimumOs(const QString &minimum) const
		= 0;
	[[nodiscard]] virtual bool previousBundleAvailable() const = 0;
	[[nodiscard]] virtual bool copyDataBeforeOlderBuild() = 0;
	virtual void showPreviousBundle() const = 0;
	[[nodiscard]] virtual std::shared_ptr<TeagramUpdateDownloader>
	createDownloader() = 0;
	[[nodiscard]] virtual std::shared_ptr<TeagramUpdateInstallJob>
	createInstaller() = 0;
	[[nodiscard]] virtual crl::time checkInterval() const = 0;
	[[nodiscard]] virtual crl::time safetyInterval() const = 0;
};

class TeagramUpdateManager final : public QObject {
  public:
	[[nodiscard]] static TeagramUpdateManager &Instance();
#ifdef TDESKTOP_UNIT_TESTS
	[[nodiscard]] static std::unique_ptr<TeagramUpdateManager>
	CreateForTesting(std::unique_ptr<TeagramUpdateManagerEnvironment> env);
#endif // TDESKTOP_UNIT_TESTS

	void start();
	void checkNow();
	void setAutomatic(bool enabled);
	void setChannel(TeagramUpdateChannel channel);
	void performSettingsAction();
	void downloadAndInstall();
	void cancel();
	void restartExplicitly();
	void dontRestart();
	void noteUserInput();
	void showPreviousBundleInFinder() const;
	void showUpdatedToastIfNeeded();
	~TeagramUpdateManager() override;

	[[nodiscard]] const TeagramUpdateSnapshot &state() const;
	[[nodiscard]] rpl::producer<TeagramUpdateSnapshot> stateChanges() const;

  private:
	struct Job;

	explicit TeagramUpdateManager(
		std::unique_ptr<TeagramUpdateManagerEnvironment> env);

	void beginCheck(bool automatic);
	void beginDownload();
	void beginInstall(bool automatic);
	void updateSafety();
	void maybeRestart();
	void shutdown();
	void setState(TeagramUpdateUiState state);
	[[nodiscard]] TeagramUpdateActiveWork activeWork() const;
	[[nodiscard]] std::chrono::milliseconds idleFor() const;
	void persistCommittedBuild(quint64 build, bool replacementCommitted,
							   bool candidateOlder);

	const std::unique_ptr<TeagramUpdateManagerEnvironment> _env;
	TeagramUpdateSnapshot _state;
	rpl::event_stream<TeagramUpdateSnapshot> _stateChanges;
	QTimer _periodicTimer;
	QTimer _safetyTimer;
	QPointer<QThread> _worker;
	std::shared_ptr<Job> _job;
	std::shared_ptr<TeagramUpdateCandidate> _candidate;
	crl::time _lastInputAt = 0;
	quint64 _suppressedAutomaticBuild = 0;
	bool _started = false;
	bool _channelSwitchCheck = false;
	bool _pendingExplicitRestart = false;
	bool _startCheckAfterWorker = false;
	bool _startDownloadAfterWorker = false;
	bool _automaticJob = false;
	bool _automaticInstalledRestartPending = false;
	bool _automaticRecoveryRestartPending = false;
	bool _installCommittedInSession = false;
	bool _updatedToastPending = false;
	TeagramUpdateUiState _pendingRestartState = TeagramUpdateUiState::Ready;
	TeagramUpdateDownloadStage _downloadStage
		= TeagramUpdateDownloadStage::Discovering;
	int _generation = 0;
};

} // namespace Core

#endif // (Q_OS_MAC && TDESKTOP_TEAGRAM) || TDESKTOP_UNIT_TESTS
