/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#if defined(Q_OS_MAC) && defined(TDESKTOP_TEAGRAM)

#include "core/teagram_update_download.h"
#include "core/teagram_update_install.h"
#include "core/teagram_update_state.h"
#include "base/timer.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QThread>

#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <memory>

namespace Core {

class TeagramUpdateManager final : public QObject {
public:
	[[nodiscard]] static TeagramUpdateManager &Instance();

	void start();
	void checkNow();
	void setAutomatic(bool enabled);
	void setChannel(TeagramUpdateChannel channel);
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

	TeagramUpdateManager();

	void beginCheck(bool automatic);
	void beginDownload();
	void beginInstall(bool automatic);
	void updateSafety();
	void maybeRestart();
	void setState(TeagramUpdateUiState state);
	[[nodiscard]] TeagramUpdateActiveWork activeWork() const;
	[[nodiscard]] std::chrono::milliseconds idleFor() const;
	void persistCommittedBuild(quint64 build);

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
	bool _pendingExplicitRestart = false;
	bool _startCheckAfterWorker = false;
	bool _startDownloadAfterWorker = false;
	bool _automaticJob = false;
	bool _automaticInstalledRestartPending = false;
	bool _updatedToastPending = false;
	TeagramUpdateUiState _pendingRestartState = TeagramUpdateUiState::Ready;
	TeagramUpdateDownloadStage _downloadStage
		= TeagramUpdateDownloadStage::Discovering;
	int _generation = 0;
};

} // namespace Core

#endif // Q_OS_MAC && TDESKTOP_TEAGRAM
