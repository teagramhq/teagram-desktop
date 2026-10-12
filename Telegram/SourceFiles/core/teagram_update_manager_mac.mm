/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_manager.h"

#if defined(Q_OS_MAC) && defined(TDESKTOP_TEAGRAM)

#include "core/application.h"
#include "base/unixtime.h"
#include "calls/calls_instance.h"
#include "core/teagram_update_install.h"
#include "core/teagram_update_worker.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "storage/localstorage.h"
#include "settings.h"
#include "window/window_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QOperatingSystemVersion>
#include <QtCore/QThread>
#include <QtCore/QVersionNumber>

#include <algorithm>
#include <utility>

namespace Core {
namespace {

constexpr auto kUpdateCheckInterval = 24 * 60 * 60 * 1000;
constexpr auto kSafetyCheckInterval = 1000;

[[nodiscard]] TeagramUpdateChannel CompiledChannel() {
	return (kTeagramUpdateChannel == "main")
		? TeagramUpdateChannel::Main
		: TeagramUpdateChannel::Dev;
}

[[nodiscard]] TeagramUpdateRefusal RefusalFor(
		TeagramUpdateInstallLocation location) {
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

[[nodiscard]] bool SupportsMinimumMacOS(const QString &minimum) {
	if (minimum.isEmpty()) {
		return true;
	}
	const auto required = QVersionNumber::fromString(minimum);
	if (required.isNull()) {
		return false;
	}
	const auto currentOs = QOperatingSystemVersion::current();
	const auto current = QVersionNumber(
		currentOs.majorVersion(),
		currentOs.minorVersion(),
		currentOs.microVersion());
	return QVersionNumber::compare(current, required) >= 0;
}

[[nodiscard]] QString CheckedTime() {
	return QDateTime::currentDateTime().toString(u"HH:mm"_q);
}

} // namespace

struct TeagramUpdateManager::Job {
	std::shared_ptr<TeagramUpdateDownloader> downloader;
	std::shared_ptr<TeagramUpdateInstaller> installer;
	std::shared_ptr<TeagramUpdateCandidate> candidate;
};

TeagramUpdateManager &TeagramUpdateManager::Instance() {
	static const auto result = new TeagramUpdateManager();
	return *result;
}

TeagramUpdateManager::TeagramUpdateManager()
: QObject(nullptr) {
	if (const auto application = QCoreApplication::instance()) {
		connect(application, &QCoreApplication::aboutToQuit, this, [=] {
			shutdown();
		});
	}
	_periodicTimer.setInterval(kUpdateCheckInterval);
	_periodicTimer.setTimerType(Qt::VeryCoarseTimer);
	connect(&_periodicTimer, &QTimer::timeout, this, [=] {
		beginCheck(true);
	});
	_safetyTimer.setInterval(kSafetyCheckInterval);
	connect(&_safetyTimer, &QTimer::timeout, this, [=] {
		updateSafety();
	});
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
	const auto settings = &Core::App().settings();
	_state.runningBuild = kTeagramUpdateBuild;
	_state.selectedChannel = ReadTeagramUpdateChannel(
		*settings,
		CompiledChannel());
	_state.highWaterBuild = ReadTeagramUpdateHighWater(*settings);
	_state.automatic = cAutoUpdate();
	_state.previousBundleAvailable = HasPreviousTeagramUpdateBundle();
	_lastInputAt = crl::now();
	_updatedToastPending = settings->readPref<bool>(
		kTeagramUpdateToastPreference,
		false)
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

void TeagramUpdateManager::checkNow() {
	beginCheck(false);
}

void TeagramUpdateManager::setAutomatic(bool enabled) {
	if (!_started
		|| _state.state == TeagramUpdateUiState::Restarting
		|| (_state.automatic == enabled)) {
		return;
	}
	cSetAutoUpdate(enabled);
	Local::writeSettings();
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
	if (!_started
		|| _state.state == TeagramUpdateUiState::Restarting
		|| _state.selectedChannel == channel) {
		return;
	}
	const auto pendingRestart = _pendingExplicitRestart;
	const auto installedBuildRestart = pendingRestart
		&& (_pendingRestartState == TeagramUpdateUiState::AlreadyInstalled
			|| _pendingRestartState == TeagramUpdateUiState::RecoveryRequired);
	_state.selectedChannel = channel;
	_state.refusal = TeagramUpdateRefusal::None;
	_state.rejectionCode.clear();
	WriteTeagramUpdateChannel(Core::App().settings(), channel);
	Local::writeSettings();
	_suppressedAutomaticBuild = 0;
	if (_state.state == TeagramUpdateUiState::RecoveryRequired) {
		_stateChanges.fire_copy(_state);
		return;
	}

	const auto stillEligible = _candidate
		&& TeagramUpdateBuildIsEligible(
			_state.runningBuild,
			_state.highWaterBuild,
			channel,
			_candidate->metadata().build(),
			_candidate->metadata().channel());
	const auto keepPendingRestart
		= TeagramUpdateKeepPendingRestartAfterChannelChange(
			pendingRestart,
			bool(_candidate),
			bool(stillEligible),
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
	_stateChanges.fire_copy(_state);
	if (_state.automatic && !keepPendingRestart) {
		beginCheck(true);
	} else if (keepPendingRestart) {
		maybeRestart();
	}
}

void TeagramUpdateManager::downloadAndInstall() {
	if (!_started
		|| _state.state == TeagramUpdateUiState::Restarting
		|| _state.state != TeagramUpdateUiState::Available) {
		return;
	}
	beginDownload();
}

void TeagramUpdateManager::cancel() {
	if (!_started
		|| (_state.state != TeagramUpdateUiState::Downloading
			&& _state.state != TeagramUpdateUiState::Preparing)) {
		return;
	}
	_suppressedAutomaticBuild = std::max(
		_suppressedAutomaticBuild,
		_state.candidateBuild);
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
	if (!_started
		|| _state.state == TeagramUpdateUiState::Restarting
		|| !_pendingExplicitRestart) {
		return;
	}
	_pendingExplicitRestart = false;
	setState(_pendingRestartState);
}

void TeagramUpdateManager::noteUserInput() {
	_lastInputAt = crl::now();
}

void TeagramUpdateManager::showPreviousBundleInFinder() const {
	ShowPreviousTeagramUpdateBundleInFinder();
}

void TeagramUpdateManager::showUpdatedToastIfNeeded() {
	if (!_updatedToastPending) {
		return;
	}
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		QTimer::singleShot(kSafetyCheckInterval, this, [=] {
			showUpdatedToastIfNeeded();
		});
		return;
	}
	window->showToast(tr::lng_teagram_update_toast(
		tr::now,
		lt_build,
		QString::number(_state.runningBuild)));
	_updatedToastPending = false;
	Core::App().settings().writePref<bool>(
		kTeagramUpdateToastPreference,
		false);
	Local::writeSettings();
}

const TeagramUpdateSnapshot &TeagramUpdateManager::state() const {
	return _state;
}

rpl::producer<TeagramUpdateSnapshot>
TeagramUpdateManager::stateChanges() const {
	return _stateChanges.events_starting_with_copy(_state);
}

void TeagramUpdateManager::beginCheck(bool automatic) {
	if (!_started
		|| _state.state == TeagramUpdateUiState::Checking
		|| _state.state == TeagramUpdateUiState::RecoveryRequired
		|| _state.state == TeagramUpdateUiState::Restarting
		|| (automatic && !_state.automatic)) {
		return;
	}
	if (_worker) {
		_startCheckAfterWorker = true;
		_automaticJob = automatic;
		return;
	}
	const auto location = CheckTeagramUpdateInstallLocation();
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
	const auto highWater = std::max(
		_state.highWaterBuild,
		automatic ? _suppressedAutomaticBuild : quint64(0));
	const auto job = std::make_shared<Job>();
	job->downloader = std::make_shared<TeagramUpdateDownloader>();
	_job = job;
	_downloadStage = TeagramUpdateDownloadStage::Discovering;
	auto thread = QThread::create([=] {
		auto result = job->downloader->checkForUpdates(
			running,
			selected,
			[this, generation](TeagramUpdateDownloadStage stage, int progress) {
				QMetaObject::invokeMethod(this, [=] {
					if (generation != _generation
						|| _state.state != TeagramUpdateUiState::Checking) {
						return;
					}
					_downloadStage = stage;
					_state.progressPercent = progress;
					_stateChanges.fire_copy(_state);
				}, Qt::QueuedConnection);
			},
			highWater,
			false);
		auto candidate = result.candidate
			? std::shared_ptr<TeagramUpdateCandidate>(
				std::move(result.candidate))
			: nullptr;
		QMetaObject::invokeMethod(this, [=, reason = result.reason] {
			if (generation != _generation) {
				return;
			}
			_state.checkedAt = CheckedTime();
			_state.progressPercent = 0;
			if (reason == TeagramUpdateDownloadReason::Available
				&& candidate) {
				if (!_candidate || candidate->metadata().build()
					> _candidate->metadata().build()) {
					_candidate = candidate;
				}
				_state.candidateBuild = _candidate->metadata().build();
				_state.minimumMacOS = _candidate->metadata().minOs();
				if (!SupportsMinimumMacOS(_state.minimumMacOS)) {
					setState(TeagramUpdateUiState::NeedsNewerMacOS);
				} else if (!TeagramUpdateBuildIsEligible(
						_state.runningBuild,
						_state.highWaterBuild,
						_state.selectedChannel,
						_state.candidateBuild,
						_candidate->metadata().channel())) {
					_candidate.reset();
					_state.candidateBuild = 0;
					setState(TeagramUpdateUiState::UpToDate);
				} else {
					const auto available
						= TeagramUpdateStateForAvailableCandidate(
							_state.automatic,
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
				} else if (_state.runningBuild < _state.highWaterBuild) {
					setState(TeagramUpdateUiState::Recovered);
				} else if (CompiledChannel() == TeagramUpdateChannel::Dev
					&& _state.selectedChannel == TeagramUpdateChannel::Main) {
					setState(TeagramUpdateUiState::WaitingForMain);
				} else {
					setState(TeagramUpdateUiState::UpToDate);
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
				setState(TeagramUpdateUiState::Rejected);
			}
		}, Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(thread, &QThread::finished, this, [=] {
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
	}, Qt::QueuedConnection);
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
	const auto location = CheckTeagramUpdateInstallLocation();
	if (location != TeagramUpdateInstallLocation::Allowed) {
		_state.refusal = RefusalFor(location);
		setState(TeagramUpdateUiState::Refused);
		return;
	}
	if (!SupportsMinimumMacOS(_candidate->metadata().minOs())) {
		_state.minimumMacOS = _candidate->metadata().minOs();
		setState(TeagramUpdateUiState::NeedsNewerMacOS);
		return;
	}
	if (!TeagramUpdateBuildIsEligible(
			_state.runningBuild,
			_state.highWaterBuild,
			_state.selectedChannel,
			_candidate->metadata().build(),
			_candidate->metadata().channel())) {
		_candidate.reset();
		_state.candidateBuild = 0;
		setState(TeagramUpdateUiState::Idle);
		return;
	}
	const auto generation = ++_generation;
	_automaticJob = _state.automatic;
	const auto job = std::make_shared<Job>();
	job->downloader = std::make_shared<TeagramUpdateDownloader>();
	job->candidate = _candidate;
	_job = job;
	_downloadStage = TeagramUpdateDownloadStage::Downloading;
	_state.progressPercent = 0;
	setState(TeagramUpdateUiState::Downloading);
	auto thread = QThread::create([=] {
		auto result = job->downloader->download(
			*job->candidate,
			[this, generation](TeagramUpdateDownloadStage, int progress) {
				QMetaObject::invokeMethod(this, [=] {
					if (generation != _generation
						|| _state.state != TeagramUpdateUiState::Downloading) {
						return;
					}
					_state.progressPercent = progress;
					_stateChanges.fire_copy(_state);
				}, Qt::QueuedConnection);
			});
		auto candidate = result.candidate
			? std::shared_ptr<TeagramUpdateCandidate>(
				std::move(result.candidate))
			: nullptr;
		QMetaObject::invokeMethod(this, [=, reason = result.reason] {
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
					_suppressedAutomaticBuild,
					_state.candidateBuild);
				_suppressedAutomaticBuild = rejected.suppressedAutomaticBuild;
				if (!rejected.candidateRetained) {
					_candidate.reset();
				}
				_state.candidateBuild = rejected.noteBuild;
				_state.rejectionCode
					= (reason
						== TeagramUpdateDownloadReason::LimitExceeded)
					? u"package-size"_q
					: u"package-hash"_q;
				setState(TeagramUpdateUiState::Rejected);
			}
		}, Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(thread, &QThread::finished, this, [=] {
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
	}, Qt::QueuedConnection);
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
	const auto location = CheckTeagramUpdateInstallLocation();
	if (location != TeagramUpdateInstallLocation::Allowed) {
		_state.refusal = RefusalFor(location);
		setState(TeagramUpdateUiState::Refused);
		return;
	}
	const auto decision = TeagramUpdateRestartWhenSafe(
		automatic,
		activeWork(),
		idleFor());
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
	const auto candidate = _candidate;
	const auto generation = ++_generation;
	const auto job = std::make_shared<Job>();
	job->installer = std::make_shared<TeagramUpdateInstaller>();
	job->candidate = candidate;
	_job = job;
	_state.progressPercent = 0;
	setState(TeagramUpdateUiState::Preparing);
	auto thread = QThread::create([=] {
		const auto result = job->installer->install(
			*job->candidate,
			[this, generation, candidate](TeagramUpdateInstallStage stage, int value) {
				if (stage == TeagramUpdateInstallStage::Replacing) {
					QMetaObject::invokeMethod(this, [=] {
						if (generation != _generation) {
							return;
						}
						_state.progressPercent = value;
						setState(TeagramUpdateUiState::Restarting);
					}, Qt::BlockingQueuedConnection);
				}
				if (stage == TeagramUpdateInstallStage::Replacing && value == 100) {
					QMetaObject::invokeMethod(
						this,
						[=] {
							persistCommittedBuild(candidate->metadata().build(),
												  true);
							candidate->discardArchive();
						},
						Qt::BlockingQueuedConnection);
				}
				QMetaObject::invokeMethod(this, [=] {
					if (generation != _generation) {
						return;
					}
					_state.progressPercent = value;
					setState((stage == TeagramUpdateInstallStage::Replacing
						|| stage == TeagramUpdateInstallStage::Relaunching)
						? TeagramUpdateUiState::Restarting
						: TeagramUpdateUiState::Preparing);
				}, Qt::QueuedConnection);
			},
		[this, generation, automatic](
				const std::function<bool()> &requestRestart) {
			auto allowed = false;
			QMetaObject::invokeMethod(this, [=, &allowed, &requestRestart] {
				if (generation != _generation) {
					return;
				}
				if (automatic && !_state.automatic) {
					setState(TeagramUpdateUiState::AlreadyInstalled);
					return;
				}
				const auto work = activeWork();
				const auto decision = TeagramUpdateRestartWhenSafe(
					automatic,
					work,
					idleFor());
				if (decision == TeagramUpdateRestartDecision::RestartNow) {
					allowed = TeagramUpdateRequestRestartIfSafe(
						automatic,
						activeWork(),
						idleFor(),
						requestRestart);
				} else {
					_state.blockers = work;
					if (automatic) {
						_automaticInstalledRestartPending = true;
						setState(TeagramUpdateUiState::AlreadyInstalled);
					} else {
						_pendingRestartState
							= TeagramUpdateUiState::AlreadyInstalled;
						_pendingExplicitRestart = true;
						setState(TeagramUpdateUiState::RestartPending);
					}
				}
			}, Qt::BlockingQueuedConnection);
			return allowed;
		});
		QMetaObject::invokeMethod(this, [=] {
			if (generation != _generation) {
				return;
			}
			persistCommittedBuild(candidate->metadata().build(),
								  result.replacementCommitted);
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
			_suppressedAutomaticBuild = std::max(
				_suppressedAutomaticBuild,
				candidate->metadata().build());
			_candidate.reset();
			_state.candidateBuild = candidate->metadata().build();
			setState(TeagramUpdateUiState::Cancelled);
		} break;
		case TeagramUpdateInstallReason::Busy:
			setState(TeagramUpdateUiState::Busy);
			break;
		case TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable:
			setState(TeagramUpdateUiState::AlreadyInstalled);
			break;
		case TeagramUpdateInstallReason::InstalledBuildChanged:
			setState(TeagramUpdateUiState::AlreadyInstalled);
			break;
		case TeagramUpdateInstallReason::InvalidTarget: {
			const auto current = CheckTeagramUpdateInstallLocation();
			_state.refusal = RefusalFor(current);
			setState(TeagramUpdateUiState::Refused);
		} break;
		case TeagramUpdateInstallReason::RefusedTranslocated:
		case TeagramUpdateInstallReason::RefusedUnwritable:
		case TeagramUpdateInstallReason::RefusedOtherOwner:
		case TeagramUpdateInstallReason::RefusedUnsupportedDisk:
			setState(TeagramUpdateUiState::Refused);
			break;
		case TeagramUpdateInstallReason::CandidateRejected:
		case TeagramUpdateInstallReason::ArchiveRejected: {
			const auto rejected = TeagramUpdateRecordRejectedBuild(
				_suppressedAutomaticBuild,
				candidate->metadata().build());
			_suppressedAutomaticBuild = rejected.suppressedAutomaticBuild;
			if (!rejected.candidateRetained) {
				_candidate.reset();
			}
			_state.candidateBuild = rejected.noteBuild;
			_state.rejectionCode
				= (result.reason == TeagramUpdateInstallReason::ArchiveRejected)
				? u"archive"_q
				: u"signature"_q;
			setState(TeagramUpdateUiState::Rejected);
		} break;
		case TeagramUpdateInstallReason::ReplacementFailed: {
			const auto rejected = TeagramUpdateRecordRejectedBuild(
				_suppressedAutomaticBuild,
				candidate->metadata().build());
			_suppressedAutomaticBuild = rejected.suppressedAutomaticBuild;
			if (!rejected.candidateRetained) {
				_candidate.reset();
			}
			_state.candidateBuild = rejected.noteBuild;
			_state.rejectionCode = u"bundle-identity"_q;
			setState(TeagramUpdateUiState::Rejected);
		} break;
		case TeagramUpdateInstallReason::RecoveryRequired:
			candidate->discardArchive();
			if (_candidate == candidate) {
				_candidate.reset();
			}
			_state.candidateBuild = candidate->metadata().build();
			_state.rejectionCode.clear();
			_pendingRestartState = TeagramUpdateUiState::RecoveryRequired;
			_pendingExplicitRestart = false;
			_automaticRecoveryRestartPending
				= result.replacementCommitted && _state.automatic;
			setState(TeagramUpdateUiState::RecoveryRequired);
			break;
		}
		}, Qt::QueuedConnection);
	});
	_worker = thread;
	thread->setParent(this);
	connect(thread, &QThread::finished, this, [=] {
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
	}, Qt::QueuedConnection);
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
			Core::Restart();
		} else {
			beginInstall(false);
		}
	} else if (_automaticRecoveryRestartPending) {
		if (_worker) {
			return;
		}
		if (TeagramUpdateRestartWhenSafe(
				true,
				activeWork(),
				idleFor()) == TeagramUpdateRestartDecision::RestartNow) {
			_automaticRecoveryRestartPending = false;
			Core::Restart();
		}
	} else if (_automaticInstalledRestartPending) {
		if (_worker) {
			return;
		}
		if (TeagramUpdateRestartWhenSafe(
				true,
				activeWork(),
				idleFor()) == TeagramUpdateRestartDecision::RestartNow) {
			_automaticInstalledRestartPending = false;
			Core::Restart();
		}
	} else if (_state.state == TeagramUpdateUiState::Ready
		&& _state.automatic) {
		beginInstall(true);
	}
}

void TeagramUpdateManager::setState(TeagramUpdateUiState state) {
	_state.state = state;
	_state.automatic = cAutoUpdate();
	_state.previousBundleAvailable = HasPreviousTeagramUpdateBundle();
	_stateChanges.fire_copy(_state);
}

TeagramUpdateActiveWork TeagramUpdateManager::activeWork() const {
	auto result = TeagramUpdateActiveWork();
	result.call = Core::App().calls().inCall()
		|| Core::App().calls().inGroupCall();
	if (const auto capture = Media::Capture::instance()) {
		result.recording = capture->started();
	}
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (session && session->uploadsInProgress()) {
			result.upload = true;
			break;
		}
	}
	return result;
}

std::chrono::milliseconds TeagramUpdateManager::idleFor() const {
	const auto elapsed = std::max<crl::time>(0, crl::now() - _lastInputAt);
	return std::chrono::milliseconds(elapsed);
}

void TeagramUpdateManager::persistCommittedBuild(quint64 build,
												 bool replacementCommitted) {
	const auto highWater = TeagramUpdateCommittedHighWater(
		_state.highWaterBuild, build, replacementCommitted);
	if (!highWater) {
		return;
	}
	_state.highWaterBuild = *highWater;
	WriteTeagramUpdateHighWater(Core::App().settings(), _state.highWaterBuild);
	Core::App().settings().writePref<bool>(
		kTeagramUpdateToastPreference,
		true);
	Local::writeSettings();
}

} // namespace Core

#endif // Q_OS_MAC && TDESKTOP_TEAGRAM
