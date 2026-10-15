/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_update_state.h"
#include "core/update_policy.h"

#include <limits>
#include <string_view>
#include <utility>

using namespace Core;

struct TeagramHighWaterSettings {
	QByteArray encoded;

	template <typename Type, typename Other = Type>
	Type readPref(std::string_view, Other &&fallback = Type()) const {
		return encoded.isEmpty() ? std::forward<Other>(fallback)
								 : Type(encoded);
	}
	template <typename Type, typename Other>
	void writePref(std::string_view, Other &&value) {
		encoded = std::forward<Other>(value);
	}
};

TEST_CASE(EveryUpdaterEntryPointIsDenied) {
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Automatic));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Periodic));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Settings));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::CrashWindowRetry));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::CrashWindowGetApp));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Manual));
}

TEST_CASE(ServerCannotChooseAnUpdateOrigin) {
	CHECK(!AcceptServerAutoupdatePrefix(QString()));
	CHECK(!AcceptServerAutoupdatePrefix(u"https://td.telegram.org"_q));
	CHECK(!AcceptServerAutoupdatePrefix(u"https://updates.attacker.test"_q));
}

TEST_CASE(TeagramUpdaterIsEnabledForProduction) {
	CHECK(TeagramUpdateUiEnabled());
}

TEST_CASE(TeagramUpdateEligibilityHonorsHighWaterMark) {
	using Channel = TeagramUpdateChannel;
	CHECK(!TeagramUpdateBuildIsEligible(101, 102, Channel::Dev, 102,
										Channel::Main));
	CHECK(!TeagramUpdateBuildIsEligible(102, 102, Channel::Main, 103,
										Channel::Dev));
	CHECK(TeagramUpdateBuildIsEligible(102, 102, Channel::Main, 103,
									   Channel::Main));
}

TEST_CASE(TeagramUpdateHighWaterPersistsFullUnsignedBuild) {
	auto settings = TeagramHighWaterSettings();
	const auto maximum = std::numeric_limits<quint64>::max();
	WriteTeagramUpdateHighWater(settings, maximum);
	CHECK(ReadTeagramUpdateHighWater(settings) == maximum);

	settings.encoded = QByteArrayLiteral("01");
	CHECK(ReadTeagramUpdateHighWater(settings) == 0);
}

TEST_CASE(TeagramUpdateNoUpdatePreservesCommittedInstallState) {
	using State = TeagramUpdateUiState;
	CHECK(TeagramUpdateStateForNoUpdate(true, true, false)
		  == State::AlreadyInstalled);
	CHECK(TeagramUpdateStateForNoUpdate(false, true, false)
		  == State::Recovered);
	CHECK(TeagramUpdateStateForNoUpdate(false, false, true)
		  == State::WaitingForMain);
	CHECK(TeagramUpdateStateForNoUpdate(false, false, false)
		  == State::UpToDate);
}

TEST_CASE(TeagramUpdateControlsAreDisabledDuringRestarting) {
	using State = TeagramUpdateUiState;
	CHECK(TeagramUpdateControlDisabled(State::Checking, true));
	CHECK(!TeagramUpdateControlDisabled(State::Checking, false));
	CHECK(TeagramUpdateControlDisabled(State::Restarting, false));
	CHECK(!TeagramUpdateControlDisabled(State::Ready, true));
	CHECK(TeagramUpdateControlDisabled(State::RecoveryRequired, true));
}

TEST_CASE(TeagramUpdateChannelSwitchKeepsOnlyValidPendingRestarts) {
	CHECK(TeagramUpdateKeepPendingRestartAfterChannelChange(true, true, true,
															false));
	CHECK(!TeagramUpdateKeepPendingRestartAfterChannelChange(true, true, false,
															 false));
	CHECK(TeagramUpdateKeepPendingRestartAfterChannelChange(true, false, false,
															true));
	CHECK(!TeagramUpdateKeepPendingRestartAfterChannelChange(false, true, true,
															 false));
}

TEST_CASE(TeagramUpdateChatListRestartPendingKeepsRestartAction) {
	CHECK(TeagramUpdateChatListActionForState(
			  TeagramUpdateUiState::RestartPending)
		  == TeagramUpdateChatListAction::Restart);
	CHECK(TeagramUpdateChatListActionForState(TeagramUpdateUiState::Downloading)
		  == TeagramUpdateChatListAction::Cancel);
	CHECK(TeagramUpdateChatListActionForState(
			  TeagramUpdateUiState::RecoveryRequired)
		  == TeagramUpdateChatListAction::Restart);
}

TEST_CASE(TeagramUpdateRecheckPreservesDownloadedCandidateAsReady) {
	const auto checked = TeagramUpdateStateForAvailableCandidate(false, true);
	CHECK(checked.state == TeagramUpdateUiState::Ready);
	CHECK(!checked.startDownload);

	const auto automatic = TeagramUpdateStateForAvailableCandidate(true, false);
	CHECK(automatic.state == TeagramUpdateUiState::Available);
	CHECK(automatic.startDownload);
}

