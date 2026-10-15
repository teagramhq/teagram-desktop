/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_manager.h"

#if (defined(Q_OS_MAC) && defined(TDESKTOP_TEAGRAM))                           \
	|| defined(TDESKTOP_UNIT_TESTS)

#include "core/teagram_update_worker.h"
#include "logs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>

#include <algorithm>
#include <utility>

namespace Core {
namespace {

[[nodiscard]] TeagramUpdateRefusal
RefusalFor(TeagramUpdateInstallLocation location) {
	switch (location) {
	case TeagramUpdateInstallLocation::Translocated:
		return TeagramUpdateRefusal::Translocated;
	case TeagramUpdateInstallLocation::Unwritable:
		return TeagramUpdateRefusal::Unwritable;
	case TeagramUpdateInstallLocation::OtherOwner:
		return TeagramUpdateRefusal::OtherOwner;
	case TeagramUpdateInstallLocation::UnsupportedDisk:
		return TeagramUpdateRefusal::UnsupportedDisk;
	case TeagramUpdateInstallLocation::Allowed:
		break;
	}
	return TeagramUpdateRefusal::None;
}

[[nodiscard]] QString CheckedTime() {
	return QDateTime::currentDateTime().toString(u"HH:mm"_q);
}

} // namespace

struct TeagramUpdateManager::Job {
	std::shared_ptr<TeagramUpdateDownloader> downloader;
	std::shared_ptr<TeagramUpdateInstallJob> installer;
	std::shared_ptr<TeagramUpdateCandidate> candidate;
};

#ifdef TDESKTOP_UNIT_TESTS
std::unique_ptr<TeagramUpdateManager> TeagramUpdateManager::CreateForTesting(
	std::unique_ptr<TeagramUpdateManagerEnvironment> env) {
	return std::unique_ptr<TeagramUpdateManager>(
		new TeagramUpdateManager(std::move(env)));
}
#endif // TDESKTOP_UNIT_TESTS

TeagramUpdateManager::TeagramUpdateManager(
	std::unique_ptr<TeagramUpdateManagerEnvironment> env)
	: QObject(nullptr), _env(std::move(env)) {
	if (const auto application = QCoreApplication::instance()) {
		connect(application, &QCoreApplication::aboutToQuit, this,
				[=] { shutdown(); });
	}
	_periodicTimer.setInterval(int(_env->checkInterval()));
	_periodicTimer.setTimerType(Qt::VeryCoarseTimer);
	connect(&_periodicTimer, &QTimer::timeout, this, [=] { beginCheck(true); });
	_safetyTimer.setInterval(int(_env->safetyInterval()));
	connect(&_safetyTimer, &QTimer::timeout, this, [=] { updateSafety(); });
}

TeagramUpdateManager::~TeagramUpdateManager() {
	shutdown();
	if (_worker) {
		_worker->wait();
	}
}

void TeagramUpdateManager::shutdown() {
	_started = false;
	++_generation;
	_periodicTimer.stop();
	_safetyTimer.stop();
	_startCheckAfterWorker = false;
	_startDownloadAfterWorker = false;
	_pendingExplicitRestart = false;
	_automaticInstalledRestartPending = false;
	_automaticRecoveryRestartPending = false;
	if (_job) {
		if (_job->downloader) {
			_job->downloader->cancel();
		}
		if (_job->installer) {
			_job->installer->cancel();
		}
	}
	ShutdownTeagramUpdateWorker(_worker, this, [this] {
		if (_candidate) {
			_candidate->discardArchive();
		}
		_candidate.reset();
		_job.reset();
	});
}

void TeagramUpdateManager::start() {
	if (_started || !TeagramUpdateUiEnabled()) {
		return;
	}
	_started = true;
	_state.runningBuild = _env->runningBuild();
	_state.selectedChannel = _env->readChannel();
	_state.highWaterBuild = _env->readHighWater();
	_state.automatic = _env->automatic();
	_state.previousBundleAvailable = _env->previousBundleAvailable();
	_lastInputAt = _env->now();
	_updatedToastPending = _env->readUpdatedToastPending()
						   && (_state.highWaterBuild == _state.runningBuild);
	if (_updatedToastPending) {
		_state.state = TeagramUpdateUiState::Updated;
	} else if (_state.runningBuild < _state.highWaterBuild) {
		_state.state = TeagramUpdateUiState::Recovered;
	} else {
		_state.state = TeagramUpdateUiState::Idle;
	}
	_stateChanges.fire_copy(_state);
	_safetyTimer.start();
	if (_state.automatic) {
		_periodicTimer.start();
		QTimer::singleShot(0, this, [=] { beginCheck(true); });
	}
	if (_updatedToastPending) {
		QTimer::singleShot(0, this, [=] { showUpdatedToastIfNeeded(); });
	}
}

void TeagramUpdateManager::checkNow() { beginCheck(false); }

void TeagramUpdateManager::setAutomatic(bool enabled) {
	if (!_started || _state.state == TeagramUpdateUiState::Restarting
		|| (_state.automatic == enabled)) {
		return;
	}
	_env->writeAutomatic(enabled);
	_state.automatic = enabled;
	if (enabled) {
		_periodicTimer.start();
		if (_state.state == TeagramUpdateUiState::RecoveryRequired) {
			_automaticRecoveryRestartPending = true;
			maybeRestart();
		} else {
			beginCheck(true);
		}
	} else {
		_periodicTimer.stop();
		_startDownloadAfterWorker = false;
		_automaticInstalledRestartPending = false;
		_automaticRecoveryRestartPending = false;
		if (_job && _job->installer && _automaticJob
			&& _state.state == TeagramUpdateUiState::Preparing) {
			++_generation;
			_job->installer->cancel();
			_pendingExplicitRestart = false;
			_state.state = TeagramUpdateUiState::Ready;
		}
	}
	_stateChanges.fire_copy(_state);
}

void TeagramUpdateManager::setChannel(TeagramUpdateChannel channel) {
	if (!_started || _state.state == TeagramUpdateUiState::Restarting
		|| _state.selectedChannel == channel) {
		return;
	}
	const auto pendingRestart = _pendingExplicitRestart;
	const auto installedBuildRestart
		= pendingRestart
		  && (_pendingRestartState == TeagramUpdateUiState::AlreadyInstalled
			  || _pendingRestartState
					 == TeagramUpdateUiState::RecoveryRequired);
	_state.selectedChannel = channel;
	_state.refusal = TeagramUpdateRefusal::None;
	_state.rejectionCode.clear();
	_env->writeChannel(channel);
	_suppressedAutomaticBuild = 0;
	if (_state.state == TeagramUpdateUiState::RecoveryRequired) {
		_stateChanges.fire_copy(_state);
		return;
	}

	const auto stillEligible
		= _candidate
		  && TeagramUpdateBuildIsEligible(
			  _state.runningBuild, _state.highWaterBuild, channel,
			  _candidate->metadata().build(), _candidate->metadata().channel(),
			  _candidate->selection());
	const auto keepPendingRestart
		= TeagramUpdateKeepPendingRestartAfterChannelChange(
			pendingRestart, bool(_candidate), bool(stillEligible),
			installedBuildRestart);
	_pendingExplicitRestart = keepPendingRestart;
	if (_candidate && !stillEligible) {
		_candidate.reset();
		if (!installedBuildRestart) {
			_state.candidateBuild = 0;
		}
		if (!keepPendingRestart) {
			_state.state = TeagramUpdateUiState::Idle;
		}
		if (_job) {
			++_generation;
			if (_job->downloader) {
				_job->downloader->cancel();
			}
			if (_job->installer) {
				_job->installer->cancel();
			}
			_startCheckAfterWorker = _state.automatic;
		}
	} else if (!_candidate && _job && !keepPendingRestart) {
		++_generation;
		if (_job->downloader) {
			_job->downloader->cancel();
		}
		_startCheckAfterWorker = _state.automatic;
		setState(TeagramUpdateUiState::Idle);
	} else if (!_candidate && !keepPendingRestart) {
		setState(TeagramUpdateUiState::Idle);
	}
	_state.candidateOlder = _candidate && _candidate->olderThanInstalled();
	_stateChanges.fire_copy(_state);
	if (_state.automatic && !keepPendingRestart) {
		_channelSwitchCheck = true;
		beginCheck(true);
	} else if (keepPendingRestart) {
		maybeRestart();
	}
}

void TeagramUpdateManager::performSettingsAction() {
	switch (TeagramUpdateSettingsActionForState(_state.state)) {
	case TeagramUpdateSettingsAction::DownloadAndInstall:
		downloadAndInstall();
		break;
	case TeagramUpdateSettingsAction::Restart:
		restartExplicitly();
		break;
	case TeagramUpdateSettingsAction::DontRestart:
		dontRestart();
		break;
	case TeagramUpdateSettingsAction::Cancel:
		cancel();
		break;
	case TeagramUpdateSettingsAction::CheckNow:
		checkNow();
		break;
	}
}

void TeagramUpdateManager::downloadAndInstall(const QString &dataCopyName) {
	if (!_started || _state.state == TeagramUpdateUiState::Restarting
		|| _state.state != TeagramUpdateUiState::Available) {
		return;
	}
	_dataCopyName = dataCopyName;
	beginDownload();
}

void TeagramUpdateManager::cancel() {
	if (!_started
		|| (_state.state != TeagramUpdateUiState::Downloading
			&& _state.state != TeagramUpdateUiState::Preparing)) {
		return;
	}
	_suppressedAutomaticBuild
		= std::max(_suppressedAutomaticBuild, _state.candidateBuild);
	_pendingExplicitRestart = false;
	++_generation;
	if (_job && _job->downloader) {
		_job->downloader->cancel();
	}
	if (_job && _job->installer) {
		_job->installer->cancel();
	}
	_candidate.reset();
	_state.progressPercent = 0;
	setState(TeagramUpdateUiState::Cancelled);
}

void TeagramUpdateManager::restartExplicitly() {
	if (!_started || _state.state == TeagramUpdateUiState::Restarting) {
		return;
	}
	if (_state.state == TeagramUpdateUiState::RestartPending
		&& _pendingExplicitRestart) {
		maybeRestart();
		return;
	}
	if (_state.state != TeagramUpdateUiState::Ready
		&& _state.state != TeagramUpdateUiState::AlreadyInstalled
		&& _state.state != TeagramUpdateUiState::RecoveryRequired) {
		return;
	}
	_pendingRestartState = _state.state;
	_pendingExplicitRestart = true;
	maybeRestart();
}

void TeagramUpdateManager::dontRestart() {
	if (!_started || _state.state == TeagramUpdateUiState::Restarting
		|| !_pendingExplicitRestart) {
		return;
	}
	_pendingExplicitRestart = false;
	setState(_pendingRestartState);
}

void TeagramUpdateManager::noteUserInput() { _lastInputAt = _env->now(); }

void TeagramUpdateManager::showPreviousBundleInFinder() const {
	_env->showPreviousBundle();
}

void TeagramUpdateManager::showUpdatedToastIfNeeded() {
	if (!_updatedToastPending) {
		return;
	}
	if (!_env->showUpdatedToast(_state.runningBuild)) {
		QTimer::singleShot(int(_env->safetyInterval()), this,
						   [=] { showUpdatedToastIfNeeded(); });
		return;
	}
	_updatedToastPending = false;
}

const TeagramUpdateSnapshot &TeagramUpdateManager::state() const {
	return _state;
}

rpl::producer<TeagramUpdateSnapshot>
TeagramUpdateManager::stateChanges() const {
	return _stateChanges.events_starting_with_copy(_state);
}

void TeagramUpdateManager::beginCheck(bool automatic) {
	const auto channelSwitch = std::exchange(_channelSwitchCheck, false);
	if (!_started || _state.state == TeagramUpdateUiState::Checking
		|| _state.state == TeagramUpdateUiState::RecoveryRequired
		|| _state.state == TeagramUpdateUiState::Restarting
		|| (automatic && !_state.automatic)) {
		return;
	}
	if (_worker) {
		_startCheckAfterWorker = true;
		_automaticJob = automatic;
		_channelSwitchCheck = channelSwitch;
		return;
	}
	const auto location = _env->installLocation();
	if (location != TeagramUpdateInstallLocation::Allowed) {
		_state.refusal = RefusalFor(location);
		_state.rejectionCode.clear();
		setState(TeagramUpdateUiState::Refused);
		return;
	}
	_automaticJob = automatic;
	_state.checkedAt.clear();
	_state.progressPercent = 0;
	_state.refusal = TeagramUpdateRefusal::None;
	_state.rejectionCode.clear();
	setState(TeagramUpdateUiState::Checking);

	const auto generation = ++_generation;
	const auto selected = _state.selectedChannel;
	const auto running = _state.runningBuild;
	const auto selection = TeagramUpdateSelectionForCheck(
		_env->compiledChannel(), selected, automatic && !channelSwitch);
	const auto highWater
		= std::max(_state.highWaterBuild,
				   automatic ? _suppressedAutomaticBuild : quint64(0));
	const auto job = std::make_shared<Job>();
	job->downloader = _env->createDownloader();
	_job = job;
	_downloadStage = TeagramUpdateDownloadStage::Discovering;
	auto thread = QThread::create([=] {
		auto result = job->downloader->checkForUpdates(
			running, selected,
			[this, generation](TeagramUpdateDownloadStage stage, int progress) {
				QMetaObject::invokeMethod(
					this,
					[=] {
						if (generation != _generation
							|| _state.state != TeagramUpdateUiState::Checking) {
							return;
						}
						_downloadStage = stage;
						_state.progressPercent = progress;
						_stateChanges.fire_copy(_state);
					},
					Qt::QueuedConnection);
			},
			highWater, false, selection);
		auto candidate = result.candidate
							 ? std::shared_ptr<TeagramUpdateCandidate>(
								   std::move(result.candidate))
							 : nullptr;
		QMetaObject::invokeMethod(
			this,
			[=, reason = result.reason] {
				if (generation != _generation) {
					return;
				}
				_state.checkedAt = CheckedTime();
				_state.progressPercent = 0;
				if (reason == TeagramUpdateDownloadReason::Available
					&& candidate) {
					if (!_candidate
						|| _candidate->selection() != candidate->selection()
						|| candidate->metadata().build()
							   > _candidate->metadata().build()) {
						_candidate = candidate;
					}
					_state.candidateBuild = _candidate->metadata().build();
					_state.minimumMacOS = _candidate->metadata().minOs();
					if (!_env->supportsMinimumOs(_state.minimumMacOS)) {
						setState(TeagramUpdateUiState::NeedsNewerMacOS);
					} else if (!TeagramUpdateBuildIsEligible(
								   _state.runningBuild, _state.highWaterBuild,
								   _state.selectedChannel,
								   _state.candidateBuild,
								   _candidate->metadata().channel(),
								   _candidate->selection())) {
						_candidate.reset();
						_state.candidateBuild = 0;
						setState(TeagramUpdateUiState::UpToDate);
					} else {
						const auto available
							= TeagramUpdateStateForAvailableCandidate(
								_state.automatic
									&& !_candidate->olderThanInstalled(),
								_candidate->downloaded());
						_startDownloadAfterWorker = available.startDownload;
						setState(available.state);
					}
				} else if (reason == TeagramUpdateDownloadReason::NoUpdate) {
					if (_candidate) {
						_state.candidateBuild = _candidate->metadata().build();
						setState(_candidate->downloaded()
									 ? TeagramUpdateUiState::Ready
									 : TeagramUpdateUiState::Available);
					} else {
						setState(TeagramUpdateStateForNoUpdate(
							_installCommittedInSession,
							_state.runningBuild < _state.highWaterBuild,
							_env->compiledChannel() == TeagramUpdateChannel::Dev
								&& _state.selectedChannel
									   == TeagramUpdateChannel::Main));
					}
				} else if (reason
						   == TeagramUpdateDownloadReason::NetworkFailure) {
					setState(_downloadStage
									 == TeagramUpdateDownloadStage::Downloading
								 ? TeagramUpdateUiState::DownloadFailed
								 : TeagramUpdateUiState::Unreachable);
				} else if (reason == TeagramUpdateDownloadReason::Cancelled) {
					setState(TeagramUpdateUiState::Cancelled);
				} else if (reason == TeagramUpdateDownloadReason::Busy) {
					setState(TeagramUpdateUiState::Busy);
				} else {
					_state.rejectionCode
						= (reason
						   == TeagramUpdateDownloadReason::VerificationFailed)
							  ? u"signature"_q
						  : (reason
							 == TeagramUpdateDownloadReason::LimitExceeded)
							  ? u"package-size"_q
							  : u"release-metadata"_q;
					const auto candidateBuild
						= candidate ? candidate->metadata().build()
									: quint64(0);
					LOG(("Teagram update check rejected: reason=%1 "
						 "candidateBuild=%2 "
						 "replacementCommitted=0 rejectionCode=%3")
							.arg(static_cast<int>(reason))
							.arg(candidateBuild)
							.arg(_state.rejectionCode));
					setState(TeagramUpdateUiState::Rejected);
				}
			},
			Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(
		thread, &QThread::finished, this,
		[=] {
			if (_worker == thread) {
				_worker = nullptr;
			}
			if (!_started) {
				if (_candidate) {
					_candidate->discardArchive();
				}
				_candidate.reset();
				_job.reset();
				thread->deleteLater();
				return;
			}
			_job.reset();
			thread->deleteLater();
			if (_startCheckAfterWorker) {
				_startCheckAfterWorker = false;
				beginCheck(_automaticJob);
			} else if (_startDownloadAfterWorker) {
				_startDownloadAfterWorker = false;
				beginDownload();
			} else {
				updateSafety();
			}
		},
		Qt::QueuedConnection);
	thread->start();
}

void TeagramUpdateManager::beginDownload() {
	if (!_started || !_candidate || _candidate->downloaded()) {
		return;
	}
	if (_worker) {
		_startDownloadAfterWorker = true;
		return;
	}
	const auto location = _env->installLocation();
	if (location != TeagramUpdateInstallLocation::Allowed) {
		_state.refusal = RefusalFor(location);
		setState(TeagramUpdateUiState::Refused);
		return;
	}
	if (!_env->supportsMinimumOs(_candidate->metadata().minOs())) {
		_state.minimumMacOS = _candidate->metadata().minOs();
		setState(TeagramUpdateUiState::NeedsNewerMacOS);
		return;
	}
	if (!TeagramUpdateBuildIsEligible(
			_state.runningBuild, _state.highWaterBuild, _state.selectedChannel,
			_candidate->metadata().build(), _candidate->metadata().channel(),
			_candidate->selection())) {
		_candidate.reset();
		_state.candidateBuild = 0;
		setState(TeagramUpdateUiState::Idle);
		return;
	}
	const auto generation = ++_generation;
	_automaticJob = _state.automatic;
	const auto job = std::make_shared<Job>();
	job->downloader = _env->createDownloader();
	job->candidate = _candidate;
	_job = job;
	_downloadStage = TeagramUpdateDownloadStage::Downloading;
	_state.progressPercent = 0;
	setState(TeagramUpdateUiState::Downloading);
	auto thread = QThread::create([=] {
		auto result = job->downloader->download(
			*job->candidate,
			[this, generation](TeagramUpdateDownloadStage, int progress) {
				QMetaObject::invokeMethod(
					this,
					[=] {
						if (generation != _generation
							|| _state.state
								   != TeagramUpdateUiState::Downloading) {
							return;
						}
						_state.progressPercent = progress;
						_stateChanges.fire_copy(_state);
					},
					Qt::QueuedConnection);
			});
		auto candidate = result.candidate
							 ? std::shared_ptr<TeagramUpdateCandidate>(
								   std::move(result.candidate))
							 : nullptr;
		QMetaObject::invokeMethod(
			this,
			[=, reason = result.reason] {
				if (generation != _generation) {
					return;
				}
				if (reason == TeagramUpdateDownloadReason::Available
					&& candidate) {
					_candidate = candidate;
					_state.candidateBuild = candidate->metadata().build();
					_state.progressPercent = 100;
					setState(TeagramUpdateUiState::Ready);
					updateSafety();
				} else if (reason
						   == TeagramUpdateDownloadReason::NetworkFailure) {
					_candidate.reset();
					setState(TeagramUpdateUiState::DownloadFailed);
				} else if (reason == TeagramUpdateDownloadReason::Cancelled) {
					_candidate.reset();
					setState(TeagramUpdateUiState::Cancelled);
				} else if (reason == TeagramUpdateDownloadReason::Busy) {
					setState(TeagramUpdateUiState::Busy);
				} else {
					const auto rejected = TeagramUpdateRecordRejectedBuild(
						_suppressedAutomaticBuild, _state.candidateBuild);
					_suppressedAutomaticBuild
						= rejected.suppressedAutomaticBuild;
					if (!rejected.candidateRetained) {
						_candidate.reset();
					}
					_state.candidateBuild = rejected.noteBuild;
					_state.rejectionCode
						= (reason == TeagramUpdateDownloadReason::LimitExceeded)
							  ? u"package-size"_q
							  : u"package-hash"_q;
					LOG(("Teagram update download rejected: reason=%1 "
						 "candidateBuild=%2 replacementCommitted=0 "
						 "rejectionCode=%3")
							.arg(static_cast<int>(reason))
							.arg(job->candidate->metadata().build())
							.arg(_state.rejectionCode));
					setState(TeagramUpdateUiState::Rejected);
				}
			},
			Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(
		thread, &QThread::finished, this,
		[=] {
			if (_worker == thread) {
				_worker = nullptr;
			}
			if (!_started) {
				if (_candidate) {
					_candidate->discardArchive();
				}
				_candidate.reset();
				_job.reset();
				thread->deleteLater();
				return;
			}
			_job.reset();
			thread->deleteLater();
			if (_startCheckAfterWorker) {
				_startCheckAfterWorker = false;
				beginCheck(_automaticJob);
			} else if (_startDownloadAfterWorker) {
				_startDownloadAfterWorker = false;
				beginDownload();
			} else {
				updateSafety();
			}
		},
		Qt::QueuedConnection);
	thread->start();
}

void TeagramUpdateManager::beginInstall(bool automatic) {
	if (!_candidate || !_candidate->downloaded()) {
		return;
	}
	if (_worker) {
		_pendingExplicitRestart = !automatic;
		return;
	}
	const auto location = _env->installLocation();
	if (location != TeagramUpdateInstallLocation::Allowed) {
		_state.refusal = RefusalFor(location);
		setState(TeagramUpdateUiState::Refused);
		return;
	}
	const auto decision
		= TeagramUpdateRestartWhenSafe(automatic, activeWork(), idleFor());
	if (decision == TeagramUpdateRestartDecision::WaitForActiveWork) {
		if (automatic) {
			return;
		}
		_pendingRestartState = TeagramUpdateUiState::Ready;
		_pendingExplicitRestart = true;
		_state.blockers = activeWork();
		setState(TeagramUpdateUiState::RestartPending);
		return;
	} else if (decision == TeagramUpdateRestartDecision::WaitForIdleInput) {
		return;
	}
	_pendingExplicitRestart = false;
	_automaticJob = automatic;
	if (_candidate->olderThanInstalled()) {
		if (_dataCopyName.isEmpty()
			|| !_env->deferOlderBuildUntilShutdown(_candidate, _dataCopyName)) {
			_candidate->discardArchive();
			_candidate.reset();
			_state.rejectionCode = u"data-copy"_q;
			setState(TeagramUpdateUiState::Rejected);
			return;
		}
		_state.highWaterBuild = _candidate->metadata().build();
		_env->writeCommittedHighWater(_state.highWaterBuild);
		setState(TeagramUpdateUiState::Restarting);
		_candidate.reset();
		_env->restart();
		return;
	}
	const auto candidate = _candidate;
	const auto generation = ++_generation;
	const auto job = std::make_shared<Job>();
	job->installer = _env->createInstaller();
	job->candidate = candidate;
	_job = job;
	_state.progressPercent = 0;
	setState(TeagramUpdateUiState::Preparing);
	auto thread = QThread::create([=] {
		const auto result = job->installer->install(
			*job->candidate,
			[this, generation, candidate](TeagramUpdateInstallStage stage,
										  int value) {
				if (stage == TeagramUpdateInstallStage::Replacing) {
					QMetaObject::invokeMethod(
						this,
						[=] {
							if (generation != _generation) {
								return;
							}
							_state.progressPercent = value;
							setState(TeagramUpdateUiState::Restarting);
						},
						Qt::BlockingQueuedConnection);
				}
				if (stage == TeagramUpdateInstallStage::Replacing
					&& value == 100) {
					QMetaObject::invokeMethod(
						this,
						[=] {
							persistCommittedBuild(
								candidate->metadata().build(), true,
								candidate->olderThanInstalled());
							candidate->discardArchive();
						},
						Qt::BlockingQueuedConnection);
				}
				QMetaObject::invokeMethod(
					this,
					[=] {
						if (generation != _generation) {
							return;
						}
						_state.progressPercent = value;
						setState(
							(stage == TeagramUpdateInstallStage::Replacing
							 || stage == TeagramUpdateInstallStage::Relaunching)
								? TeagramUpdateUiState::Restarting
								: TeagramUpdateUiState::Preparing);
					},
					Qt::QueuedConnection);
			},
			[this, generation,
			 automatic](const std::function<bool()> &requestRestart) {
				auto allowed = false;
				QMetaObject::invokeMethod(
					this,
					[=, &allowed, &requestRestart] {
						if (generation != _generation) {
							return;
						}
						if (automatic && !_state.automatic) {
							setState(TeagramUpdateUiState::AlreadyInstalled);
							return;
						}
						const auto work = activeWork();
						const auto decision = TeagramUpdateRestartWhenSafe(
							automatic, work, idleFor());
						if (decision
							== TeagramUpdateRestartDecision::RestartNow) {
							allowed = TeagramUpdateRequestRestartIfSafe(
								automatic, activeWork(), idleFor(),
								requestRestart);
						} else {
							_state.blockers = work;
							if (automatic) {
								_automaticInstalledRestartPending = true;
								setState(
									TeagramUpdateUiState::AlreadyInstalled);
							} else {
								_pendingRestartState
									= TeagramUpdateUiState::AlreadyInstalled;
								_pendingExplicitRestart = true;
								setState(TeagramUpdateUiState::RestartPending);
							}
						}
					},
					Qt::BlockingQueuedConnection);
				return allowed;
			});
		QMetaObject::invokeMethod(
			this,
			[=] {
				if (generation != _generation) {
					return;
				}
				persistCommittedBuild(candidate->metadata().build(),
									  result.replacementCommitted,
									  candidate->olderThanInstalled());
				if (result.replacementCommitted) {
					candidate->discardArchive();
					if (_candidate == candidate) {
						_candidate.reset();
					}
					_state.candidateBuild = candidate->metadata().build();
				}
				switch (result.reason) {
				case TeagramUpdateInstallReason::RelaunchRequested:
					setState(TeagramUpdateUiState::Restarting);
					break;
				case TeagramUpdateInstallReason::Cancelled: {
					_suppressedAutomaticBuild
						= std::max(_suppressedAutomaticBuild,
								   candidate->metadata().build());
					_candidate.reset();
					_state.candidateBuild = candidate->metadata().build();
					setState(TeagramUpdateUiState::Cancelled);
				} break;
				case TeagramUpdateInstallReason::Busy:
					setState(TeagramUpdateUiState::Busy);
					break;
				case TeagramUpdateInstallReason::
					RelaunchFailedPreviousAvailable:
					setState(TeagramUpdateUiState::AlreadyInstalled);
					break;
				case TeagramUpdateInstallReason::InstalledBuildChanged:
					setState(TeagramUpdateUiState::AlreadyInstalled);
					break;
				case TeagramUpdateInstallReason::InvalidTarget: {
					const auto current = _env->installLocation();
					_state.refusal = RefusalFor(current);
					setState(TeagramUpdateUiState::Refused);
				} break;
				case TeagramUpdateInstallReason::RefusedTranslocated:
				case TeagramUpdateInstallReason::RefusedUnwritable:
				case TeagramUpdateInstallReason::RefusedOtherOwner:
				case TeagramUpdateInstallReason::RefusedUnsupportedDisk:
					setState(TeagramUpdateUiState::Refused);
					break;
				case TeagramUpdateInstallReason::DataCopyFailed:
				case TeagramUpdateInstallReason::CandidateRejected:
				case TeagramUpdateInstallReason::ArchiveRejected: {
					const auto rejected = TeagramUpdateRecordRejectedBuild(
						_suppressedAutomaticBuild,
						candidate->metadata().build());
					_suppressedAutomaticBuild
						= rejected.suppressedAutomaticBuild;
					if (!rejected.candidateRetained) {
						_candidate.reset();
					}
					_state.candidateBuild = rejected.noteBuild;
					_state.rejectionCode
						= (result.reason
						   == TeagramUpdateInstallReason::ArchiveRejected)
							  ? u"archive"_q
							  : u"signature"_q;
					LOG(("Teagram update install rejected: reason=%1 "
						 "candidateBuild=%2 replacementCommitted=%3 "
						 "rejectionCode=%4")
							.arg(static_cast<int>(result.reason))
							.arg(candidate->metadata().build())
							.arg(result.replacementCommitted ? 1 : 0)
							.arg(_state.rejectionCode));
					setState(TeagramUpdateUiState::Rejected);
				} break;
				case TeagramUpdateInstallReason::ReplacementFailed: {
					const auto rejected = TeagramUpdateRecordRejectedBuild(
						_suppressedAutomaticBuild,
						candidate->metadata().build());
					_suppressedAutomaticBuild
						= rejected.suppressedAutomaticBuild;
					if (!rejected.candidateRetained) {
						_candidate.reset();
					}
					_state.candidateBuild = rejected.noteBuild;
					_state.rejectionCode = u"bundle-identity"_q;
					LOG(("Teagram update replacement failed: reason=%1 "
						 "candidateBuild=%2 replacementCommitted=%3 "
						 "rejectionCode=%4")
							.arg(static_cast<int>(result.reason))
							.arg(candidate->metadata().build())
							.arg(result.replacementCommitted ? 1 : 0)
							.arg(_state.rejectionCode));
					setState(TeagramUpdateUiState::Rejected);
				} break;
				case TeagramUpdateInstallReason::RecoveryRequired:
					candidate->discardArchive();
					if (_candidate == candidate) {
						_candidate.reset();
					}
					_state.candidateBuild = candidate->metadata().build();
					_state.rejectionCode.clear();
					_pendingRestartState
						= TeagramUpdateUiState::RecoveryRequired;
					_pendingExplicitRestart = false;
					_automaticRecoveryRestartPending
						= result.replacementCommitted && _state.automatic;
					LOG(("Teagram update recovery required: reason=%1 "
						 "candidateBuild=%2 replacementCommitted=%3")
							.arg(static_cast<int>(result.reason))
							.arg(candidate->metadata().build())
							.arg(result.replacementCommitted ? 1 : 0));
					setState(TeagramUpdateUiState::RecoveryRequired);
					break;
				}
			},
			Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(
		thread, &QThread::finished, this,
		[=] {
			if (_worker == thread) {
				_worker = nullptr;
			}
			if (!_started) {
				if (_candidate) {
					_candidate->discardArchive();
				}
				_candidate.reset();
				_job.reset();
				thread->deleteLater();
				return;
			}
			_job.reset();
			thread->deleteLater();
			if (_startCheckAfterWorker) {
				_startCheckAfterWorker = false;
				beginCheck(_automaticJob);
			} else if (_startDownloadAfterWorker) {
				_startDownloadAfterWorker = false;
				beginDownload();
			} else if (_pendingExplicitRestart) {
				maybeRestart();
			} else {
				updateSafety();
			}
		},
		Qt::QueuedConnection);
	thread->start();
}

void TeagramUpdateManager::updateSafety() {
	if (!_started) {
		return;
	}
	const auto work = activeWork();
	const auto changed = (_state.blockers.call != work.call)
						 || (_state.blockers.recording != work.recording)
						 || (_state.blockers.upload != work.upload);
	_state.blockers = work;
	if (changed) {
		_stateChanges.fire_copy(_state);
	}
	maybeRestart();
}

void TeagramUpdateManager::maybeRestart() {
	if (_pendingExplicitRestart) {
		if (_worker) {
			return;
		}
		const auto work = activeWork();
		if (work.call || work.recording || work.upload) {
			if (_state.state != TeagramUpdateUiState::RestartPending) {
				_state.blockers = work;
				setState(TeagramUpdateUiState::RestartPending);
			}
			return;
		}
		if (_pendingRestartState == TeagramUpdateUiState::AlreadyInstalled
			|| _pendingRestartState == TeagramUpdateUiState::RecoveryRequired) {
			_pendingExplicitRestart = false;
			_env->restart();
		} else {
			beginInstall(false);
		}
	} else if (_automaticRecoveryRestartPending) {
		if (_worker) {
			return;
		}
		if (TeagramUpdateRestartWhenSafe(true, activeWork(), idleFor())
			== TeagramUpdateRestartDecision::RestartNow) {
			_automaticRecoveryRestartPending = false;
			_env->restart();
		}
	} else if (_automaticInstalledRestartPending) {
		if (_worker) {
			return;
		}
		if (TeagramUpdateRestartWhenSafe(true, activeWork(), idleFor())
			== TeagramUpdateRestartDecision::RestartNow) {
			_automaticInstalledRestartPending = false;
			_env->restart();
		}
	} else if (_state.state == TeagramUpdateUiState::Ready && _state.automatic
			   && !_state.candidateOlder) {
		beginInstall(true);
	}
}

void TeagramUpdateManager::setState(TeagramUpdateUiState state) {
	_state.state = state;
	_state.candidateOlder = _candidate && _candidate->olderThanInstalled();
	_state.automatic = _env->automatic();
	_state.previousBundleAvailable = _env->previousBundleAvailable();
	_stateChanges.fire_copy(_state);
}

TeagramUpdateActiveWork TeagramUpdateManager::activeWork() const {
	return _env->activeWork();
}

std::chrono::milliseconds TeagramUpdateManager::idleFor() const {
	const auto elapsed = std::max<crl::time>(0, _env->now() - _lastInputAt);
	return std::chrono::milliseconds(elapsed);
}

void TeagramUpdateManager::persistCommittedBuild(quint64 build,
												 bool replacementCommitted,
												 bool candidateOlder) {
	if (replacementCommitted) {
		_installCommittedInSession = true;
	}
	const auto highWater = TeagramUpdateCommittedHighWater(
		_state.highWaterBuild, build, replacementCommitted, candidateOlder);
	if (!highWater) {
		return;
	}
	_state.highWaterBuild = *highWater;
	_env->writeCommittedHighWater(_state.highWaterBuild);
}

} // namespace Core

#endif // (Q_OS_MAC && TDESKTOP_TEAGRAM) || TDESKTOP_UNIT_TESTS
