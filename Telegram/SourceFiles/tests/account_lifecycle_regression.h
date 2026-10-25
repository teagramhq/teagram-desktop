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

namespace Window {
class SessionController;
} // namespace Window

class PeerData;

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
namespace MTP {
class Instance;
struct ResponseHandler;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP
#endif

namespace Tests {

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
enum class MainQueueBarrierResult {
	Completed,
	TimedOut,
};

[[nodiscard]] MainQueueBarrierResult
WaitForMainQueueBarrierForRegressionTest(int timeoutMilliseconds = 5000,
										 bool suppressCompletion = false);

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
	std::vector<const Main::Session *> unavailableToasts;
	int permissionRequests = 0;
	int callRpcs = 0;
	int voiceChatLinkJoinAttempts = 0;
	int activations = 0;
	int leavePrompts = 0;
	std::vector<SessionNavigationRegressionEvent> navigationEvents;
	std::vector<const PeerData *> openedPeers;
};

void ResetCallStartRegressionForTest();
void SetCallStartRegressionInterceptForTest(bool enabled);
[[nodiscard]] CallStartRegressionSnapshot
GetCallStartRegressionSnapshotForTest();
[[nodiscard]] bool
InterceptCallStartForRegressionTest(CallStartRegressionKind kind);
[[nodiscard]] bool InterceptCallRpcForRegressionTest();
void RecordCallUnavailableToastForRegressionTest(const Main::Session *session);
void RecordCallPermissionRequestForRegressionTest();
void RecordCallRpcForRegressionTest();
void RecordVoiceChatLinkJoinAttemptForRegressionTest();
void RecordCallActivationForRegressionTest();
void RecordCallLeavePromptForRegressionTest();
void RecordCallLinkChannelOpenedForRegressionTest(const PeerData *peer);
void RecordFeatureUnavailableToastForRegressionTest();

struct SettingsDeepLinkRouteCountsForRegressionTest {
	int myProfileRoute = 0;
	int informationFallback = 0;
	std::array<int, 4> messageRouteDispatches{};
};

struct SettingsDeepLinkRequestCountsForRegressionTest {
	int starsTopupOptions = 0;
	int contactBirthdays = 0;
	int starGifts = 0;
	int savedStarGifts = 0;
};

struct SettingsDeepLinkRequestCaptureForRegressionTest {
	MTP::Instance *instance = nullptr;
	SettingsDeepLinkRequestCountsForRegressionTest counts;
};

enum class SettingsDeepLinkRouteForRegressionTest {
	MyProfile,
	StarsTopUp,
	StarsGift,
	SendGift,
	SendGiftSelf,
};

void RecordSettingsMyProfileRouteForRegressionTest(
	const Window::SessionController *controller, bool informationFallback);
void RecordSettingsDeepLinkRouteForRegressionTest(
	const Window::SessionController *controller,
	SettingsDeepLinkRouteForRegressionTest route);
[[nodiscard]] SettingsDeepLinkRouteCountsForRegressionTest
GetSettingsDeepLinkRouteCountsForRegressionTest(
	const Window::SessionController *controller);
[[nodiscard]] SettingsDeepLinkRequestCaptureForRegressionTest *
SetSettingsDeepLinkRequestCaptureForRegressionTest(
	SettingsDeepLinkRequestCaptureForRegressionTest *capture);
void RecordSettingsDeepLinkRequestForRegressionTest(
	not_null<MTP::Instance *> instance,
	const MTP::details::SerializedRequest &request);

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

enum class AiComposeApplyRegressionEvent {
	HistoryWidgetDraftRead,
	ComposeControlsDraftRead,
	ApplyDispatched,
};

struct AiComposeApplyRegressionCounts {
	int historyWidgetDraftReads = 0;
	int composeControlsDraftReads = 0;
	int applyDispatches = 0;
};

enum class TranslationRpcForRegressionTest {
	ChannelAutoTranslationToggle,
	Text,
	Rich,
};

struct TranslationRpcCountsForRegressionTest {
	int channelAutoTranslationToggle = 0;
	int text = 0;
	int rich = 0;
};

struct BoostRequestSessionCountsForRegressionTest {
	MTP::Instance *instance = nullptr;
	int boostsStatus = 0;
	int myBoosts = 0;
	int applyBoost = 0;
};

struct BoostRequestCaptureForRegressionTest {
	std::vector<BoostRequestSessionCountsForRegressionTest> sessions;
};

void RecordLifecycleWriteForRegressionTest(
	LifecycleWriteForRegressionTest operation);
void ResetLifecycleWriteCountsForRegressionTest();
[[nodiscard]] LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest();
void RecordAiComposeApplyRegressionEvent(AiComposeApplyRegressionEvent event);
void ResetAiComposeApplyRegressionCounts();
[[nodiscard]] AiComposeApplyRegressionCounts
GetAiComposeApplyRegressionCounts();
void RecordTranslationRpcForRegressionTest(
	TranslationRpcForRegressionTest operation);
void ResetTranslationRpcCountsForRegressionTest();
[[nodiscard]] TranslationRpcCountsForRegressionTest
GetTranslationRpcCountsForRegressionTest();
[[nodiscard]] BoostRequestCaptureForRegressionTest *
SetBoostRequestCaptureForRegressionTest(
	BoostRequestCaptureForRegressionTest *capture);
[[nodiscard]] bool CaptureBoostRequestForRegressionTest(
	not_null<MTP::Instance *> instance,
	const MTP::details::SerializedRequest &request);
[[nodiscard]] bool CaptureChannelSaveRequestForRegressionTest(
	not_null<MTP::Instance *> instance, int32 requestId,
	const MTP::details::SerializedRequest &request,
	MTP::ResponseHandler &callbacks);
#endif

void RunAccountLifecycleRegression(Fn<void(int)> done);

} // namespace Tests