TEST_CASE(TeagramUpdateRestartChecksSafetyAtQuitRequest) {
	auto work = TeagramUpdateActiveWork();
	CHECK(TeagramUpdateRestartWhenSafe(false, work, std::chrono::minutes(5))
		  == TeagramUpdateRestartDecision::RestartNow);

	work.upload = true;
	auto requested = false;
	CHECK(!TeagramUpdateRequestRestartIfSafe(false, work,
											 std::chrono::minutes(5), [&] {
												 requested = true;
												 return true;
											 }));
	CHECK(!requested);

	work.upload = false;
	CHECK(TeagramUpdateRequestRestartIfSafe(false, work,
											std::chrono::minutes(5), [&] {
												requested = true;
												return true;
											}));
	CHECK(requested);
}

TEST_CASE(TeagramUpdateRejectedBuildIsSuppressedAndKeptForTheNote) {
	const auto first = TeagramUpdateRecordRejectedBuild(100, 103);
	CHECK(first.suppressedAutomaticBuild == 103);
	CHECK(first.noteBuild == 103);
	CHECK(!first.candidateRetained);

	const auto older = TeagramUpdateRecordRejectedBuild(104, 103);
	CHECK(older.suppressedAutomaticBuild == 104);
	CHECK(older.noteBuild == 103);
}

TEST_CASE(TeagramUpdateNoteVariantsFollowStateAndAutomaticSetting) {
	using State = TeagramUpdateUiState;
	using Note = TeagramUpdateNoteVariant;
	CHECK(TeagramUpdateNoteForState(State::Idle, true) == Note::AutomaticOn);
	CHECK(TeagramUpdateNoteForState(State::Checking, false)
		  == Note::AutomaticOff);
	CHECK(TeagramUpdateNoteForState(State::Unreachable, true)
		  == Note::UnreachableAutomatic);
	CHECK(TeagramUpdateNoteForState(State::Unreachable, false)
		  == Note::UnreachableManual);
	CHECK(TeagramUpdateNoteForState(State::Ready, true)
		  == Note::ReadyAutomatic);
	CHECK(TeagramUpdateNoteForState(State::Ready, false) == Note::ReadyManual);
	CHECK(TeagramUpdateNoteForState(State::RecoveryRequired, false)
		  == Note::RecoveryRequired);
}

TEST_CASE(TeagramUpdateOlderMainIsEligibleOnlyOnExplicitSwitch) {
	using Channel = TeagramUpdateChannel;
	using Selection = TeagramUpdateSelection;
	CHECK(TeagramUpdateSelectionForCheck(Channel::Dev, Channel::Main, false)
		  == Selection::SwitchToMain);
	CHECK(TeagramUpdateSelectionForCheck(Channel::Dev, Channel::Main, true)
		  == Selection::Newer);
	CHECK(TeagramUpdateSelectionForCheck(Channel::Dev, Channel::Dev, false)
		  == Selection::Newer);
	CHECK(TeagramUpdateSelectionForCheck(Channel::Main, Channel::Main, false)
		  == Selection::Newer);
	CHECK(TeagramUpdateSelectionForCheck(Channel::Main, Channel::Dev, false)
		  == Selection::Newer);

	CHECK(!TeagramUpdateBuildIsEligible(271, 271, Channel::Main, 250,
										Channel::Main));
	CHECK(TeagramUpdateBuildIsEligible(271, 271, Channel::Main, 250,
									   Channel::Main, Selection::SwitchToMain));
	CHECK(!TeagramUpdateBuildIsEligible(
		271, 271, Channel::Main, 271, Channel::Main, Selection::SwitchToMain));
	CHECK(!TeagramUpdateBuildIsEligible(271, 271, Channel::Main, 250,
										Channel::Dev, Selection::SwitchToMain));
	CHECK(!TeagramUpdateBuildIsEligible(
		271, 271, Channel::Dev, 250, Channel::Main, Selection::SwitchToMain));
}

TEST_CASE(TeagramUpdateOlderMainResetsHighWaterAndNeedsConfirmation) {
	CHECK(TeagramUpdateCommittedHighWater(271, 250, true, true)
		  == std::optional<quint64>(250));
	CHECK(TeagramUpdateCommittedHighWater(271, 250, true)
		  == std::optional<quint64>(271));
	CHECK(!TeagramUpdateCommittedHighWater(271, 250, false, true));

	using State = TeagramUpdateUiState;
	using Note = TeagramUpdateNoteVariant;
	CHECK(TeagramUpdateChatListActionForState(State::Available, true)
		  == TeagramUpdateChatListAction::Hidden);
	CHECK(TeagramUpdateChatListActionForState(State::Available)
		  == TeagramUpdateChatListAction::DownloadAndInstall);
	CHECK(TeagramUpdateNoteForState(State::Available, true, true)
		  == Note::AvailableOlder);
	CHECK(TeagramUpdateNoteForState(State::Ready, true, true)
		  == Note::ReadyManual);
}
