/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/teagram_update_verification.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace Core {

inline constexpr auto kTeagramUpdateEngineActive = false;
inline constexpr auto kTeagramUpdateChannelPreference
	= std::string_view("teagram-update-channel");
inline constexpr auto kTeagramUpdateHighWaterPreference
	= std::string_view("teagram-update-high-water");
inline constexpr auto kTeagramUpdateToastPreference
	= std::string_view("teagram-update-pending-toast");

enum class TeagramUpdateUiState {
	Idle,
	Checking,
	UpToDate,
	WaitingForMain,
	Unreachable,
	Available,
	Downloading,
	Preparing,
	Ready,
	RestartPending,
	Restarting,
	Updated,
	AlreadyInstalled,
	Cancelled,
	DownloadFailed,
	Rejected,
	Busy,
	Refused,
	NeedsNewerMacOS,
	Recovered,
	RecoveryRequired,
};

enum class TeagramUpdateRefusal {
	None,
	Translocated,
	Unwritable,
	OtherOwner,
	UnsupportedDisk,
};

struct TeagramUpdateActiveWork {
	bool call = false;
	bool recording = false;
	bool upload = false;
};

struct TeagramUpdateSnapshot {
	TeagramUpdateUiState state = TeagramUpdateUiState::Idle;
	TeagramUpdateChannel selectedChannel = TeagramUpdateChannel::Dev;
	TeagramUpdateRefusal refusal = TeagramUpdateRefusal::None;
	quint64 runningBuild = 0;
	quint64 candidateBuild = 0;
	quint64 highWaterBuild = 0;
	int progressPercent = 0;
	QString checkedAt;
	QString rejectionCode;
	QString minimumMacOS;
	TeagramUpdateActiveWork blockers;
	bool automatic = true;
	bool previousBundleAvailable = false;
};

enum class TeagramUpdateNoteVariant {
	AutomaticOn,
	AutomaticOff,
	WaitingForMain,
	UnreachableAutomatic,
	UnreachableManual,
	Available,
	Downloading,
	Preparing,
	ReadyAutomatic,
	ReadyManual,
	RestartPending,
	Restarting,
	Cancelled,
	DownloadFailed,
	Rejected,
	Busy,
	Refused,
	NeedsNewerMacOS,
	AlreadyInstalled,
	Recovered,
	RecoveryRequired,
};

enum class TeagramUpdateChatListAction {
	Hidden,
	DownloadAndInstall,
	Cancel,
	Restart,
};

struct TeagramUpdateRejectedBuildState {
	quint64 suppressedAutomaticBuild = 0;
	quint64 noteBuild = 0;
	bool candidateRetained = false;
};

[[nodiscard]] constexpr bool TeagramUpdateControlDisabled(
		TeagramUpdateUiState state,
		bool checkControl) {
	return (state == TeagramUpdateUiState::Restarting)
		|| (checkControl
			&& (state == TeagramUpdateUiState::Checking
				|| state == TeagramUpdateUiState::RecoveryRequired));
}

[[nodiscard]] constexpr bool TeagramUpdateKeepPendingRestartAfterChannelChange(
		bool pendingRestart,
		bool hasCandidate,
		bool candidateStillEligible,
		bool installedBuildRestart) {
	return pendingRestart
		&& (installedBuildRestart
			|| (hasCandidate && candidateStillEligible));
}

[[nodiscard]] constexpr TeagramUpdateChatListAction
TeagramUpdateChatListActionForState(TeagramUpdateUiState state) {
	switch (state) {
	case TeagramUpdateUiState::Available:
		return TeagramUpdateChatListAction::DownloadAndInstall;
	case TeagramUpdateUiState::Downloading:
	case TeagramUpdateUiState::Preparing:
		return TeagramUpdateChatListAction::Cancel;
	case TeagramUpdateUiState::Ready:
	case TeagramUpdateUiState::RestartPending:
	case TeagramUpdateUiState::AlreadyInstalled:
	case TeagramUpdateUiState::RecoveryRequired:
		return TeagramUpdateChatListAction::Restart;
	default:
		return TeagramUpdateChatListAction::Hidden;
	}
}

[[nodiscard]] constexpr TeagramUpdateRejectedBuildState
TeagramUpdateRecordRejectedBuild(
		quint64 suppressedAutomaticBuild,
		quint64 rejectedBuild) {
	return {
		.suppressedAutomaticBuild = (rejectedBuild > suppressedAutomaticBuild)
			? rejectedBuild
			: suppressedAutomaticBuild,
		.noteBuild = rejectedBuild,
	};
}

[[nodiscard]] TeagramUpdateNoteVariant TeagramUpdateNoteForState(
	TeagramUpdateUiState state,
	bool automatic);

[[nodiscard]] constexpr bool TeagramUpdateUiEnabled() {
	return kTeagramUpdateEngineActive;
}

template <typename Settings>
[[nodiscard]] TeagramUpdateChannel ReadTeagramUpdateChannel(
		Settings &settings,
		TeagramUpdateChannel compiledChannel) {
	switch (settings.template readPref<int>(
			kTeagramUpdateChannelPreference,
			-1)) {
	case int(TeagramUpdateChannel::Dev):
		return TeagramUpdateChannel::Dev;
	case int(TeagramUpdateChannel::Main):
		return TeagramUpdateChannel::Main;
	default:
		return compiledChannel;
	}
}

template <typename Settings>
void WriteTeagramUpdateChannel(
		Settings &settings,
		TeagramUpdateChannel channel) {
	settings.template writePref<int>(
		kTeagramUpdateChannelPreference,
		static_cast<int>(channel));
}

template <typename Settings>
[[nodiscard]] quint64 ReadTeagramUpdateHighWater(Settings &settings) {
	const auto encoded = settings.template readPref<QByteArray>(
		kTeagramUpdateHighWaterPreference,
		QByteArray());
	if (encoded.isEmpty()) {
		return 0;
	}
	auto valid = false;
	const auto value = encoded.toULongLong(&valid);
	return valid && QByteArray::number(value) == encoded ? value : 0;
}

template <typename Settings>
void WriteTeagramUpdateHighWater(
	Settings &settings,
	quint64 build) {
	settings.template writePref<QByteArray>(
		kTeagramUpdateHighWaterPreference,
		QByteArray::number(build));
}

[[nodiscard]] constexpr std::optional<quint64>
TeagramUpdateCommittedHighWater(
		quint64 highWaterBuild,
		quint64 candidateBuild,
		bool replacementCommitted) {
	if (!replacementCommitted) {
		return std::nullopt;
	}
	return (candidateBuild > highWaterBuild)
		? candidateBuild
		: highWaterBuild;
}

[[nodiscard]] bool TeagramUpdateBuildIsEligible(
	std::uint64_t runningBuild,
	std::uint64_t highWaterBuild,
	TeagramUpdateChannel selectedChannel,
	std::uint64_t candidateBuild,
	TeagramUpdateChannel candidateChannel);

enum class TeagramUpdateRestartDecision {
	RestartNow,
	WaitForActiveWork,
	WaitForIdleInput,
};

struct TeagramUpdateAvailableCandidateState {
	TeagramUpdateUiState state = TeagramUpdateUiState::Available;
	bool startDownload = false;
};

[[nodiscard]] constexpr TeagramUpdateAvailableCandidateState
TeagramUpdateStateForAvailableCandidate(
		bool automatic,
		bool alreadyDownloaded) {
	return alreadyDownloaded
		? TeagramUpdateAvailableCandidateState{
			.state = TeagramUpdateUiState::Ready,
			.startDownload = false,
		}
		: TeagramUpdateAvailableCandidateState{
			.state = TeagramUpdateUiState::Available,
			.startDownload = automatic,
		};
}

[[nodiscard]] TeagramUpdateRestartDecision TeagramUpdateRestartWhenSafe(
	bool automatic,
	TeagramUpdateActiveWork work,
	std::chrono::milliseconds idleFor);

[[nodiscard]] bool TeagramUpdateRequestRestartIfSafe(
	bool automatic,
	TeagramUpdateActiveWork work,
	std::chrono::milliseconds idleFor,
	const std::function<bool()> &requestRestart);

} // namespace Core
