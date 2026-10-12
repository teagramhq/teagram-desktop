/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_state.h"

namespace Core {

bool TeagramUpdateBuildIsEligible(
		std::uint64_t runningBuild,
		std::uint64_t highWaterBuild,
		TeagramUpdateChannel selectedChannel,
		std::uint64_t candidateBuild,
		TeagramUpdateChannel candidateChannel) {
	return candidateBuild > runningBuild
		&& candidateBuild > highWaterBuild
		&& (selectedChannel == TeagramUpdateChannel::Dev
			|| candidateChannel == TeagramUpdateChannel::Main);
}

TeagramUpdateRestartDecision TeagramUpdateRestartWhenSafe(
		bool automatic,
		TeagramUpdateActiveWork work,
		std::chrono::milliseconds idleFor) {
	if (work.call || work.recording || work.upload) {
		return TeagramUpdateRestartDecision::WaitForActiveWork;
	} else if (automatic && idleFor < std::chrono::minutes(2)) {
		return TeagramUpdateRestartDecision::WaitForIdleInput;
	}
	return TeagramUpdateRestartDecision::RestartNow;
}

} // namespace Core
