/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_state.h"

namespace Core {

TeagramUpdateNoteVariant TeagramUpdateNoteForState(TeagramUpdateUiState state,
												   bool automatic,
												   bool candidateOlder) {
	switch (state) {
	case TeagramUpdateUiState::Idle:
	case TeagramUpdateUiState::Checking:
	case TeagramUpdateUiState::UpToDate:
	case TeagramUpdateUiState::Updated:
		return automatic ? TeagramUpdateNoteVariant::AutomaticOn
						 : TeagramUpdateNoteVariant::AutomaticOff;
	case TeagramUpdateUiState::WaitingForMain:
		return TeagramUpdateNoteVariant::WaitingForMain;
	case TeagramUpdateUiState::Unreachable:
		return automatic ? TeagramUpdateNoteVariant::UnreachableAutomatic
						 : TeagramUpdateNoteVariant::UnreachableManual;
	case TeagramUpdateUiState::Available:
		return candidateOlder ? TeagramUpdateNoteVariant::AvailableOlder
							  : TeagramUpdateNoteVariant::Available;
	case TeagramUpdateUiState::Downloading:
		return TeagramUpdateNoteVariant::Downloading;
	case TeagramUpdateUiState::Preparing:
		return TeagramUpdateNoteVariant::Preparing;
	case TeagramUpdateUiState::Ready:
		return (automatic && !candidateOlder)
				   ? TeagramUpdateNoteVariant::ReadyAutomatic
				   : TeagramUpdateNoteVariant::ReadyManual;
	case TeagramUpdateUiState::RestartPending:
		return TeagramUpdateNoteVariant::RestartPending;
	case TeagramUpdateUiState::Restarting:
		return TeagramUpdateNoteVariant::Restarting;
	case TeagramUpdateUiState::Cancelled:
		return TeagramUpdateNoteVariant::Cancelled;
	case TeagramUpdateUiState::DownloadFailed:
		return TeagramUpdateNoteVariant::DownloadFailed;
	case TeagramUpdateUiState::Rejected:
		return TeagramUpdateNoteVariant::Rejected;
	case TeagramUpdateUiState::Busy:
		return TeagramUpdateNoteVariant::Busy;
	case TeagramUpdateUiState::Refused:
		return TeagramUpdateNoteVariant::Refused;
	case TeagramUpdateUiState::NeedsNewerMacOS:
		return TeagramUpdateNoteVariant::NeedsNewerMacOS;
	case TeagramUpdateUiState::AlreadyInstalled:
		return TeagramUpdateNoteVariant::AlreadyInstalled;
	case TeagramUpdateUiState::Recovered:
		return TeagramUpdateNoteVariant::Recovered;
	case TeagramUpdateUiState::RecoveryRequired:
		return TeagramUpdateNoteVariant::RecoveryRequired;
	}
	Unexpected("Value in TeagramUpdateNoteForState.");
}

bool TeagramUpdateBuildIsEligible(std::uint64_t runningBuild,
								  std::uint64_t highWaterBuild,
								  TeagramUpdateChannel selectedChannel,
								  std::uint64_t candidateBuild,
								  TeagramUpdateChannel candidateChannel,
								  TeagramUpdateSelection selection) {
	if (selection == TeagramUpdateSelection::SwitchToMain) {
		return candidateBuild != runningBuild
			   && selectedChannel == TeagramUpdateChannel::Main
			   && candidateChannel == TeagramUpdateChannel::Main;
	}
	return candidateBuild > runningBuild && candidateBuild > highWaterBuild
		   && (selectedChannel == TeagramUpdateChannel::Dev
			   || candidateChannel == TeagramUpdateChannel::Main);
}

TeagramUpdateRestartDecision
TeagramUpdateRestartWhenSafe(bool automatic, TeagramUpdateActiveWork work,
							 std::chrono::milliseconds idleFor) {
	if (work.call || work.recording || work.upload) {
		return TeagramUpdateRestartDecision::WaitForActiveWork;
	} else if (automatic && idleFor < std::chrono::minutes(2)) {
		return TeagramUpdateRestartDecision::WaitForIdleInput;
	}
	return TeagramUpdateRestartDecision::RestartNow;
}

bool TeagramUpdateRequestRestartIfSafe(
	bool automatic, TeagramUpdateActiveWork work,
	std::chrono::milliseconds idleFor,
	const std::function<bool()> &requestRestart) {
	return TeagramUpdateRestartWhenSafe(automatic, work, idleFor)
			   == TeagramUpdateRestartDecision::RestartNow
		   && requestRestart && requestRestart();
}

} // namespace Core
