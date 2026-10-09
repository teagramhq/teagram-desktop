/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace Main {
class Session;
} // namespace Main

class PeerData;

namespace Tests {

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
class CallsInstanceRegressionAccess;

enum class CallStartRegressionKind {
	Outgoing,
	Group,
	Conference,
	Rtmp,
};

enum class SessionNavigationRegressionEvent {
	ChannelOpened,
	FeatureUnavailableToast,
};

struct CallStartRegressionSnapshot {
	std::array<int, 4> startPassed{};
	std::vector<const Main::Session*> unavailableToasts;
	int permissionRequests = 0;
	int callRpcs = 0;
	int activations = 0;
	int leavePrompts = 0;
	std::vector<SessionNavigationRegressionEvent> navigationEvents;
	std::vector<const PeerData*> openedPeers;
};

void ResetCallStartRegressionForTest();
void SetCallStartRegressionInterceptForTest(bool enabled);
[[nodiscard]] CallStartRegressionSnapshot
GetCallStartRegressionSnapshotForTest();
[[nodiscard]] bool InterceptCallStartForRegressionTest(
	CallStartRegressionKind kind);
void RecordCallUnavailableToastForRegressionTest(
	const Main::Session *session);
void RecordCallPermissionRequestForRegressionTest();
void RecordCallRpcForRegressionTest();
void RecordCallActivationForRegressionTest();
void RecordCallLeavePromptForRegressionTest();
void RecordCallLinkChannelOpenedForRegressionTest(
	const PeerData *peer);
void RecordFeatureUnavailableToastForRegressionTest();

enum class LifecycleWriteForRegressionTest {
	AuthorizationSnapshot,
	AuthorizationFailureMarker,
	CustomServerBlockMarker,
};

struct LifecycleWriteCountsForRegressionTest {
	int authorizationSnapshot = 0;
	int authorizationFailureMarker = 0;
	int customServerBlockMarker = 0;
};

void RecordLifecycleWriteForRegressionTest(
	LifecycleWriteForRegressionTest operation);
void ResetLifecycleWriteCountsForRegressionTest();
[[nodiscard]] LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest();
#endif

void RunAccountLifecycleRegression(Fn<void(int)> done);

} // namespace Tests
