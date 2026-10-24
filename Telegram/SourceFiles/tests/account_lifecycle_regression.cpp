/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/account_lifecycle_regression.h"

#include "api/api_compose_with_ai.h"
#include "api/api_chat_filters.h"
#include "api/api_common.h"
#include "api/api_peer_photo.h"
#include "api/api_updates.h"
#include "apiwrap.h"
#include "base/options.h"
#include "base/platform/base_platform_info.h"
#include "base/qthelp_regex.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/about_box.h"
#include "boxes/add_contact_box.h"
#include "boxes/edit_privacy_box.h"
#include "boxes/peers/add_bot_to_chat_box.h"
#include "boxes/peers/edit_admin_rights_save.h"
#include "boxes/peers/edit_participant_box.h"
#include "boxes/peers/edit_peer_common.h"
#include "boxes/peers/edit_peer_info_box.h"
#include "boxes/peers/edit_peer_permissions_box.h"
#include "boxes/peer_list_box.h"
#include "boxes/language_box.h"
#include "boxes/star_gift_box.h"
#include "boxes/translate_box.h"
#include "calls/group/calls_group_common.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "calls/calls_panel.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/core_settings.h"
#include "core/deep_links/deep_links_router.h"
#include "core/local_url_conversion.h"
#include "core/local_url_handlers.h"
#include "core/mac_protected_path_runtime.h"
#include "core/teagram_icon_choice.h"
#include "crl/crl_on_main.h"
#include "crl/crl_semaphore.h"
#include "data/components/scheduled_messages.h"
#include "data/data_ai_compose_tones.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_cloud_themes.h"
#include "data/data_download_manager.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_peer_id.h"
#include "data/data_photo.h"
#include "data/data_saved_messages.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/ui/dialogs_suggestions.h"
#include "export/export_controller.h"
#include "export/export_manager.h"
#include "export/view/export_view_panel_controller.h"
#include "history/admin_log/history_admin_log_inner.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_reply_markup.h"
#include "history/history_widget.h"
#include "history/view/history_view_chat_section.h"
#include "history/view/history_view_top_bar_widget.h"
#include "history/view/history_view_translate_bar.h"
#include "history/view/history_view_translate_tracker.h"
#include "inline_bots/bot_attach_web_view.h"
#include "info/info_controller.h"
#include "info/info_layer_widget.h"
#include "info/info_wrap_widget.h"
#include "info/profile/info_profile_top_bar.h"
#include "info/peer_gifts/info_peer_gifts_common.h"
#include "iv/iv_rich_page.h"
#include "lang/lang_keys.h"
#include "lang/translate_provider.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_account_persistence.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mainwidget.h"
#include "main/session_feature_support.h"
#include "menu/menu_send.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/sender.h"
#include "payments/payments_checkout_process.h"
#include "payments/payments_form.h"
#include "platform/platform_translate_provider.h"
#include "settings/sections/settings_business.h"
#include "settings/sections/settings_credits.h"
#include "passport/passport_form_controller.h"
#include "settings/sections/settings_folders.h"
#include "settings/sections/settings_active_sessions.h"
#include "settings/sections/settings_calls.h"
#include "settings/sections/settings_information.h"
#include "settings/sections/settings_premium.h"
#include "settings/settings_builder.h"
#include "settings/settings_privacy_policy.h"
#include "spellcheck/platform/platform_language.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"
#include "storage/storage_encryption.h"
#include "storage/streamed_file_downloader.h"
#include "test/test_widgets.h"
#include "ui/controls/compose_ai_button_factory.h"
#include "ui/controls/userpic_button.h"
#include "ui/image/image_location.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/special_fields.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_main_menu.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include "styles/style_basic.h"
#include "styles/style_widgets.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QSemaphore>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QAccessible>
#include <QtGui/QAction>
#include <QtGui/QCursor>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace Tests {
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
namespace {

auto gLifecycleWriteCounts = LifecycleWriteCountsForRegressionTest();
auto gAiComposeApplyRegressionCounts = AiComposeApplyRegressionCounts();
auto gCallStartRegressionSnapshot = CallStartRegressionSnapshot();
auto gCallStartRegressionIntercept = false;
auto gTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
BoostRequestCaptureForRegressionTest *gBoostRequestCapture = nullptr;

enum class TranslationRpcRoleForRegressionTest {
	None,
	Stock,
	Custom,
};

auto gTranslationRpcRoleForRegressionTest
	= TranslationRpcRoleForRegressionTest::None;
auto gStockTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
auto gCustomTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
struct SettingsDeepLinkRouteEventForRegressionTest {
	const Window::SessionController *controller = nullptr;
	bool informationFallback = false;
};
auto gSettingsDeepLinkRouteEvents
	= std::vector<SettingsDeepLinkRouteEventForRegressionTest>();

struct CapturedChannelSaveRequest {
	mtpRequestId id = 0;
	mtpBuffer body;
	MTP::ResponseHandler callbacks;
};

struct ChannelSaveRequestCapture {
	not_null<MTP::Instance *> instance;
	std::vector<CapturedChannelSaveRequest> requests;
};

ChannelSaveRequestCapture *gChannelSaveRequestCapture = nullptr;

struct PaymentRequestCapture {
	not_null<MTP::Instance *> instance;
	std::vector<uint32> constructors;
};

PaymentRequestCapture *gPaymentRequestCapture = nullptr;

[[nodiscard]] bool CapturePaymentRequestForRegressionTest(
	not_null<MTP::Instance *> instance,
	const MTP::details::SerializedRequest &request) {
	const auto capture = gPaymentRequestCapture;
	if (!capture || capture->instance != instance) {
		return false;
	}
	auto body = mtpBuffer();
	request.write(body);
	if (body.isEmpty()) {
		return false;
	}
	const auto constructor = static_cast<uint32>(body.front());
	switch (constructor) {
	case mtpc_payments_getPaymentForm:
	case mtpc_payments_getPaymentReceipt:
	case mtpc_payments_validateRequestedInfo:
	case mtpc_payments_sendPaymentForm:
	case mtpc_payments_sendStarsForm:
	case mtpc_payments_getStarsStatus:
		capture->constructors.push_back(constructor);
		return true;
	default:
		return false;
	}
}

template <typename Request>
[[nodiscard]] bool ReplyToChannelSaveRequestForRegressionTest(
	ChannelSaveRequestCapture &capture, size_t index,
	const Request &expectedRequest,
	const typename Request::ResponseType &result) {
	if (capture.requests.size() != index + 1) {
		std::fprintf(stderr,
					 "Channel Save regression failed: request "
					 "%zu was missing or unexpected requests were sent.\n",
					 index);
		return false;
	}
	auto expectedBody = mtpBuffer();
	expectedRequest.write(expectedBody);
	auto &request = capture.requests[index];
	if (request.body != expectedBody || !request.callbacks.done) {
		std::fprintf(stderr,
					 "Channel Save regression failed: request "
					 "%zu had the wrong payload or no success handler.\n",
					 index);
		return false;
	}
	auto response = MTP::Response();
	response.requestId = request.id;
	result.write(response.reply);
	auto done = std::move(request.callbacks.done);
	if (!done(response)) {
		std::fprintf(stderr,
					 "Channel Save regression failed: response "
					 "%zu could not be parsed.\n",
					 index);
		return false;
	}
	return true;
}

template <typename Request>
[[nodiscard]] bool ReplyToCapturedRequestForRegressionTest(
	ChannelSaveRequestCapture &capture, size_t index,
	uint32 expectedConstructor, const typename Request::ResponseType &result) {
	if (capture.requests.size() != index + 1) {
		std::fprintf(stderr,
					 "Peer photo regression failed: request %zu was missing or "
					 "unexpected requests were sent.\n",
					 index);
		return false;
	}
	auto &request = capture.requests[index];
	if (request.body.isEmpty()
		|| static_cast<uint32>(request.body.front()) != expectedConstructor
		|| !request.callbacks.done) {
		std::fprintf(stderr,
					 "Peer photo regression failed: request %zu had the wrong "
					 "constructor or no success handler.\n",
					 index);
		return false;
	}
	auto response = MTP::Response();
	response.requestId = request.id;
	result.write(response.reply);
	auto done = std::move(request.callbacks.done);
	if (!done(response)) {
		std::fprintf(stderr,
					 "Peer photo regression failed: response %zu could not be "
					 "parsed.\n",
					 index);
		return false;
	}
	return true;
}

template <typename Request>
[[nodiscard]] bool FailChannelSaveRequestForRegressionTest(
	ChannelSaveRequestCapture &capture, size_t index,
	const Request &expectedRequest, const QString &errorType) {
	if (capture.requests.size() != index + 1) {
		std::fprintf(
			stderr,
			"Manage controls regression failed: save request %zu was missing "
			"or unexpected requests were sent.\n",
			index);
		return false;
	}
	auto expectedBody = mtpBuffer();
	expectedRequest.write(expectedBody);
	auto &request = capture.requests[index];
	if (request.body != expectedBody || !request.callbacks.fail) {
		std::fprintf(
			stderr,
			"Manage controls regression failed: save request %zu had the wrong "
			"payload or no error handler.\n",
			index);
		return false;
	}
	auto response = MTP::Response();
	response.requestId = request.id;
	const auto error
		= MTP::Error(MTP_rpc_error(MTP_int(400), MTP_string(errorType)));
	const auto fail = std::move(request.callbacks.fail);
	if (!fail(error, response)) {
		std::fprintf(stderr,
					 "Manage controls regression failed: save error %zu was "
					 "not handled.\n",
					 index);
		return false;
	}
	return true;
}

void IncrementTranslationRpcCountForRegressionTest(
	TranslationRpcCountsForRegressionTest &counts,
	TranslationRpcForRegressionTest operation) {
	switch (operation) {
	case TranslationRpcForRegressionTest::ChannelAutoTranslationToggle:
		++counts.channelAutoTranslationToggle;
		break;
	case TranslationRpcForRegressionTest::Text:
		++counts.text;
		break;
	case TranslationRpcForRegressionTest::Rich:
		++counts.rich;
		break;
	}
}

void ReportCallRegressionCheckpoint(const char *name) {
	std::fprintf(stderr, "Call regression checkpoint: %s\n", name);
	std::fflush(stderr);
}

} // namespace

class CallsInstanceRegressionAccess {
  public:
	[[nodiscard]] static Calls::Call *
	InstallStockCall(not_null<Calls::Instance *> instance,
					 not_null<UserData *> user) {
		if (instance->_currentCall || instance->_currentGroupCall
			|| instance->_startingGroupCall) {
			return nullptr;
		}
		auto call = std::make_unique<Calls::Call>(
			TestDelegate(), user, Calls::Call::Type::Outgoing, false);
		call->_state = Calls::Call::State::Established;
		instance->_currentCallPanel
			= std::make_unique<Calls::Panel>(call.get());
		instance->_currentCall = std::move(call);
		instance->_currentCallChanges.fire_copy(instance->_currentCall.get());
		return instance->_currentCall.get();
	}

	static void RemoveStockCall(not_null<Calls::Instance *> instance,
								not_null<Calls::Call *> call) {
		if (instance->_currentCall.get() == call.get()) {
			instance->_currentCallPanel->closeBeforeDestroy();
			instance->_currentCallPanel = nullptr;
			auto taken = base::take(instance->_currentCall);
			instance->_currentCallChanges.fire(nullptr);
			taken.reset();
		}
	}

	[[nodiscard]] static std::unique_ptr<Calls::Call>
	CreateIncomingCallForAnswerRegression(not_null<UserData *> user,
										  bool conferenceInvite) {
		auto call = conferenceInvite
						? std::make_unique<Calls::Call>(
							  TestDelegate(), user, CallId(3053), MsgId(3053),
							  std::vector<not_null<PeerData *>>(), false)
						: std::make_unique<Calls::Call>(
							  TestDelegate(), user, Calls::Call::Type::Incoming,
							  false);
		call->_state = Calls::Call::State::WaitingIncoming;
		call->_id = CallId(3053);
		call->_accessHash = 1;
		call->_gb = bytes::vector(1, gsl::byte{});
		return call;
	}

  private:
	class CallDelegate final : public Calls::Call::Delegate {
	  public:
		Calls::DhConfig getDhConfig() const override { return {}; }

		void callFinished(not_null<Calls::Call *>) override {}

		void callFailed(not_null<Calls::Call *>) override {}

		void callRedial(not_null<Calls::Call *>) override {}

		void callPlaySound(Calls::Call::Delegate::CallSound) override {}

		void callRequestPermissionsOrFail(Fn<void()> onSuccess, bool) override {
			RecordCallPermissionRequestForRegressionTest();
			onSuccess();
		}

		auto callGetVideoCapture(const QString &, bool)
			-> std::shared_ptr<tgcalls::VideoCaptureInterface> override {
			return nullptr;
		}
	};

	[[nodiscard]] static CallDelegate *TestDelegate() {
		static auto result = std::make_unique<CallDelegate>();
		return result.get();
	}
};

void ResetCallStartRegressionForTest() {
	gCallStartRegressionSnapshot = CallStartRegressionSnapshot();
}

void SetCallStartRegressionInterceptForTest(bool enabled) {
	gCallStartRegressionIntercept = enabled;
}

CallStartRegressionSnapshot GetCallStartRegressionSnapshotForTest() {
	return gCallStartRegressionSnapshot;
}

bool InterceptCallStartForRegressionTest(CallStartRegressionKind kind) {
	if (!gCallStartRegressionIntercept) {
		return false;
	}
	++gCallStartRegressionSnapshot.startPassed[static_cast<std::size_t>(kind)];
	return true;
}

void RecordCallUnavailableToastForRegressionTest(const Main::Session *session) {
	gCallStartRegressionSnapshot.unavailableToasts.push_back(session);
}

void RecordCallPermissionRequestForRegressionTest() {
	++gCallStartRegressionSnapshot.permissionRequests;
}

void RecordCallRpcForRegressionTest() {
	++gCallStartRegressionSnapshot.callRpcs;
}

bool InterceptCallRpcForRegressionTest() {
	if (!gCallStartRegressionIntercept) {
		return false;
	}
	RecordCallRpcForRegressionTest();
	return true;
}

void RecordVoiceChatLinkJoinAttemptForRegressionTest() {
	++gCallStartRegressionSnapshot.voiceChatLinkJoinAttempts;
}

void RecordCallActivationForRegressionTest() {
	++gCallStartRegressionSnapshot.activations;
}

void RecordCallLeavePromptForRegressionTest() {
	++gCallStartRegressionSnapshot.leavePrompts;
}

void RecordCallLinkChannelOpenedForRegressionTest(const PeerData *peer) {
	gCallStartRegressionSnapshot.navigationEvents.push_back(
		SessionNavigationRegressionEvent::ChannelOpened);
	gCallStartRegressionSnapshot.openedPeers.push_back(peer);
}

void RecordFeatureUnavailableToastForRegressionTest() {
	gCallStartRegressionSnapshot.navigationEvents.push_back(
		SessionNavigationRegressionEvent::FeatureUnavailableToast);
}

void RecordSettingsMyProfileRouteForRegressionTest(
	const Window::SessionController *controller, bool informationFallback) {
	gSettingsDeepLinkRouteEvents.push_back({controller, informationFallback});
}

SettingsDeepLinkRouteCountsForRegressionTest
GetSettingsDeepLinkRouteCountsForRegressionTest(
	const Window::SessionController *controller) {
	auto result = SettingsDeepLinkRouteCountsForRegressionTest();
	for (const auto &event : gSettingsDeepLinkRouteEvents) {
		if (event.controller == controller) {
			++result.myProfileRoute;
			result.informationFallback += event.informationFallback;
		}
	}
	return result;
}

void RecordLifecycleWriteForRegressionTest(
		LifecycleWriteForRegressionTest operation) {
	switch (operation) {
	case LifecycleWriteForRegressionTest::AuthorizationSnapshot:
		++gLifecycleWriteCounts.authorizationSnapshot;
		break;
	case LifecycleWriteForRegressionTest::AuthorizationFailureMarker:
		++gLifecycleWriteCounts.authorizationFailureMarker;
		break;
	case LifecycleWriteForRegressionTest::CustomServerBlockMarker:
		++gLifecycleWriteCounts.customServerBlockMarker;
		break;
	}
}

void ResetLifecycleWriteCountsForRegressionTest() {
	gLifecycleWriteCounts = LifecycleWriteCountsForRegressionTest();
}

LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest() {
	return gLifecycleWriteCounts;
}

void RecordAiComposeApplyRegressionEvent(AiComposeApplyRegressionEvent event) {
	switch (event) {
	case AiComposeApplyRegressionEvent::HistoryWidgetDraftRead:
		++gAiComposeApplyRegressionCounts.historyWidgetDraftReads;
		break;
	case AiComposeApplyRegressionEvent::ComposeControlsDraftRead:
		++gAiComposeApplyRegressionCounts.composeControlsDraftReads;
		break;
	case AiComposeApplyRegressionEvent::ApplyDispatched:
		++gAiComposeApplyRegressionCounts.applyDispatches;
		break;
	}
}

void ResetAiComposeApplyRegressionCounts() {
	gAiComposeApplyRegressionCounts = AiComposeApplyRegressionCounts();
}

AiComposeApplyRegressionCounts GetAiComposeApplyRegressionCounts() {
	return gAiComposeApplyRegressionCounts;
}

void RecordTranslationRpcForRegressionTest(
	TranslationRpcForRegressionTest operation) {
	IncrementTranslationRpcCountForRegressionTest(gTranslationRpcCounts,
												  operation);
	switch (gTranslationRpcRoleForRegressionTest) {
	case TranslationRpcRoleForRegressionTest::Stock:
		IncrementTranslationRpcCountForRegressionTest(
			gStockTranslationRpcCounts, operation);
		break;
	case TranslationRpcRoleForRegressionTest::Custom:
		IncrementTranslationRpcCountForRegressionTest(
			gCustomTranslationRpcCounts, operation);
		break;
	case TranslationRpcRoleForRegressionTest::None:
		break;
	}
}

void ResetTranslationRpcCountsForRegressionTest() {
	gTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
	switch (gTranslationRpcRoleForRegressionTest) {
	case TranslationRpcRoleForRegressionTest::Stock:
		gStockTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
		break;
	case TranslationRpcRoleForRegressionTest::Custom:
		gCustomTranslationRpcCounts = TranslationRpcCountsForRegressionTest();
		break;
	case TranslationRpcRoleForRegressionTest::None:
		break;
	}
}

TranslationRpcCountsForRegressionTest
GetTranslationRpcCountsForRegressionTest() {
	return gTranslationRpcCounts;
}

BoostRequestCaptureForRegressionTest *SetBoostRequestCaptureForRegressionTest(
	BoostRequestCaptureForRegressionTest *capture) {
	return std::exchange(gBoostRequestCapture, capture);
}

bool CaptureBoostRequestForRegressionTest(
	not_null<MTP::Instance *> instance,
	const MTP::details::SerializedRequest &request) {
	const auto capture = gBoostRequestCapture;
	if (!capture) {
		return false;
	}
	const auto i = std::find_if(
		capture->sessions.begin(), capture->sessions.end(),
		[&](const auto &session) { return session.instance == instance; });
	if (i == capture->sessions.end()) {
		return false;
	}
	auto body = mtpBuffer();
	request.write(body);
	if (body.isEmpty()) {
		return false;
	}
	switch (body.front()) {
	case mtpc_premium_getBoostsStatus:
		++i->boostsStatus;
		return true;
	case mtpc_premium_getMyBoosts:
		++i->myBoosts;
		return true;
	case mtpc_premium_applyBoost:
		++i->applyBoost;
		return true;
	default:
		return false;
	}
}

bool CaptureChannelSaveRequestForRegressionTest(
	not_null<MTP::Instance *> instance, int32 requestId,
	const MTP::details::SerializedRequest &request,
	MTP::ResponseHandler &callbacks) {
	if (CapturePaymentRequestForRegressionTest(instance, request)) {
		return true;
	}
	const auto capture = gChannelSaveRequestCapture;
	if (!capture || capture->instance != instance) {
		return false;
	}
	auto body = mtpBuffer();
	request.write(body);
	if (body.isEmpty()) {
		return false;
	}
	switch (static_cast<uint32>(body.front())) {
	case mtpc_channels_deactivateAllUsernames:
	case mtpc_channels_editTitle:
	case mtpc_channels_setDiscussionGroup:
	case mtpc_channels_toggleAutotranslation:
	case mtpc_channels_updatePaidMessagesPrice:
	case mtpc_messages_editChatAbout:
	case mtpc_account_setGlobalPrivacySettings:
	case mtpc_channels_getParticipant:
	case mtpc_messages_createChat:
	case mtpc_upload_saveFilePart:
	case mtpc_messages_editChatPhoto:
		capture->requests.push_back({
			.id = requestId,
			.body = std::move(body),
			.callbacks = std::move(callbacks),
		});
		return true;
	default:
		return false;
	}
}

MainQueueBarrierResult
WaitForMainQueueBarrierForRegressionTest(int timeoutMilliseconds,
										 bool suppressCompletion) {
	Q_ASSERT(timeoutMilliseconds > 0 && timeoutMilliseconds <= 5000);
	struct State {
		QEventLoop *loop = nullptr;
		bool active = true;
		bool completed = false;
	};
	const auto state = std::make_shared<State>();
	auto loop = QEventLoop();
	state->loop = &loop;
	QTimer::singleShot(timeoutMilliseconds, &loop, &QEventLoop::quit);
	if (!suppressCompletion) {
		crl::on_main([state] {
			if (!state->active || state->completed || !state->loop) {
				return;
			}
			state->completed = true;
			state->loop->quit();
		});
	}
	if (!state->completed) {
		loop.exec();
	}
	state->active = false;
	state->loop = nullptr;
	return state->completed ? MainQueueBarrierResult::Completed
							: MainQueueBarrierResult::TimedOut;
}
#endif

namespace {

const char kRegressionServerKey[] = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g
5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO
62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/
+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9
t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs
5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB
-----END RSA PUBLIC KEY-----)";

const char kRegressionOtherServerKey[] = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAyMEdY1aR+sCR3ZSJrtztKTKqigvO/vBfqACJLZtS7QMgCGXJ6XIR
yy7mx66W0/sOFa7/1mAZtEoIokDP3ShoqF4fVNb6XeqgQfaUHd8wJpDWHcR2OFwv
plUUI1PLTktZ9uW2WE23b+ixNwJjJGwBDJPQEQFBE+vfmH0JP503wr5INS1poWg/
j25sIWeYPHYeOrFp/eXaqhISP6G+q2IeTaWTXpwZj4LzXq5YOpk4bYEQ6mvRq7D1
aHWfYmlEGepfaYR8Q0YqvvhYtMte3ITnuSJs171+GDqpdKcSwHnd6FudwGO4pcCO
j4WcDuXc2CTHgH8gFTNhp/Y8/SpDOhvn9QIDAQAB
-----END RSA PUBLIC KEY-----)";

[[nodiscard]] std::shared_ptr<MTP::details::RSAPublicKey>
RegressionServerKey() {
	return std::make_shared<MTP::details::RSAPublicKey>(bytes::make_span(
		kRegressionServerKey,
		sizeof(kRegressionServerKey) - 1));
}

using OnlineUpdateCounts = Api::Updates::OnlineUpdateCountsForRegressionTest;
using DeferredOnlineUpdatePhase
	= Api::Updates::DeferredOnlineUpdatePhaseForRegressionTest;
using DeferredOnlineUpdateDispatchCounts
	= Api::Updates::DeferredOnlineUpdateDispatchCountsForRegressionTest;
using DeferredOnlineUpdateMutation
	= Window::Controller::DeferredOnlineUpdateMutationForRegressionTest;

constexpr auto kDeferredSwitchDispatchDeadlineMs = 1000;
constexpr auto kDeferredSwitchDispatchQuietIntervalMs = 10;

class ScopedDeferredOnlineUpdatePhase final {
  public:
	explicit ScopedDeferredOnlineUpdatePhase(DeferredOnlineUpdatePhase phase)
		: _previous(
			  Api::Updates::deferredOnlineUpdatePhaseForRegressionTest()) {
		Api::Updates::setDeferredOnlineUpdatePhaseForRegressionTest(phase);
	}

	~ScopedDeferredOnlineUpdatePhase() { restore(); }

	void restore() {
		if (_active) {
			Api::Updates::setDeferredOnlineUpdatePhaseForRegressionTest(
				_previous);
			_active = false;
		}
	}

  private:
	DeferredOnlineUpdatePhase _previous;
	bool _active = true;
};

[[nodiscard]] int
SessionSwitchUpdateCountForTest(const OnlineUpdateCounts &counts) {
	return counts.switchInline + counts.switchDeferred;
}

struct SwitchUpdateExpectation {
	int inlineSwitch = 0;
	int deferredSwitch = 0;
};

enum class SwitchUpdateClassification {
	Matches,
	MissingDeferred,
	ExtraDeferred,
	InlineMismatch,
};

[[nodiscard]] OnlineUpdateCounts
OnlineUpdateDelta(const OnlineUpdateCounts &current,
				  const OnlineUpdateCounts &before) {
	return {
		current.total - before.total,
		current.other - before.other,
		current.switchInline - before.switchInline,
		current.switchDeferred - before.switchDeferred,
	};
}

[[nodiscard]] DeferredOnlineUpdateDispatchCounts
DeferredOnlineUpdateDispatchDelta(
	const DeferredOnlineUpdateDispatchCounts &current,
	const DeferredOnlineUpdateDispatchCounts &before) {
	return {
		current.setupEnqueued - before.setupEnqueued,
		current.setupExecuted - before.setupExecuted,
		current.closeSwitchEnqueued - before.closeSwitchEnqueued,
		current.closeSwitchExecuted - before.closeSwitchExecuted,
	};
}

[[nodiscard]] int DeferredOnlineUpdateDispatchOutstanding(
	const DeferredOnlineUpdateDispatchCounts &counts,
	DeferredOnlineUpdatePhase phase) {
	switch (phase) {
	case DeferredOnlineUpdatePhase::Setup:
		return counts.setupEnqueued - counts.setupExecuted;
	case DeferredOnlineUpdatePhase::CloseSwitch:
		return counts.closeSwitchEnqueued - counts.closeSwitchExecuted;
	}
	return 0;
}

void ReportDeferredOnlineUpdateDispatchCounts(
	const char *observation, const DeferredOnlineUpdateDispatchCounts &stock,
	const DeferredOnlineUpdateDispatchCounts &pinned) {
	const auto print = [](const char *role,
						  const DeferredOnlineUpdateDispatchCounts &counts) {
		std::fprintf(stderr,
					 " %s{setup_enqueued=%d setup_executed=%d setup_pending=%d "
					 "close_enqueued=%d close_executed=%d close_pending=%d}",
					 role, counts.setupEnqueued, counts.setupExecuted,
					 counts.setupEnqueued - counts.setupExecuted,
					 counts.closeSwitchEnqueued, counts.closeSwitchExecuted,
					 counts.closeSwitchEnqueued - counts.closeSwitchExecuted);
	};
	std::fprintf(stderr, "Deferred online callback provenance: observation=%s",
				 observation);
	print("stock", stock);
	print("pinned", pinned);
	std::fprintf(stderr, "\n");
}

void ReportDeferredOnlineUpdateDispatchDeltas(
	const char *observation, const DeferredOnlineUpdateDispatchCounts &stock,
	const DeferredOnlineUpdateDispatchCounts &pinned) {
	const auto print = [](const char *role,
						  const DeferredOnlineUpdateDispatchCounts &counts) {
		std::fprintf(stderr,
					 " %s{setup_enqueued=%d setup_executed=%d "
					 "close_enqueued=%d close_executed=%d}",
					 role, counts.setupEnqueued, counts.setupExecuted,
					 counts.closeSwitchEnqueued, counts.closeSwitchExecuted);
	};
	std::fprintf(stderr, "Deferred online callback delta: observation=%s",
				 observation);
	print("stock", stock);
	print("pinned", pinned);
	std::fprintf(stderr, "\n");
}

[[nodiscard]] bool
WaitForDeferredOnlineUpdateDispatchesToSettle(Main::Session &stock,
											  Main::Session &pinned,
											  DeferredOnlineUpdatePhase phase) {
	const auto settled = [&] {
		return DeferredOnlineUpdateDispatchOutstanding(
				   stock.updates()
					   .deferredOnlineUpdateDispatchCountsForRegressionTest(),
				   phase)
				   == 0
			   && DeferredOnlineUpdateDispatchOutstanding(
					  pinned.updates()
						  .deferredOnlineUpdateDispatchCountsForRegressionTest(),
					  phase)
					  == 0;
	};
	auto loop = QEventLoop();
	auto poll = QTimer();
	auto quiet = QTimer();
	auto deadline = QTimer();
	poll.setInterval(1);
	quiet.setSingleShot(true);
	deadline.setSingleShot(true);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (settled()) {
			if (!quiet.isActive()) {
				quiet.start(kDeferredSwitchDispatchQuietIntervalMs);
			}
		} else {
			quiet.stop();
		}
	});
	QObject::connect(&quiet, &QTimer::timeout, &loop, [&] {
		if (settled()) {
			loop.quit();
		}
	});
	QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
	poll.start();
	deadline.start(kDeferredSwitchDispatchDeadlineMs);
	loop.exec();
	poll.stop();
	quiet.stop();
	deadline.stop();
	const auto result = settled();
	if (!result) {
		std::fprintf(stderr,
					 "Deferred switch dispatch observation deadline expired: "
					 "deadline_ms=%d.\n",
					 kDeferredSwitchDispatchDeadlineMs);
	}
	return result;
}

[[nodiscard]] SwitchUpdateClassification
ClassifySwitchUpdateDelta(const OnlineUpdateCounts &delta,
						  SwitchUpdateExpectation expected) {
	if (delta.switchDeferred < expected.deferredSwitch) {
		return SwitchUpdateClassification::MissingDeferred;
	} else if (delta.switchDeferred > expected.deferredSwitch) {
		return SwitchUpdateClassification::ExtraDeferred;
	} else if (delta.switchInline != expected.inlineSwitch) {
		return SwitchUpdateClassification::InlineMismatch;
	}
	return SwitchUpdateClassification::Matches;
}

[[nodiscard]] const char *
SwitchUpdateClassificationName(SwitchUpdateClassification classification) {
	switch (classification) {
	case SwitchUpdateClassification::Matches:
		return "switch-causes-match";
	case SwitchUpdateClassification::MissingDeferred:
		return "missing-deferred-switch-dispatch";
	case SwitchUpdateClassification::ExtraDeferred:
		return "duplicate-or-misrouted-deferred-switch-dispatch";
	case SwitchUpdateClassification::InlineMismatch:
		return "inline-switch-dispatch-mismatch";
	}
	return "unknown-switch-update-classification";
}

[[nodiscard]] bool
ReportSwitchUpdateDeltas(const char *caseName, const OnlineUpdateCounts &stock,
						 SwitchUpdateExpectation stockExpected,
						 const OnlineUpdateCounts &pinned,
						 SwitchUpdateExpectation pinnedExpected,
						 const OnlineUpdateCounts *additional = nullptr,
						 const char *additionalName = nullptr,
						 SwitchUpdateExpectation additionalExpected = {}) {
	const auto stockClassification
		= ClassifySwitchUpdateDelta(stock, stockExpected);
	const auto pinnedClassification
		= ClassifySwitchUpdateDelta(pinned, pinnedExpected);
	const auto additionalClassification
		= additional
			  ? ClassifySwitchUpdateDelta(*additional, additionalExpected)
			  : SwitchUpdateClassification::Matches;
	const auto causesMatch
		= (stockClassification == SwitchUpdateClassification::Matches)
		  && (pinnedClassification == SwitchUpdateClassification::Matches)
		  && (additionalClassification == SwitchUpdateClassification::Matches);
	auto classification = stockClassification;
	if (classification == SwitchUpdateClassification::Matches) {
		classification = pinnedClassification;
	}
	if (classification == SwitchUpdateClassification::Matches) {
		classification = additionalClassification;
	}
	const auto name = (causesMatch
					   && (stock.other || pinned.other
						   || (additional && additional->other)))
						  ? "unrelated-extra-update"
						  : SwitchUpdateClassificationName(classification);
	const auto print = [](const char *role, const OnlineUpdateCounts &delta) {
		std::fprintf(stderr, " %s{inline=%d deferred=%d total=%d other=%d}",
					 role, delta.switchInline, delta.switchDeferred,
					 delta.total, delta.other);
	};
	std::fprintf(stderr, "Online update lifecycle: case=%s classification=%s",
				 caseName, name);
	print("stock", stock);
	print("pinned", pinned);
	if (additional) {
		print(additionalName ? additionalName : "additional", *additional);
	}
	std::fprintf(stderr, "\n");
	return causesMatch;
}

[[nodiscard]] bool WaitForDeferredSwitchUpdate(Main::Session &session,
											   int expectedCount) {
	auto currentCount = [&] {
		return session.updates()
			.onlineUpdateCountsForRegressionTest()
			.switchDeferred;
	};
	const auto before = currentCount();
	if (before < expectedCount) {
		auto loop = QEventLoop();
		auto poll = QTimer();
		poll.setInterval(1);
		QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
			if (currentCount() != before) {
				loop.quit();
			}
		});
		auto deadlineExpired = false;
		auto deadline = QTimer();
		deadline.setSingleShot(true);
		QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
			deadlineExpired = true;
			loop.quit();
		});
		poll.start();
		deadline.start(kDeferredSwitchDispatchDeadlineMs);
		loop.exec();
		poll.stop();
		if (deadlineExpired && currentCount() < expectedCount) {
			std::fprintf(stderr,
						 "Deferred switch observation deadline expired: "
						 "deadline_ms=%d.\n",
						 kDeferredSwitchDispatchDeadlineMs);
			return false;
		}
	}
	QCoreApplication::processEvents();
	return currentCount() >= expectedCount;
}

[[nodiscard]] bool RunDeferredSwitchMutationRegression(
	Window::Controller &primary, Main::Account &previous, Main::Account &shown,
	DeferredOnlineUpdateMutation mutation, const char *mutationName,
	const char *caseName, SwitchUpdateClassification expectedClassification) {
	const auto previousBefore
		= previous.session().updates().onlineUpdateCountsForRegressionTest();
	const auto shownBefore
		= shown.session().updates().onlineUpdateCountsForRegressionTest();
	primary.setDeferredOnlineUpdateMutationForRegressionTest(mutation);
	primary.showAccount(&shown);
	const auto previousAfterInline = OnlineUpdateDelta(
		previous.session().updates().onlineUpdateCountsForRegressionTest(),
		previousBefore);
	const auto shownAfterInline = OnlineUpdateDelta(
		shown.session().updates().onlineUpdateCountsForRegressionTest(),
		shownBefore);
	const auto inlineMatches = (primary.maybeSession() == &shown.session())
							   && (previousAfterInline.total == 0)
							   && (shownAfterInline.total == 1)
							   && (shownAfterInline.switchInline == 1)
							   && (shownAfterInline.switchDeferred == 0);
	QCoreApplication::processEvents();
	const auto deferredObserved = WaitForDeferredSwitchUpdate(
		previous.session(), previousBefore.switchDeferred + 1);
	const auto previousDelta = OnlineUpdateDelta(
		previous.session().updates().onlineUpdateCountsForRegressionTest(),
		previousBefore);
	const auto shownDelta = OnlineUpdateDelta(
		shown.session().updates().onlineUpdateCountsForRegressionTest(),
		shownBefore);
	const auto assertionAccepted = ReportSwitchUpdateDeltas(
		caseName, previousDelta, {0, 1}, shownDelta, {1, 0});
	const auto classification
		= ClassifySwitchUpdateDelta(previousDelta, {0, 1});
	const auto duplicate = mutation == DeferredOnlineUpdateMutation::Duplicate;
	const auto expectedCount = duplicate ? 2 : 0;
	const auto expectedObserved = duplicate;
	const auto failedAsExpected
		= !assertionAccepted && classification == expectedClassification
		  && previousDelta.switchDeferred == expectedCount
		  && deferredObserved == expectedObserved;
	std::fprintf(stderr,
				 "Deferred switch mutation evidence: mutation=%s assertion=%s "
				 "inline=%s event_loop=%s deferred=%d\n",
				 mutationName,
				 failedAsExpected ? "failed-as-expected" : "unexpected-result",
				 inlineMatches ? "passed" : "failed",
				 deferredObserved ? "dispatch-observed" : "dispatch-absent",
				 previousDelta.switchDeferred);
	return inlineMatches && failedAsExpected;
}

struct ProtectedCacheFixtures {
	QString root;
	QString cacheRoot;
	QString mediaCacheRoot;
	QString cacheLeaf;
	QString mediaCacheLeaf;
	QString openedCache;
	QString openedMediaCache;
	QString cleanupRoot;
};

[[nodiscard]] bool WriteFixtureFile(const QString &path,
									const QByteArray &bytes) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
		   && file.flush();
}

[[nodiscard]] QByteArray SerializeRefusedDownloadHistory(const QString &path,
														 FullMsgId itemId) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << quint64(7) << qint32(Data::DownloadType::Document)
		   << qint64(1) << quint32(1) << quint64(itemId.peer.value)
		   << qint64(itemId.msg.bare) << quint64(0) << path;
	stream.device()->close();
	return result;
}

[[nodiscard]] bool
StoreRefusedDownloadHistory(not_null<Main::Account *> account,
							const QString &path, FullMsgId itemId) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	if (Core::MacProtectedPath::CheckExternalPath(
			Core::MacProtectedPath::Operation::Stat, path,
			"Tests::DownloadHistory::fixture")) {
		return false;
	}
	const auto serialized = SerializeRefusedDownloadHistory(path, itemId);
	const auto called = std::make_shared<bool>(false);
	account->local().updateDownloads([=] {
		*called = true;
		return serialized;
	});
	auto loop = QEventLoop();
	QTimer::singleShot(1500, &loop, &QEventLoop::quit);
	loop.exec();
	return *called && account->local().downloadsSerialized() == serialized;
}

[[nodiscard]] bool
RunRefusedDownloadHistoryRegression(not_null<Main::Session *> session,
									FullMsgId itemId) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	if (session->data().message(itemId)) {
		return false;
	}
	auto published = 0;
	auto lifetime = rpl::lifetime();
	Core::App().downloadManager().loadedAdded()
		| rpl::on_next(
			[&](not_null<const Data::DownloadedId *>) { ++published; },
			lifetime);
	auto loaded = 0;
	for (const auto entry : Core::App().downloadManager().loadedList()) {
		loaded += (entry->itemId == itemId);
	}
	return !published && !loaded;
}

[[nodiscard]] bool
SetProtectedCacheFixturePaths(ProtectedCacheFixtures *fixtures) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto home = qEnvironmentVariable("TDESKTOP_MAC_PROFILE_TEST_HOME");
	if (home.isEmpty()) {
		return false;
	}
	fixtures->root = QDir(home).filePath(
		"Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/"
		"SyntheticStorageFixtures");
	fixtures->cacheRoot = fixtures->root + "/cache-root";
	fixtures->mediaCacheRoot = fixtures->root + "/media-cache-root";
	fixtures->cacheLeaf = fixtures->root + "/cache-leaf";
	fixtures->mediaCacheLeaf = fixtures->root + "/media-cache-leaf";
	fixtures->openedCache = fixtures->root + "/opened-cache";
	fixtures->openedMediaCache = fixtures->root + "/opened-media-cache";
	fixtures->cleanupRoot = fixtures->root + "/legacy-cleanup";
	return true;
}

template <typename Predicate>
[[nodiscard]] bool WaitForRegressionCondition(Predicate condition);

[[nodiscard]] bool RunPeerPhotoRemovalLifecycleRegression(
	not_null<Window::SessionController *> controller) {
	auto &session = controller->session();
	auto capture = ChannelSaveRequestCapture{
		.instance = &session.mtp(),
	};
	const auto restoreCapture = gsl::finally(
		[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
			gChannelSaveRequestCapture = previous;
		});

	const auto denied = session.data().chat(ChatId(1181));
	denied->setName(u"Restricted photo removal group"_q);
	denied->setLoadedStatus(PeerData::LoadedStatus::Normal);
	denied->setDefaultRestrictions(
		ChatRestrictions(ChatRestriction::ChangeInfo));
	const auto deniedPhotoId = PhotoId(118101);
	denied->setPhoto(MTP_chatPhoto(MTP_flags(0),
								   MTP_long(static_cast<int64>(deniedPhotoId)),
								   MTPbytes(), MTP_int(2)));
	const auto unloadedDeniedPhoto = session.data().photo(deniedPhotoId);
	if (!denied->usesCustomServer() || !denied->hasUserpic()
		|| denied->canEditInformation() || !unloadedDeniedPhoto->isNull()) {
		std::fprintf(
			stderr,
			"Peer photo removal regression failed: restricted fixture did not "
			"have an unloaded existing photo.\n");
		return false;
	}
	session.api().peerPhoto().clearChatPhoto(denied);
	QCoreApplication::processEvents();
	if (!capture.requests.empty() || !denied->hasUserpic()
		|| denied->userpicPhotoId() != deniedPhotoId) {
		std::fprintf(
			stderr,
			"Peer photo removal regression failed: denied member sent a "
			"removal request or lost the photo locally.\n");
		return false;
	}

	const auto chat = session.data().chat(ChatId(1182));
	chat->setName(u"Photo removal lifecycle group"_q);
	chat->setFlags(ChatDataFlag::Creator);
	chat->setLoadedStatus(PeerData::LoadedStatus::Normal);
	const auto photoId = PhotoId(118201);
	chat->setPhoto(MTP_chatPhoto(MTP_flags(0),
								 MTP_long(static_cast<int64>(photoId)),
								 MTPbytes(), MTP_int(2)));
	const auto unloadedPhoto = session.data().photo(photoId);
	if (!chat->usesCustomServer() || !chat->hasUserpic()
		|| !chat->canEditInformation() || !unloadedPhoto->isNull()) {
		std::fprintf(
			stderr,
			"Peer photo removal regression failed: creator fixture did not "
			"have an unloaded existing photo.\n");
		return false;
	}
	session.api().peerPhoto().clearChatPhoto(chat);
	QCoreApplication::processEvents();
	auto expectedRequest = mtpBuffer();
	MTPmessages_EditChatPhoto(chat->inputChat(), MTP_inputChatPhotoEmpty())
		.write(expectedRequest);
	if (capture.requests.size() != 1
		|| capture.requests.front().body != expectedRequest) {
		std::fprintf(
			stderr,
			"Peer photo removal regression failed: empty-photo request was "
			"missing or malformed.\n");
		return false;
	}
	const auto updatedChat = MTP_chat(
		MTP_flags(MTPDchat::Flag::f_creator),
		MTP_long(peerToChat(chat->id).bare),
		MTP_string(u"Photo removal lifecycle group"_q), MTP_chatPhotoEmpty(),
		MTP_int(1), MTP_int(1), MTP_int(2), MTPInputChannel(),
		MTPChatAdminRights(), MTPChatBannedRights());
	const auto reply = MTP_updates(
		MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>({updatedChat}), MTP_int(2), MTP_int(0));
	if (!ReplyToCapturedRequestForRegressionTest<MTPmessages_EditChatPhoto>(
			capture, 0, mtpc_messages_editChatPhoto, reply)
		|| chat->hasUserpic() || chat->userpicPhotoId()) {
		std::fprintf(
			stderr,
			"Peer photo removal regression failed: server confirmation did "
			"not clear the photo.\n");
		return false;
	}
	std::fprintf(
		stderr,
		"Peer photo removal regression passed: restricted requests were "
		"denied and the server response removed the existing photo.\n");
	return true;
}

[[nodiscard]] bool RunPeerPhotoUploadLifecycleRegression(
	not_null<Window::SessionController *> controller,
	bool expectServerConfirmation) {
	const auto chat = controller->session().data().chat(ChatId(1171));
	chat->setName(u"Photo upload lifecycle group"_q);
	chat->setFlags(ChatDataFlag::Creator);
	chat->setLoadedStatus(PeerData::LoadedStatus::Normal);
	const auto originalPhotoId = PhotoId(117100);
	const auto originalChat
		= MTP_chat(MTP_flags(MTPDchat::Flag::f_creator),
				   MTP_long(peerToChat(chat->id).bare),
				   MTP_string(u"Photo upload lifecycle group"_q),
				   MTP_chatPhoto(MTP_flags(0),
								 MTP_long(static_cast<int64>(originalPhotoId)),
								 MTPbytes(), MTP_int(2)),
				   MTP_int(1), MTP_int(1), MTP_int(2), MTPInputChannel(),
				   MTPChatAdminRights(), MTPChatBannedRights());
	controller->session().data().processChats(
		MTP_vector<MTPChat>({originalChat}));
	if (expectServerConfirmation) {
		chat->setDefaultRestrictions(
			ChatRestrictions(ChatRestriction::ChangeInfo));
		if (!chat->canEditInformation()) {
			std::fprintf(stderr, "Peer photo lifecycle regression failed: "
								 "creator lost photo edit "
								 "rights under default restrictions.\n");
			return false;
		}
	}
	if (chat->usesCustomServer() != expectServerConfirmation) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: account fixture did not "
			"match the expected upload completion behavior.\n");
		return false;
	}
	if (chat->userpicPhotoId() != originalPhotoId) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: original photo was not "
			"installed before replacement.\n");
		return false;
	}
	const auto peer = not_null<PeerData *>(static_cast<PeerData *>(&*chat));
	ChannelSaveRequestCapture capture{
		.instance = &controller->session().mtp(),
	};
	const auto restoreCapture = gsl::finally(
		[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
			gChannelSaveRequestCapture = previous;
		});
	auto previousImage = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
	previousImage.fill(Qt::red);
	controller->session().api().peerPhoto().upload(peer,
												   {std::move(previousImage)});
	if (!WaitForRegressionCondition([&] { return !capture.requests.empty(); })
		|| static_cast<uint32>(capture.requests.front().body.front())
			   != mtpc_upload_saveFilePart) {
		std::fprintf(stderr,
					 "Peer photo lifecycle regression failed: the first "
					 "pending upload did not start.\n");
		return false;
	}
	auto supersededImage = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
	supersededImage.fill(Qt::yellow);
	controller->session().api().peerPhoto().upload(
		peer, {std::move(supersededImage)});
	if (!WaitForRegressionCondition(
			[&] { return capture.requests.size() == 2; })
		|| static_cast<uint32>(capture.requests.back().body.front())
			   != mtpc_upload_saveFilePart) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: replacing a pending "
			"upload did not start.\n");
		return false;
	}
	const auto box = controller->window().show(
		object_ptr<EditPeerInfoBox>(controller->widget().get(), controller,
									peer),
		Ui::LayerOption::KeepOther, anim::type::normal);
	if (!box) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: edit box did not open.\n");
		return false;
	}
	const auto root = not_null<QWidget *>(box.get());
	const auto buttonRoot = not_null<QWidget *>(box->parentWidget());
	const auto photo = Test::FindFirst<Ui::UserpicButton>(root);
	auto save = static_cast<Ui::RoundButton *>(nullptr);
	for (const auto button : Test::FindAll<Ui::RoundButton>(buttonRoot)) {
		if (button->accessibilityName() == tr::lng_settings_save(tr::now)) {
			save = button;
			break;
		}
	}
	if (!photo || !save) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: photo or Save control was "
			"missing.\n");
		return false;
	}
	const auto photoId = PhotoId(117101);
	struct EditCloseState {
		bool editClosed = false;
		bool photoChangedBeforeClose = false;
	};
	const auto editCloseState = std::make_shared<EditCloseState>();
	QObject::connect(box.get(), &QObject::destroyed, [editCloseState, chat] {
		editCloseState->editClosed = true;
		editCloseState->photoChangedBeforeClose
			= (chat->userpicPhotoId() == photoId);
	});
	auto image = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::blue);
	photo->showCustom(std::move(image));
	const auto replacementUploadIndex = capture.requests.size();
	Test::Click(save);
	QCoreApplication::processEvents();
	if (!WaitForRegressionCondition(
			[&] { return capture.requests.size() > replacementUploadIndex; })
		|| (!expectServerConfirmation
			&& !WaitForRegressionCondition([&] { return !box; }))) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: upload did not start or "
			"the stock editor did not close.\n");
		return false;
	}
	const auto editClosedBeforeConfirmation = !box;
	if (capture.requests.size() != replacementUploadIndex + 1
		|| static_cast<uint32>(
			   capture.requests[replacementUploadIndex].body.front())
			   != mtpc_upload_saveFilePart
		|| chat->userpicPhotoId() != originalPhotoId
		|| (expectServerConfirmation == editClosedBeforeConfirmation)) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: edit box did not follow "
			"the account's upload completion behavior.\n");
		return false;
	}
	if (expectServerConfirmation) {
		auto failureAnimationWait = QElapsedTimer();
		failureAnimationWait.start();
		if (!WaitForRegressionCondition([&] {
				return failureAnimationWait.elapsed() >= st::radialDuration * 5;
			})
			|| !photo->pendingCustomUploadShownForRegressionTest()) {
			std::fprintf(stderr, "Peer photo lifecycle regression failed: "
								 "replacement preview or "
								 "upload progress disappeared while the "
								 "replacement was pending.\n");
			return false;
		}
		Test::Click(save);
		QCoreApplication::processEvents();
		if (!box || capture.requests.size() != replacementUploadIndex + 1
			|| chat->userpicPhotoId() != originalPhotoId) {
			std::fprintf(
				stderr,
				"Peer photo lifecycle regression failed: second Save closed "
				"the edit before the photo upload completed.\n");
			return false;
		}
	}
	if (!ReplyToCapturedRequestForRegressionTest<MTPupload_SaveFilePart>(
			capture, replacementUploadIndex, mtpc_upload_saveFilePart,
			MTP_boolTrue())
		|| !WaitForRegressionCondition([&] {
			   return capture.requests.size() == replacementUploadIndex + 2;
		   })) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: uploaded photo was not "
			"sent to the chat.\n");
		return false;
	}
	const auto editClosedBeforePhotoUpdate = !box;
	if ((expectServerConfirmation == editClosedBeforePhotoUpdate)
		|| chat->userpicPhotoId() != originalPhotoId
		|| static_cast<uint32>(
			   capture.requests[replacementUploadIndex + 1].body.front())
			   != mtpc_messages_editChatPhoto) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: edit box or photo state "
			"changed before the server photo update.\n");
		return false;
	}
	const auto updatedChatFlags
		= MTPDchat::Flags(0) | MTPDchat::Flag::f_creator
		  | (expectServerConfirmation ? MTPDchat::Flag::f_default_banned_rights
									  : MTPDchat::Flags(0));
	const auto updatedChat = MTP_chat(
		MTP_flags(updatedChatFlags), MTP_long(peerToChat(chat->id).bare),
		MTP_string(u"Photo upload lifecycle group"_q),
		MTP_chatPhoto(MTP_flags(0), MTP_long(static_cast<int64>(photoId)),
					  MTPbytes(), MTP_int(2)),
		MTP_int(1), MTP_int(1), MTP_int(2), MTPInputChannel(),
		MTPChatAdminRights(),
		expectServerConfirmation
			? MTPChatBannedRights(MTP_chatBannedRights(
				  MTP_flags(MTPDchatBannedRights::Flag::f_change_info),
				  MTP_int(0)))
			: MTPChatBannedRights());
	const auto reply = MTP_updates(
		MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>({updatedChat}), MTP_int(2), MTP_int(0));
	if (!ReplyToCapturedRequestForRegressionTest<MTPmessages_EditChatPhoto>(
			capture, replacementUploadIndex + 1, mtpc_messages_editChatPhoto,
			reply)) {
		return false;
	}
	if (chat->userpicPhotoId() != photoId) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: server photo update was "
			"not applied.\n");
		return false;
	}
	if (expectServerConfirmation
		&& !(chat->defaultRestrictions() & ChatRestriction::ChangeInfo)) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: synthetic server chat "
			"response did not preserve default_banned_rights.\n");
		return false;
	}
	if (expectServerConfirmation && !chat->canEditInformation()) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: creator lost photo edit "
			"rights after the server update.\n");
		return false;
	}
	if (!WaitForRegressionCondition([&] { return !box; })
		|| !editCloseState->editClosed
		|| (expectServerConfirmation
			&& !editCloseState->photoChangedBeforeClose)) {
		std::fprintf(
			stderr,
			"Peer photo lifecycle regression failed: server photo update was "
			"not applied before the edit closed.\n");
		return false;
	}
	std::fprintf(
		stderr, "Peer photo lifecycle regression passed: edit box followed the "
				"account's upload completion behavior.\n");
	return true;
}

[[nodiscard]] bool RunCreatedGroupPhotoUploadRegression(
	not_null<Window::SessionController *> controller) {
	const auto member = controller->session().data().userLoaded(UserId(2));
	if (!member || !controller->session().user()->usesCustomServer()) {
		std::fprintf(
			stderr,
			"Created group photo regression failed: pinned group fixture was "
			"unavailable.\n");
		return false;
	}
	const auto box = controller->window().show(
		object_ptr<GroupInfoBox>(controller->widget().get(), controller,
								 GroupInfoBox::Type::Group),
		Ui::LayerOption::KeepOther, anim::type::normal);
	if (!box) {
		std::fprintf(stderr, "Created group photo regression failed: create "
							 "box did not open.\n");
		return false;
	}
	const auto root = not_null<QWidget *>(box.get());
	const auto photo = Test::FindFirst<Ui::UserpicButton>(root);
	const auto title = Test::FindFirst<Ui::InputField>(root);
	auto next = static_cast<Ui::RoundButton *>(nullptr);
	for (const auto button :
		 Test::FindAll<Ui::RoundButton>(box->parentWidget())) {
		if (button->accessibilityName() == tr::lng_create_group_next(tr::now)) {
			next = button;
			break;
		}
	}
	if (!photo || !title || !next) {
		std::fprintf(
			stderr,
			"Created group photo regression failed: photo, title or Next "
			"control was missing.\n");
		return false;
	}
	const auto groupName = u"Created photo lifecycle group"_q;
	Test::Settle([&] { title->setText(groupName); });
	auto image = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::green);
	photo->showCustom(std::move(image));
	Test::Click(next);
	const auto selection
		= Test::FindFirst<PeerListBox>(controller->widget().get());
	if (!selection) {
		std::fprintf(stderr, "Created group photo regression failed: member "
							 "selection did not open.\n");
		return false;
	}
	selection->peerListAddSelectedPeers(
		std::vector<not_null<PeerData *>>{member});
	auto create = static_cast<Ui::RoundButton *>(nullptr);
	for (const auto button :
		 Test::FindAll<Ui::RoundButton>(selection->parentWidget())) {
		if (button->accessibilityName()
			== tr::lng_create_group_create(tr::now)) {
			create = button;
			break;
		}
	}
	if (!create) {
		std::fprintf(stderr, "Created group photo regression failed: final "
							 "Create control was missing.\n");
		return false;
	}
	ChannelSaveRequestCapture capture{
		.instance = &controller->session().mtp(),
	};
	const auto restoreCapture = gsl::finally(
		[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
			gChannelSaveRequestCapture = previous;
		});
	Test::Click(create);
	if (!WaitForRegressionCondition(
			[&] { return capture.requests.size() == 1; })
		|| static_cast<uint32>(capture.requests.front().body.front())
			   != mtpc_messages_createChat) {
		std::fprintf(
			stderr,
			"Created group photo regression failed: group creation request "
			"did not start.\n");
		return false;
	}
	const auto chatId = ChatId(1172);
	const auto createdChat
		= MTP_chat(MTP_flags(MTPDchat::Flag::f_creator), MTP_long(chatId.bare),
				   MTP_string(groupName), MTP_chatPhotoEmpty(), MTP_int(1),
				   MTP_int(1), MTP_int(2), MTPInputChannel(),
				   MTPChatAdminRights(), MTPChatBannedRights());
	const auto createdUpdates = MTP_updates(
		MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>({createdChat}), MTP_int(2), MTP_int(0));
	const auto invitedUsers = MTP_messages_invitedUsers(
		createdUpdates, MTP_vector<MTPMissingInvitee>());
	if (!ReplyToCapturedRequestForRegressionTest<MTPmessages_CreateChat>(
			capture, 0, mtpc_messages_createChat, invitedUsers)
		|| box || !controller->session().data().chat(chatId)
		|| capture.requests.size() != 2
		|| static_cast<uint32>(capture.requests[1].body.front())
			   != mtpc_upload_saveFilePart) {
		std::fprintf(
			stderr,
			"Created group photo regression failed: selected photo was not "
			"uploaded after group creation.\n");
		return false;
	}
	if (!ReplyToCapturedRequestForRegressionTest<MTPupload_SaveFilePart>(
			capture, 1, mtpc_upload_saveFilePart, MTP_boolTrue())
		|| !WaitForRegressionCondition(
			[&] { return capture.requests.size() == 3; })
		|| static_cast<uint32>(capture.requests[2].body.front())
			   != mtpc_messages_editChatPhoto) {
		std::fprintf(stderr, "Created group photo regression failed: photo "
							 "update did not target "
							 "the created group.\n");
		return false;
	}
	const auto photoId = PhotoId(117201);
	const auto updatedChat = MTP_chat(
		MTP_flags(MTPDchat::Flag::f_creator), MTP_long(chatId.bare),
		MTP_string(groupName),
		MTP_chatPhoto(MTP_flags(0), MTP_long(static_cast<int64>(photoId)),
					  MTPbytes(), MTP_int(2)),
		MTP_int(1), MTP_int(1), MTP_int(2), MTPInputChannel(),
		MTPChatAdminRights(), MTPChatBannedRights());
	const auto photoUpdates = MTP_updates(
		MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>({updatedChat}), MTP_int(3), MTP_int(0));
	const auto created = controller->session().data().chat(chatId);
	if (!ReplyToCapturedRequestForRegressionTest<MTPmessages_EditChatPhoto>(
			capture, 2, mtpc_messages_editChatPhoto, photoUpdates)
		|| created->userpicPhotoId() != photoId) {
		std::fprintf(
			stderr,
			"Created group photo regression failed: uploaded photo was not "
			"applied to the created group.\n");
		return false;
	}
	std::fprintf(
		stderr,
		"Created group photo regression passed: selected image reached the "
		"created group.\n");
	return true;
}

[[nodiscard]] bool
RunLegacyCleanupSymlinkRegression(const ProtectedCacheFixtures &fixtures) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto profile = Core::MacProtectedPath::ProfileRoot();
	const auto base = profile + "tdata/cleanup_regression";
	const auto original = base + ".original";
	const auto probeName = u"unrecognized-legacy-file"_q;
	const auto originalFile = original + '/' + probeName;
	const auto initialAlias = profile + "tdata/cleanup_initial";
	if (!QDir().mkpath(QFileInfo(initialAlias).absolutePath())
		|| !QFile::link(fixtures.cleanupRoot, initialAlias)) {
		return false;
	}
	struct InitialState {
		bool completed = false;
		bool collectorCalled = false;
	};
	const auto initialState = std::make_shared<InitialState>();
	const auto initialLoop = std::make_shared<QEventLoop>();
	const auto initialTimer = std::make_shared<QTimer>();
	initialTimer->setSingleShot(true);
	QObject::connect(initialTimer.get(), &QTimer::timeout, initialLoop.get(),
					 &QEventLoop::quit);
	Storage::ClearLegacyFilesGuarded(
		initialAlias + '/',
		[initialState](FnMut<void(::base::flat_set<QString> &&)> then) {
			initialState->collectorCalled = true;
			then({});
		},
		[=] {
			initialState->completed = true;
			initialLoop->quit();
		});
	if (!initialState->completed) {
		initialTimer->start(10000);
		initialLoop->exec();
	}
	initialTimer->stop();
	const auto initialRetained
		= initialState->completed && !initialState->collectorCalled;
	QFile::remove(initialAlias);
	if (!initialRetained) {
		return false;
	}

	if (!QDir().mkpath(base)
		|| !WriteFixtureFile(base + '/' + probeName, "legacy bytes")) {
		return false;
	}
	struct State {
		bool swapped = false;
		bool completed = false;
	};
	const auto state = std::make_shared<State>();
	const auto loop = std::make_shared<QEventLoop>();
	const auto timer = std::make_shared<QTimer>();
	timer->setSingleShot(true);
	QObject::connect(timer.get(), &QTimer::timeout, loop.get(),
					 &QEventLoop::quit);
	Storage::ClearLegacyFilesGuarded(
		base + '/',
		[=](FnMut<void(::base::flat_set<QString> &&)> then) mutable {
			state->swapped = QDir().rename(base, original)
							 && QFile::link(fixtures.cleanupRoot, base);
			then({});
		},
		[=] {
			state->completed = true;
			loop->quit();
		});
	if (!state->completed) {
		timer->start(10000);
		loop->exec();
	}
	timer->stop();
	auto originalContents = QFile(originalFile);
	const auto originalRetained = QFileInfo::exists(originalFile);
	return state->swapped && state->completed && originalRetained
		   && originalContents.open(QIODevice::ReadOnly)
		   && originalContents.readAll() == "legacy bytes"
		   && QFileInfo(base).isSymLink();
}

[[nodiscard]] std::shared_ptr<MTP::details::RSAPublicKey>
RegressionOtherServerKey() {
	return std::make_shared<MTP::details::RSAPublicKey>(bytes::make_span(
		kRegressionOtherServerKey, sizeof(kRegressionOtherServerKey) - 1));
}

[[nodiscard]] bool
ConfigurePinnedServer(not_null<Main::Account *> account,
					  std::shared_ptr<MTP::details::RSAPublicKey> key) {
	const auto configured
		= account->mtp().dcOptions().setCustomServer(MTP::CustomServer{
			.dcId = 2,
			.ip = "127.0.0.1",
			.port = 8443,
			.key = std::move(key),
		});
	return configured && account->local().writeMtpConfig(true);
}

[[nodiscard]] MTP::AuthKeyPtr RegressionAuthKey(int byte) {
	auto data = MTP::AuthKey::Data();
	std::fill(
		data.begin(),
		data.end(),
		static_cast<gsl::byte>(byte));
	return std::make_shared<MTP::AuthKey>(
		MTP::AuthKey::Type::Generated,
		2,
		data);
}

[[nodiscard]] bool IncompleteStoredSessionStartsPaused(Main::Domain &domain) {
	auto account = std::make_unique<Main::Account>(
		&domain, u"main-2327-incomplete-session-regression"_q, 0);
	auto config = account->prepareToStart(RegressionAuthKey(0x45));
	if (!config) {
		config = std::make_unique<MTP::Config>(MTP::Environment::Production);
	}
	account->setSessionUserId(UserId(4242));
	account->setSessionFromStorage(std::make_unique<Main::SessionSettings>(),
								   QByteArray(), 0);
	account->setLegacyMtpKey(RegressionAuthKey(0x44));

	if (!config->dcOptions().setCustomServer(MTP::CustomServer{
			.dcId = 2,
			.ip = "127.0.0.1",
			.port = 8443,
			.key = RegressionServerKey(),
		})) {
		return false;
	}
	account->start(std::move(config));
	if (account->sessionExists()
		|| account->willHaveSessionUniqueId(nullptr) != 0
		|| !account->mtp().getKeysForWrite().empty()
		|| !account->mtp().dcOptions().hasCustomServer()
		|| account->mtp().dcOptions().blocked()
		|| account->mtp().dcOptions().isAuthorized(2)
		|| account->mtp().isServerEnrollmentNetworkAllowed()) {
		return false;
	}

	auto pausedFailures = 0;
	auto sender = MTP::Sender(&account->mtp());
	const auto requestId
		= sender.request(MTPupdates_GetState())
			  .fail([&](const MTP::Error &error) {
				  if (MTP::IsServerEnrollmentPausedError(error)) {
					  ++pausedFailures;
				  }
			  })
			  .send();
	if (!sender.pending(requestId)) {
		return false;
	}
	QCoreApplication::processEvents();
	return !sender.pending(requestId) && pausedFailures == 1
		   && !account->mtp().isServerEnrollmentNetworkAllowed();
}

[[nodiscard]] bool HasAuthKey(
		not_null<Main::Account*> account,
		MTP::AuthKey::KeyId keyId) {
	const auto keys = account->mtp().getKeysForWrite();
	return std::any_of(keys.begin(), keys.end(), [=](const auto &key) {
		return key->keyId() == keyId;
	});
}

[[nodiscard]] bool HasNoAuthorizationState(
		not_null<Main::Account*> account) {
	return account->mtp().getKeysForWrite().empty()
		&& !account->sessionExists()
		&& (account->willHaveSessionUniqueId(nullptr) == 0);
}

constexpr auto kSyntheticStockCacheBindingForRegression
	= Storage::ServerCacheBinding{
		.fingerprintKnown = true,
		.fingerprint = 0,
		.userIdKnown = true,
		.userId = 1,
};

[[nodiscard]] bool
PrepareSyntheticStockAccountForRegression(not_null<Main::Account *> account) {
	account->mtp().stopForServerEnrollment();
	account->mtp().dcOptions().constructUnenrolled();
	return account->local().writeMtpAuthorization()
		   && !account->mtp().dcOptions().blocked()
		   && account->mtp().dcOptions().unenrolled()
		   && account->mtp().dcOptions().configEnumDcIds().empty()
		   && !account->local().mtpAuthorizationWriteFailed()
		   && !account->local().hasStoredCustomServer()
		   && !account->local().customServerPinUnknown()
		   && HasNoAuthorizationState(account)
		   && (account->local().checkServerCacheBinding(
				   kSyntheticStockCacheBindingForRegression.fingerprint,
				   kSyntheticStockCacheBindingForRegression.userId)
			   == Storage::ServerCacheBindingStatus::Match);
}

[[nodiscard]] bool
RestartDomain(Main::Domain &domain,
			  LifecycleWriteCountsForRegressionTest *teardownWriteCounts
			  = nullptr) {
	auto &app = Core::App();
	const auto applicationWindows = [&] {
		auto result = std::vector<Window::Controller *>();
		for (const auto widget : QApplication::topLevelWidgets()) {
			if (const auto window = app.findWindow(widget)) {
				if (std::find(result.begin(), result.end(), window)
					== result.end()) {
					result.push_back(window);
				}
			}
		}
		return result;
	};
	const auto windows = applicationWindows();
	for (const auto window : windows) {
		app.closeWindow(window);
	}
	if (!applicationWindows().empty()) {
		return false;
	}
	domain.local().writeAccounts();
	domain.finish();
	if (teardownWriteCounts) {
		*teardownWriteCounts = GetLifecycleWriteCountsForRegressionTest();
	}
	Storage::details::Sync();
	if (domain.start(QByteArray()) != Storage::StartResult::Success
		|| domain.accounts().empty()) {
		return false;
	}
	app.createPrimaryWindowForLifecycleRegression();
	const auto primary = app.activePrimaryWindow();
	if (!primary) {
		return false;
	}
	const auto active = &domain.active();
	if (primary->id().account != active) {
		primary->showAccount(active);
	}
	return &primary->account() == active;
}

[[nodiscard]] bool TeagramIconChoicePersistsAcrossSettingsReload() {
	auto settings = Core::Settings();
	if (Core::ReadTeagramIconChoice(settings)
		!= Core::TeagramIconChoice::MugSignal) {
		return false;
	}
	auto legacy = Core::Settings();
	legacy.writePref<bool>(Core::kLegacyTeagramIconChoicePreference, true);
	auto legacyReloaded = Core::Settings();
	legacyReloaded.addFromSerialized(legacy.serialize());
	if (Core::ReadTeagramIconChoice(legacyReloaded)
		!= Core::TeagramIconChoice::TPrimary) {
		return false;
	}
	for (const auto choice : {
			 Core::TeagramIconChoice::TPrimary,
			 Core::TeagramIconChoice::MugSignal,
			 Core::TeagramIconChoice::MugGreen,
			 Core::TeagramIconChoice::MugCrimson,
			 Core::TeagramIconChoice::TCrimson,
			 Core::TeagramIconChoice::MugBrown,
			 Core::TeagramIconChoice::TBrown,
		 }) {
		Core::WriteTeagramIconChoice(settings, choice);
		auto reloaded = Core::Settings();
		reloaded.addFromSerialized(settings.serialize());
		if (Core::ReadTeagramIconChoice(reloaded) != choice) {
			return false;
		}
	}
	std::fprintf(stderr,
				 "Teagram icon choice persistence regression passed.\n");
	return true;
}

[[nodiscard]] MTPUser RegressionUser(UserId id, bool self, const QString &phone,
									 bool premium = false);

[[nodiscard]] bool CacheMismatchWaitsForDestructiveConfirmation(
	not_null<Main::Account *> account,
	std::shared_ptr<MTP::details::RSAPublicKey> candidateKey,
	UserId candidateUserId, uint64 retainedFingerprint, uint64 retainedUserId) {
	if (!ConfigurePinnedServer(account, candidateKey)) {
		return false;
	}
	account->setSessionUserId(candidateUserId);
	if (account->createSession(RegressionUser(candidateUserId, true, QString()),
							   std::make_unique<Main::SessionSettings>())) {
		return false;
	}
	const auto candidateFingerprint = candidateKey->fingerprint();
	if (account->sessionExists()
		|| !account->serverCacheBindingMismatchPending()
		|| account->mtp().isServerEnrollmentNetworkAllowed()
		|| account->local().checkServerCacheBinding(
			retainedFingerprint,
			retainedUserId) != Storage::ServerCacheBindingStatus::Match
		|| account->local().checkServerCacheBinding(
			candidateFingerprint,
			candidateUserId.bare)
			!= Storage::ServerCacheBindingStatus::Mismatch) {
		return false;
	}
	if (account->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::IdentityChange,
			true)
		|| account->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::DestructiveConfirmation,
			false)
		|| account->local().serverReenrollmentPending()) {
		return false;
	}
	return account->local().checkServerCacheBinding(
		retainedFingerprint,
		retainedUserId) == Storage::ServerCacheBindingStatus::Match;
}

template <typename Result, typename Start>
[[nodiscard]] std::optional<Result> AwaitCacheCallback(
	Start start,
	int timeoutMs = 5000) {
	struct State {
		std::optional<Result> result;
		QEventLoop *loop = nullptr;
		bool active = true;
		bool completed = false;
	};
	const auto state = std::make_shared<State>();
	auto loop = QEventLoop();
	const auto application = QCoreApplication::instance();
	if (!application) {
		return std::nullopt;
	}
	state->loop = &loop;
	QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
	// The callback only posts owned state; the queued handler touches the loop
	// while it is active on this thread.
	start([state, application](Result result) mutable {
		QMetaObject::invokeMethod(
			application,
			[state, result = std::move(result)]() mutable {
				if (!state->active || state->completed || !state->loop) {
					return;
				}
				state->result.emplace(std::move(result));
				state->completed = true;
				state->loop->quit();
			},
			Qt::QueuedConnection);
	});
	loop.exec();
	state->active = false;
	state->loop = nullptr;
	return std::move(state->result);
}

[[nodiscard]] bool CacheCallbackTimeoutIsSafe() {
	const auto failure = AwaitCacheCallback<int>([](auto done) {
		done(-1);
	});
	if (!failure || (*failure != -1)) {
		return false;
	}
	auto release = std::promise<void>();
	const auto waitForRelease = release.get_future().share();
	auto delayed = std::thread();
	const auto timedOut = AwaitCacheCallback<int>([&](auto done) {
		delayed = std::thread([done = std::move(done), waitForRelease]() mutable {
			waitForRelease.wait();
			done(1);
		});
	}, 1);
	release.set_value();
	delayed.join();
	QCoreApplication::processEvents();
	return !timedOut;
}

[[nodiscard]] bool CacheOperationSucceeded(
		const std::optional<Storage::Cache::Error> &result) {
	return result
		&& (result->type == Storage::Cache::Error::Type::None);
}

[[nodiscard]] bool OpenCache(
		Storage::Cache::Database &cache,
		not_null<Main::Account*> account) {
	return CacheOperationSucceeded(AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache.open(account->local().cacheKey(), std::move(done));
		}));
}

[[nodiscard]] bool WriteCachePayload(
		not_null<Main::Account*> account,
		const Storage::Cache::Key &key,
		const QByteArray &payload) {
	auto cache = Core::App().databases().get(
		account->local().cachePath(),
		account->local().cacheSettings());
	if (!OpenCache(*cache, account)) {
		return false;
	}
	return CacheOperationSucceeded(AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache->put(key, QByteArray(payload), std::move(done));
		}));
}

[[nodiscard]] bool CachePayloadMatches(
		Storage::Cache::Database &cache,
		const Storage::Cache::Key &key,
		const QByteArray &expected) {
	const auto payload = AwaitCacheCallback<QByteArray>([&](auto done) {
		cache.get(key, std::move(done));
	});
	return payload && (*payload == expected);
}

[[nodiscard]] bool PersistedCachePayloadMatches(
		not_null<Main::Account*> account,
		const Storage::Cache::Key &key,
		const QByteArray &expected) {
	auto cache = Core::App().databases().get(
		account->local().cachePath(),
		account->local().cacheSettings());
	return OpenCache(*cache, account)
		&& CachePayloadMatches(*cache, key, expected);
}

using CacheGetter = Fn<Storage::Cache::Database &()>;
using CacheClearer = Fn<void()>;
using CachePointerGetter = Fn<Storage::Cache::Database *()>;
using RefusedCacheReaderRegression = Fn<bool()>;

class RefusedCacheLoader final : public Media::Streaming::Loader {
  public:
	static constexpr auto kSize = int64(81) * Loader::kPartSize;

	Storage::Cache::Key baseCacheKey() const override {
		return {0x5245465553454443ULL, 0x4143484554455354ULL};
	}

	int64 size() const override { return kSize; }

	void load(int64 offset) override {
		QMutexLocker lock(&_mutex);
		_loaded.emplace(offset);
	}

	void cancel(int64) override {}

	void resetPriorities() override {}

	void setPriority(int) override {}

	void stop() override {}

	void tryRemoveFromQueue() override {}

	rpl::producer<Media::Streaming::LoadedPart> parts() const override {
		return _parts.events();
	}

	rpl::producer<Media::Streaming::SpeedEstimate>
	speedEstimate() const override {
		return _speed.events();
	}

	void
	attachDownloader(not_null<Storage::StreamedFileDownloader *>) override {}

	void clearAttachedDownloader() override {}

	[[nodiscard]] bool loaded(int64 offset) const {
		QMutexLocker lock(&_mutex);
		return _loaded.contains(offset);
	}

  private:
	mutable QMutex _mutex;
	std::set<int64> _loaded;
	rpl::event_stream<Media::Streaming::LoadedPart> _parts;
	rpl::event_stream<Media::Streaming::SpeedEstimate> _speed;
};

[[nodiscard]] bool
RunRefusedCacheStreamingRegression(CachePointerGetter cache) {
	auto loader = std::make_unique<RefusedCacheLoader>();
	auto rawLoader = loader.get();
	auto reader = std::make_shared<Media::Streaming::Reader>(std::move(loader),
															 std::move(cache));
	auto notify = std::make_shared<crl::semaphore>();
	struct Result {
		std::atomic<bool> completed = false;
		std::atomic<bool> cancelled = false;
		Media::Streaming::Reader::FillState state
			= Media::Streaming::Reader::FillState::WaitingCache;
	};
	const auto result = std::make_shared<Result>();
	auto entered = QSemaphore();
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	auto streaming = std::thread([=, &entered, &loop] {
		auto buffer = QByteArray(1, Qt::Uninitialized);
		auto state
			= reader->fill(0, bytes::make_detached_span(buffer), notify.get());
		entered.release();
		while ((state != Media::Streaming::Reader::FillState::Failed)
			   && !rawLoader->loaded(0)
			   && !result->cancelled.load(std::memory_order_acquire)) {
			notify->acquire();
			if (result->cancelled.load(std::memory_order_acquire)) {
				break;
			}
			state = reader->fill(0, bytes::make_detached_span(buffer),
								 notify.get());
		}
		result->state = state;
		result->completed.store(true, std::memory_order_release);
		QMetaObject::invokeMethod(
			&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
	});
	entered.acquire();
	if (!result->completed.load(std::memory_order_acquire)) {
		timeout.start(5000);
		loop.exec();
	}
	const auto completed = result->completed.load(std::memory_order_acquire);
	if (!completed) {
		result->cancelled.store(true, std::memory_order_release);
		notify->release();
	}
	streaming.join();
	reader->stopStreaming();
	return completed
		   && (result->state
			   == Media::Streaming::Reader::FillState::WaitingRemote)
		   && rawLoader->loaded(0);
}

[[nodiscard]] bool
RunRefusedCacheDownloaderRegression(not_null<Main::Account *> account,
									CachePointerGetter cache) {
	auto loader = std::make_unique<RefusedCacheLoader>();
	auto rawLoader = loader.get();
	auto reader = std::make_shared<Media::Streaming::Reader>(std::move(loader),
															 std::move(cache));
	auto downloader = std::make_unique<Storage::StreamedFileDownloader>(
		not_null<Main::Session *>(&account->session()), 1, 2,
		Data::FileOrigin(), Storage::Cache::Key{1, 2}, MediaKey{0, 0}, reader,
		Core::MacProtectedPath::ProfileRoot()
			+ u"tdata/refused_cache_download"_q,
		RefusedCacheLoader::kSize, DocumentFileLocation, LoadToFileOnly,
		LoadFromCloudOrLocal, false, 0);
	auto loop = QEventLoop();
	auto timeout = QTimer();
	auto poll = QTimer();
	timeout.setSingleShot(true);
	poll.setInterval(10);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (rawLoader->loaded(0)) {
			loop.quit();
		}
	});
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	reader->loadForDownloader(downloader.get(), 0);
	reader->continueDownloaderFromMainThread();
	if (!rawLoader->loaded(0)) {
		poll.start();
		timeout.start(5000);
		loop.exec();
	}
	poll.stop();
	return rawLoader->loaded(0);
}

[[nodiscard]] bool
RunRefusedCacheReaderRegressions(not_null<Main::Account *> account,
								 bool mediaCache) {
	const auto cache = [=] {
		auto &data = account->session().data();
		return mediaCache ? data.cacheBigFileIfAllowed()
						  : data.cacheIfAllowed();
	};
	const auto streaming = RunRefusedCacheStreamingRegression(cache);
	const auto downloading
		= RunRefusedCacheDownloaderRegression(account, cache);
	return streaming && downloading;
}

[[nodiscard]] bool RunCacheConcurrentDeletionRegression() {
#if defined(Q_OS_MAC) && defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto parent
		= QDir(Core::MacProtectedPath::ProfileRoot()).filePath("tdata");
	if (!QDir().mkpath(parent)) {
		return false;
	}
	auto directory
		= QTemporaryDir(QDir(parent).filePath("cache-scan-deletion-XXXXXX"));
	const auto vanished = QDir(directory.path()).filePath("vanished");
	if (!directory.isValid()
		|| !WriteFixtureFile(vanished, "synthetic cache entry")) {
		return false;
	}
	auto deleted = false;
	const auto allowed = Core::MacProtectedPath::CheckCachePathForTesting(
		directory.path(), "Tests::ProtectedCache::concurrent-deletion",
		[&](const QString &entry) {
			if (entry != vanished) {
				return;
			}
			auto remove = std::thread([&] { deleted = QFile::remove(entry); });
			remove.join();
		});
	return allowed && deleted && !QFileInfo::exists(vanished);
#else
	return true;
#endif
}

[[nodiscard]] QString ActiveCacheVersionPath(const QString &path) {
	const auto directories
		= QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const auto &directory : directories) {
		const auto version = QDir(path).filePath(directory);
		if (QFileInfo(version + "/binlog").isFile()) {
			return version;
		}
	}
	return {};
}

[[nodiscard]] bool
RunPostOpenCacheSymlinkRegression(const QString &path, const QString &fixture,
								  CacheGetter cache, CacheClearer clearCaches,
								  RefusedCacheReaderRegression cacheRefusal) {
	const auto seedKey = Storage::Cache::Key{
		0x4d41494e39353731ULL,
		0x4341434845534545ULL,
	};
	const auto writeKey = Storage::Cache::Key{
		0x4d41494e39353732ULL,
		0x4341434845575249ULL,
	};
	const auto payload = QByteArray("post-open cache guard seed");
	const auto seed = AwaitCacheCallback<Storage::Cache::Error>([&](auto done) {
		cache().put(seedKey, QByteArray(payload), std::move(done));
	});
	if (!CacheOperationSucceeded(seed)) {
		std::fprintf(stderr,
					 "Post-open cache setup failed: seed error=%d path=%s.\n",
					 seed ? int(seed->type) : -1, qPrintable(path));
		return false;
	}
	cache().sync();

	const auto active = ActiveCacheVersionPath(path);
	const auto saved = active + ".guard-test-original";
	if (active.isEmpty() || QFileInfo::exists(saved)
		|| !QDir().rename(active, saved)) {
		return false;
	}
	auto restored = false;
	const auto restore = gsl::finally([&] {
		if (!restored) {
			QFile::remove(active);
			QDir().rename(saved, active);
		}
	});
	if (!QFile::link(fixture, active)) {
		return false;
	}
	const auto readerRefused = cacheRefusal();
	const auto read = AwaitCacheCallback<QByteArray>(
		[&](auto done) { cache().get(seedKey, std::move(done)); });
	const auto readRefused = read && read->isEmpty();
	const auto write = AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache().put(writeKey, QByteArray("must not reach protected cache"),
						std::move(done));
		});
	clearCaches();
	cache().sync();
	const auto writeRefused
		= write && (write->type != Storage::Cache::Error::Type::None);
	const auto aliasRemoved = QFile::remove(active);
	const auto originalRestored = QDir().rename(saved, active);
	restored = aliasRemoved && originalRestored;
	cache().sync();
	const auto passed
		= readerRefused && readRefused && writeRefused && restored;
	if (!passed) {
		std::fprintf(
			stderr,
			"Post-open cache regression failed: reader=%d read=%d write=%d "
			"restored=%d path=%s.\n",
			readerRefused, readRefused, writeRefused, restored,
			qPrintable(path));
	}
	return passed;
}

[[nodiscard]] Main::Account *
FindAuthorizationBlockedAccount(const Main::Domain &domain) {
	for (const auto &[index, account] : domain.accounts()) {
		if (account->local().mtpAuthorizationWriteFailed()) {
			return account.get();
		}
	}
	return nullptr;
}

[[nodiscard]] MTPUser RegressionUser(UserId id, bool self, const QString &phone,
									 bool premium) {
	const auto flags
		= (self ? MTPDuser::Flag::f_self : MTPDuser::Flag())
		  | (phone.isEmpty() ? MTPDuser::Flag() : MTPDuser::Flag::f_phone)
		  | (premium ? MTPDuser::Flag::f_premium : MTPDuser::Flag());
	return MTP_user(MTP_flags(flags), MTP_long(id.bare), MTPlong(),
					MTP_string(u"Regression"_q), MTPstring(), MTPstring(),
					MTP_string(phone), MTPUserProfilePhoto(), MTPUserStatus(),
					MTPint(), MTP_vector<MTPRestrictionReason>(0), MTPstring(),
					MTPstring(), MTPEmojiStatus(), MTP_vector<MTPUsername>(0),
					MTPRecentStory(), MTPPeerColor(), MTPPeerColor(), MTPint(),
					MTPlong(), MTPlong(), MTPlong());
}

[[nodiscard]] bool
SetRegressionSessionPremium(not_null<Main::Session *> session, UserId selfId,
							bool premium) {
	session->data().processUsers(MTP_vector<MTPUser>({
		RegressionUser(selfId, true, QString(), premium),
	}));
	return session->premium() == premium;
}

[[nodiscard]] MTPmessages_ChatFull
RegressionChatFullReply(ChatId chatId, int version, QString selfPhone,
						UserId creatorId, bool memberIsAdmin = false,
						bool selfPremium = false) {
	const auto memberId = (creatorId == UserId(1)) ? UserId(2) : UserId(1);
	const auto member
		= memberIsAdmin
			  ? MTP_chatParticipantAdmin(
					MTP_flags(MTPDchatParticipantAdmin::Flags()),
					MTP_long(memberId.bare), MTP_long(creatorId.bare),
					MTP_int(0), MTP_string(QString()))
			  : MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()),
									MTP_long(memberId.bare),
									MTP_long(creatorId.bare), MTP_int(0),
									MTP_string(QString()));
	const auto participants = MTP_chatParticipants(
		MTP_long(chatId.bare),
		MTP_vector<MTPChatParticipant>({
			MTP_chatParticipantCreator(
				MTP_flags(MTPDchatParticipantCreator::Flags()),
				MTP_long(creatorId.bare), MTP_string(QString())),
			member,
			MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()),
								MTP_long(UserId(9).bare),
								MTP_long(UserId(1).bare), MTP_int(0),
								MTP_string(QString())),
		}),
		MTP_int(version));
	const auto notifySettings = MTP_peerNotifySettings(
		MTP_flags(MTPDpeerNotifySettings::Flags()), MTP_boolFalse(),
		MTP_boolFalse(), MTP_int(0), MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(), MTP_notificationSoundDefault(),
		MTP_boolFalse(), MTP_boolFalse(), MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(), MTP_notificationSoundDefault());
	const auto fullChat = MTP_chatFull(
		MTP_flags(MTPDchatFull::Flags()), MTP_long(chatId.bare),
		MTP_string(QString()), participants, MTP_photoEmpty(MTP_long(0)),
		notifySettings, MTPExportedChatInvite(), MTP_vector<MTPBotInfo>(0),
		MTPint(), MTPint(), MTPInputGroupCall(), MTPint(), MTPPeer(),
		MTPstring(), MTPint(), MTP_vector<MTPlong>(0), MTP_chatReactionsNone(),
		MTPint());
	return MTP_messages_chatFull(
		fullChat, MTP_vector<MTPChat>(0),
		MTP_vector<MTPUser>({
			RegressionUser(UserId(1), true, selfPhone, selfPremium),
			RegressionUser(UserId(2), false, u"2"_q),
			RegressionUser(UserId(9), false, u"9"_q),
		}));
}

[[nodiscard]] MTPmessages_ChatFull
RegressionStockChatFullReply(ChatId chatId, int version, QString selfPhone,
							 UserId creatorId, bool memberIsAdmin = false) {
	return RegressionChatFullReply(chatId, version, std::move(selfPhone),
								   creatorId, memberIsAdmin, true);
}

// A bot peer for the webview fixtures: the bot flag plus a bot info
// version make UserData::isBot() true, which is what the deferred completion
// of an app open requires before it builds the app. It carries no
// username, so a username lookup misses it and the deferred path is taken.
[[nodiscard]] MTPUser RegressionBotUser(UserId id) {
	return MTP_user(MTP_flags(MTPDuser::Flag::f_bot), MTP_long(id.bare),
					MTPlong(), MTP_string(u"Regression Bot"_q), MTPstring(),
					MTPstring(), MTPstring(), MTPUserProfilePhoto(),
					MTPUserStatus(), MTP_int(1),
					MTP_vector<MTPRestrictionReason>(0), MTPstring(),
					MTPstring(), MTPEmojiStatus(), MTP_vector<MTPUsername>(0),
					MTPRecentStory(), MTPPeerColor(), MTPPeerColor(), MTPint(),
					MTPlong(), MTPlong(), MTPlong());
}

// A username open on the session that owns it, taken session by session so
// that no reference can outlive the session it was bound to.
[[nodiscard]] bool
OpenMiniAppByUsername(not_null<Main::Session *> session,
					  not_null<Window::SessionController *> controller) {
	return session->attachWebView().openByUsername(
		controller, Api::SendAction(session->data().history(session->user())),
		u"regression_bot"_q, QString(), false);
}

[[nodiscard]] bool HasParticipant(not_null<ChatData *> chat, UserId id) {
	return std::any_of(
		chat->participants.begin(), chat->participants.end(),
		[id](not_null<UserData *> user) { return peerToUser(user->id) == id; });
}

[[nodiscard]] bool HasExpectedParticipants(not_null<ChatData *> chat,
										   UserId creatorId) {
	return (chat->participants.size() == 3) && (chat->creator == creatorId)
		   && HasParticipant(chat, UserId(1)) && HasParticipant(chat, UserId(2))
		   && HasParticipant(chat, UserId(9));
}

// Counters that make a mini-app open observable: the shared unavailable-server
// toast, the app requests that create a webview instance, the activations of an
// instance, the username resolves that precede an app request, and the live
// instances. Read straight from the owning session, so a refusal that returns
// false after sending a request or activating an instance cannot pass.
struct WebViewOpenCounters {
	int instances = 0;
	int requests = 0;
	int activations = 0;
	int resolves = 0;
	int toasts = 0;
};

[[nodiscard]] WebViewOpenCounters
ReadWebViewOpenCounters(not_null<Main::Session *> session) {
	const auto &webView = session->attachWebView();
	return {
		.instances = webView.liveInstancesCountForRegressionTest(),
		.requests = webView.appRequestCountForRegressionTest(),
		.activations = webView.appActivateCountForRegressionTest(),
		.resolves = webView.usernameResolveCountForRegressionTest(),
		.toasts = webView.unavailableToastCountForRegressionTest(),
	};
}

[[nodiscard]] bool WebViewOpenDeltaMatches(const char *stage,
										   const WebViewOpenCounters &before,
										   const WebViewOpenCounters &after,
										   int toasts, int requests,
										   int activations, int resolves,
										   int instances) {
	if (((after.toasts - before.toasts) == toasts)
		&& ((after.requests - before.requests) == requests)
		&& ((after.activations - before.activations) == activations)
		&& ((after.resolves - before.resolves) == resolves)
		&& ((after.instances - before.instances) == instances)) {
		return true;
	}
	std::fprintf(stderr,
				 "Mini-app open regression %s: toasts %d->%d, requests %d->%d, "
				 "activations %d->%d, resolves %d->%d, instances %d->%d "
				 "(expected +%d toasts, +%d requests, +%d activations, "
				 "+%d resolves, +%d instances)\n",
				 stage, before.toasts, after.toasts, before.requests,
				 after.requests, before.activations, after.activations,
				 before.resolves, after.resolves, before.instances,
				 after.instances, toasts, requests, activations, resolves,
				 instances);
	return false;
}

// Every entry class of the accepted matrix runs through the one gate all of
// them share: the keyboard WebView button, the SimpleWebView button, the
// inline switch, app and bot-profile links, the attachment link and attach
// menu, the Apps tab, the bot menu, game callbacks, the profile entry, age
// verification, and the invitation link. It returns how many opens the
// session accepted.
constexpr int kRegressionEntryClassCount = 13;

[[nodiscard]] bool SettingsSearchHasEntry(
	const std::vector<Settings::Builder::SearchEntry> &entries,
	const QString &id) {
	return std::any_of(entries.begin(), entries.end(),
					   [&](const auto &entry) { return entry.id == id; });
}

[[nodiscard]] bool SettingsSearchHasPaidEntry(
	const std::vector<Settings::Builder::SearchEntry> &entries) {
	return std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
		return entry.section == Settings::PremiumId()
			   || entry.section == Settings::CreditsId()
			   || entry.section == Settings::CurrencyId()
			   || entry.section == Settings::BusinessId()
			   || entry.id.startsWith(u"premium/"_q)
			   || entry.id.startsWith(u"stars/"_q)
			   || entry.id.startsWith(u"business/"_q)
			   || entry.id == u"main/premium"_q || entry.id == u"main/credits"_q
			   || entry.id == u"main/currency"_q
			   || entry.id == u"main/business"_q
			   || entry.id == u"main/send-gift"_q;
	});
}

[[nodiscard]] bool SettingsSearchHasSectionTitle(
	const std::vector<Settings::Builder::SearchEntry> &entries,
	Settings::Type section) {
	return std::any_of(entries.begin(), entries.end(), [&](const auto &entry) {
		return entry.id.isEmpty() && entry.section == section;
	});
}

[[nodiscard]] bool
SettingsSearchMatchesExpected(not_null<Main::Session *> session,
							  bool paidSupported, bool bioSupported,
							  bool botAppsSupported) {
	const auto entries
		= Settings::Builder::SearchRegistry::Instance().collectAll(session);
	const auto hasPaid = SettingsSearchHasPaidEntry(entries);
	const auto hasPrivacy
		= SettingsSearchHasEntry(entries, u"privacy/last_seen"_q);
	const auto hasBio = SettingsSearchHasEntry(entries, u"edit/bio"_q);
	const auto hasChatAutomation
		= SettingsSearchHasEntry(entries, u"edit/chat-automation"_q);
	const auto callsSupported = session->callsSupported();
	const auto hasCallsAcceptEntry
		= SettingsSearchHasEntry(entries, u"calls/accept"_q);
	const auto hasCallsNotificationsSection
		= SettingsSearchHasEntry(entries, u"notifications/calls"_q);
	const auto hasCallsNotificationsAcceptEntry
		= SettingsSearchHasEntry(entries, u"notifications/calls/accept"_q);
	const auto privacyCallsExpected
		= callsSupported
		  && Settings::details::IsPrivacyKeySupported(
			  Api::UserPrivacy::Key::Calls);
	const auto hasPrivacyCallsEntry
		= SettingsSearchHasEntry(entries, u"privacy/calls"_q);
	const auto hasAcceptCallSectionKeyword
		= std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
			  return (entry.id == u"calls/other"_q)
					 && entry.keywords.contains(u"accept"_q);
		  });
	const auto hasSystemPreferencesEntry
		= SettingsSearchHasEntry(entries, u"calls/system-prefs"_q);
	const auto hasYourColorEntry
		= SettingsSearchHasEntry(entries, u"edit/your-color"_q);
	const auto hasPeerColorEntry
		= SettingsSearchHasEntry(entries, u"chat/peer-color"_q);
	const auto hasPrivacyGiftsEntry
		= SettingsSearchHasEntry(entries, u"privacy/gifts"_q);
	const auto hasStockPaidRows
		= SettingsSearchHasEntry(entries, u"premium/no_ads"_q)
		  && SettingsSearchHasEntry(entries, u"stars/gift"_q)
		  && SettingsSearchHasEntry(entries, u"business/sponsored"_q)
		  && SettingsSearchHasEntry(entries, u"main/premium"_q)
		  && SettingsSearchHasEntry(entries, u"main/credits"_q)
		  && SettingsSearchHasEntry(entries, u"main/currency"_q)
		  && SettingsSearchHasEntry(entries, u"main/business"_q)
		  && SettingsSearchHasSectionTitle(entries, Settings::PremiumId())
		  && SettingsSearchHasSectionTitle(entries, Settings::CreditsId())
		  && SettingsSearchHasSectionTitle(entries, Settings::BusinessId());
	const auto matches
		= hasPaid == paidSupported && hasPrivacy && (hasBio == bioSupported)
		  && (hasChatAutomation == botAppsSupported)
		  && (hasCallsAcceptEntry == callsSupported)
		  && (hasCallsNotificationsSection == callsSupported)
		  && (hasCallsNotificationsAcceptEntry == callsSupported)
		  && (hasPrivacyCallsEntry == privacyCallsExpected)
		  && (callsSupported || !hasAcceptCallSectionKeyword)
		  && hasSystemPreferencesEntry && (hasYourColorEntry == paidSupported)
		  && (hasPeerColorEntry == paidSupported)
		  && (paidSupported || !hasPrivacyGiftsEntry)
		  && (!paidSupported || hasStockPaidRows);
	if (!matches) {
		std::fprintf(
			stderr,
			"Settings search regression failed: paid=%d expected=%d, "
			"privacy=%d, bio=%d expected=%d, chat-automation=%d "
			"expected=%d, calls-accept=%d, calls-notifications=%d, "
			"calls-notifications-accept=%d, privacy-calls=%d expected=%d, "
			"call-accept-keyword=%d, system-prefs=%d, your-color=%d, "
			"peer-color=%d, privacy-gifts=%d, stock-rows=%d.\n",
			hasPaid, paidSupported, hasPrivacy, hasBio, bioSupported,
			hasChatAutomation, botAppsSupported, hasCallsAcceptEntry,
			hasCallsNotificationsSection, hasCallsNotificationsAcceptEntry,
			hasPrivacyCallsEntry, privacyCallsExpected,
			hasAcceptCallSectionKeyword, hasSystemPreferencesEntry,
			hasYourColorEntry, hasPeerColorEntry, hasPrivacyGiftsEntry,
			hasStockPaidRows);
	}
	return matches;
}

template <typename Predicate>
[[nodiscard]] bool WaitForRegressionCondition(Predicate condition) {
	if (condition()) {
		return true;
	}
	auto loop = QEventLoop();
	auto timeout = QTimer();
	auto poll = QTimer();
	timeout.setSingleShot(true);
	poll.setInterval(10);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (condition()) {
			loop.quit();
		}
	});
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	poll.start();
	timeout.start(5000);
	loop.exec();
	poll.stop();
	return condition();
}

[[nodiscard]] bool
ActivateWindowForRegression(Core::Application &app,
							not_null<Window::Controller *> window,
							bool requirePrimary = false) {
	const auto isActive = [&] {
		return (app.activeWindow() == window.get())
			   && (!requirePrimary
				   || (app.activePrimaryWindow() == window.get()));
	};
	auto retry = QTimer();
	retry.setInterval(100);
	QObject::connect(&retry, &QTimer::timeout, &retry, [&] {
		if (!isActive()) {
			window->activate();
		}
	});
	retry.start();
	window->activate();
	const auto activated = WaitForRegressionCondition(isActive);
	retry.stop();
	return activated;
}

[[nodiscard]] bool
ShownInfoSectionMatches(not_null<Window::SessionController *> controller,
						Fn<bool(const Info::Section &)> matches) {
	const auto widgets = controller->widget()->findChildren<QWidget *>();
	for (const auto widget : widgets) {
		const auto wrap = dynamic_cast<Info::WrapWidget *>(widget);
		if (wrap && wrap->isVisible()
			&& matches(wrap->controller()->section())) {
			return true;
		}
	}
	return false;
}

struct SettingsDeepLinkInfoCounts {
	int wraps = 0;
	int visibleWraps = 0;
	int notHiddenWraps = 0;
	int settingsWraps = 0;
	int informationWraps = 0;
	int visibleInformationWraps = 0;
	int notHiddenInformationWraps = 0;
	int storiesWraps = 0;
};

[[nodiscard]] SettingsDeepLinkInfoCounts CountSettingsDeepLinkInfoSections(
	not_null<Window::SessionController *> controller) {
	auto result = SettingsDeepLinkInfoCounts();
	const auto widgets = controller->widget()->findChildren<QWidget *>();
	for (const auto widget : widgets) {
		const auto wrap = dynamic_cast<Info::WrapWidget *>(widget);
		if (!wrap) {
			continue;
		}
		++result.wraps;
		const auto visible = wrap->isVisible();
		const auto notHidden = !wrap->isHidden();
		result.visibleWraps += visible;
		result.notHiddenWraps += notHidden;
		const auto &section = wrap->controller()->section();
		if (section.type() == Info::Section::Type::Settings) {
			++result.settingsWraps;
			if (section.settingsType() == Settings::InformationId()) {
				++result.informationWraps;
				result.visibleInformationWraps += visible;
				result.notHiddenInformationWraps += notHidden;
			}
		} else if (section.type() == Info::Section::Type::Stories) {
			++result.storiesWraps;
		}
	}
	return result;
}

[[nodiscard]] bool
ShownStoriesSection(not_null<Window::SessionController *> controller) {
	return ShownInfoSectionMatches(
		controller, [](const Info::Section &section) {
			return (section.type() == Info::Section::Type::Stories);
		});
}

template <typename Condition>
[[nodiscard]] bool WaitForSettingsDeepLinkCondition(Condition condition) {
	if (condition()) {
		return true;
	}
	auto loop = QEventLoop();
	auto poll = QTimer();
	auto deadline = QTimer();
	poll.setInterval(10);
	deadline.setSingleShot(true);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (condition()) {
			loop.quit();
		}
	});
	QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
	poll.start();
	deadline.start(1000);
	loop.exec();
	poll.stop();
	deadline.stop();
	return condition();
}

[[nodiscard]] bool RunSettingsDeepLinkRefusalRegression(
	Core::Application &app, not_null<Main::Account *> stock,
	not_null<Main::Account *> pinned, not_null<Main::Account *> blocked,
	not_null<Main::Account *> restoredBlocked,
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> pinnedController,
	not_null<Window::SessionController *> blockedController) {
	const auto routes = std::vector<QString>{
		u"tg://settings/premium"_q,
		u"tg://settings/PREMIUM"_q,
		u"tg://settings/premium/x"_q,
		u"tg://settings/stars"_q,
		u"tg://settings/stars/top-up"_q,
		u"tg://settings/stars/stats"_q,
		u"tg://settings/stars/gift"_q,
		u"tg://settings/stars/earn"_q,
		u"tg://settings/stars/unlisted"_q,
		u"tg://settings/ton"_q,
		u"tg://settings/ton/x"_q,
		u"tg://settings/business"_q,
		u"tg://settings/business/do-not-hide-ads"_q,
		u"tg://settings/business/unlisted"_q,
		u"tg://settings/send-gift"_q,
		u"tg://settings/send-gift/x"_q,
		u"tg://settings/send-gift/self"_q,
		u"tg://settings/send-gift/self/x"_q,
		u"tg://settings/emoji-status"_q,
		u"tg://chats/emoji-status"_q,
		u"tg://settings/profile-color"_q,
		u"tg://settings/profile-color/name"_q,
		u"tg://settings/profile-color/profile/use-gift"_q,
		u"tg://settings/appearance/your-color"_q,
		u"tg://settings/appearance/your-color/name/add-icons"_q,
		u"tg://settings/edit/your-color"_q,
		u"tg://settings/privacy/gifts"_q,
		u"tg://settings/privacy/gifts/always"_q,
	};
	const auto storyRoutes = std::vector<QString>{
		u"tg://settings/my-profile/posts"_q,
		u"tg://settings/my-profile/posts/add-album"_q,
		u"tg://settings/my-profile/archived-posts"_q,
		u"tg://settings/MY-PROFILE/ARCHIVED-POSTS"_q,
	};
	const auto sameUserId
		= stock->session().uniqueId() == pinned->session().uniqueId()
		  && pinned->session().uniqueId() == blocked->session().uniqueId();
	const auto stockPaidSupported = stock->session().paidFeaturesSupported();
	const auto customPaidUnsupported
		= !pinned->session().paidFeaturesSupported();
	const auto blockedPaidUnsupported
		= !blocked->session().paidFeaturesSupported();
	const auto restoredBlockedIsDistinct
		= (restoredBlocked.get() != stock.get())
		  && (restoredBlocked.get() != pinned.get())
		  && (restoredBlocked.get() != blocked.get());
	const auto restoredBlockedHasNoSession = !restoredBlocked->sessionExists();
	const auto restoredBlockedConfigBlocked
		= restoredBlocked->mtp().config().blocked();
	const auto restoredBlockedHasNoCustomConfig
		= !restoredBlocked->mtp().dcOptions().hasCustomServer();
	const auto restoredBlockedHasNoStoredCustom
		= !restoredBlocked->local().hasStoredCustomServer();
	const auto restoredBlockedPinStateKnown
		= !restoredBlocked->local().customServerPinUnknown();
	const auto restoredBlockedAuthorizationFailed
		= restoredBlocked->local().mtpAuthorizationWriteFailed();
	const auto restoredBlockedNetworkAllowed
		= restoredBlocked->mtp().isServerEnrollmentNetworkAllowed();
	const auto restoredBlockedWithoutPin
		= restoredBlockedHasNoSession && restoredBlockedConfigBlocked
		  && restoredBlockedHasNoCustomConfig
		  && restoredBlockedHasNoStoredCustom && restoredBlockedPinStateKnown
		  && restoredBlockedAuthorizationFailed
		  && !restoredBlockedNetworkAllowed;
	const auto customIsPinned = pinned->mtp().dcOptions().hasCustomServer();
	const auto customIsUnblocked = !pinned->mtp().dcOptions().blocked();
	const auto blockedHasNoCustomConfig
		= !blocked->mtp().dcOptions().hasCustomServer();
	const auto blockedConfigBlocked = blocked->mtp().dcOptions().blocked();
	const auto blockedHasNoStoredCustom
		= !blocked->local().hasStoredCustomServer();
	const auto blockedPinStateKnown
		= !blocked->local().customServerPinUnknown();
	const auto stockControllerOwnsSession
		= (&stockController->session() == &stock->session());
	const auto customControllerOwnsSession
		= (&pinnedController->session() == &pinned->session());
	const auto blockedControllerOwnsSession
		= (&blockedController->session() == &blocked->session());
	const auto configured
		= sameUserId && stockPaidSupported && customPaidUnsupported
		  && blockedPaidUnsupported && restoredBlockedIsDistinct
		  && restoredBlockedWithoutPin && customIsPinned && customIsUnblocked
		  && blockedHasNoCustomConfig && blockedConfigBlocked
		  && blockedHasNoStoredCustom && blockedPinStateKnown
		  && stockControllerOwnsSession && customControllerOwnsSession
		  && blockedControllerOwnsSession;
	std::fprintf(
		stderr,
		"Settings deep link fixture checks: same_user=%d stock_paid=%d "
		"custom_paid_unsupported=%d blocked_paid_unsupported=%d "
		"restored_distinct=%d restored_no_session=%d restored_blocked=%d "
		"restored_no_custom_config=%d restored_no_stored_custom=%d "
		"restored_pin_state_known=%d restored_auth_failed=%d "
		"restored_network_allowed=%d "
		"custom_pinned=%d custom_unblocked=%d blocked_no_custom_config=%d "
		"blocked_config_blocked=%d blocked_no_stored_custom=%d "
		"blocked_pin_state_known=%d controllers=%d/%d/%d.\n",
		sameUserId, stockPaidSupported, customPaidUnsupported,
		blockedPaidUnsupported, restoredBlockedIsDistinct,
		restoredBlockedHasNoSession, restoredBlockedConfigBlocked,
		restoredBlockedHasNoCustomConfig, restoredBlockedHasNoStoredCustom,
		restoredBlockedPinStateKnown, restoredBlockedAuthorizationFailed,
		restoredBlockedNetworkAllowed, customIsPinned, customIsUnblocked,
		blockedHasNoCustomConfig, blockedConfigBlocked,
		blockedHasNoStoredCustom, blockedPinStateKnown,
		stockControllerOwnsSession, customControllerOwnsSession,
		blockedControllerOwnsSession);
	if (!configured) {
		std::fprintf(
			stderr,
			"Settings deep link regression failed: session fixtures did not "
			"match stock, custom, blocked, and restored blocked accounts.\n");
		return false;
	}

	const auto previousWindow = app.activeWindow();
	const auto restoreWindows = gsl::finally([&] {
		for (const auto controller : {
				 stockController.get(),
				 pinnedController.get(),
				 blockedController.get(),
			 }) {
			if (controller->isLayerShown()) {
				controller->hideLayer(anim::type::instant);
			}
			controller->clearSectionStack(Window::SectionShow(
				anim::type::instant, anim::activation::background));
		}
		if (previousWindow) {
			previousWindow->activate();
		}
	});
	const auto resetController
		= [](not_null<Window::SessionController *> controller) {
			  if (controller->isLayerShown()) {
				  controller->hideLayer(anim::type::instant);
			  }
			  controller->clearSectionStack(Window::SectionShow(
				  anim::type::instant, anim::activation::background));
		  };
	for (const auto &controller : {
			 stockController,
			 pinnedController,
			 blockedController,
		 }) {
		resetController(controller);
	}
	QCoreApplication::processEvents();
	for (const auto &controller : {
			 stockController,
			 pinnedController,
			 blockedController,
		 }) {
		if (controller->content()->isMainSectionShown()
			|| controller->isLayerShown()) {
			std::fprintf(
				stderr,
				"Settings deep link regression failed: test window did not "
				"start at its base section.\n");
			return false;
		}
	}

	const auto contextFor
		= [](not_null<Window::SessionController *> controller) {
			  return QVariant::fromValue(ClickHandlerContext{
				  .sessionWindow = base::make_weak(controller.get()),
			  });
		  };
	const auto dispatchRefused = [&](const QString &url,
									 not_null<Window::SessionController *>
										 target,
									 Window::Controller *activeBefore) {
		const auto stockToastBefore
			= stockController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto pinnedToastBefore
			= pinnedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto blockedToastBefore
			= blockedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto handled = app.openLocalUrl(url, contextFor(target));
		QCoreApplication::processEvents();
		const auto targetToastOnly
			= (stockController
				   ->featureUnavailableOnServerToastCallsForRegressionTest()
			   == stockToastBefore + (target.get() == stockController.get()))
			  && (pinnedController
					  ->featureUnavailableOnServerToastCallsForRegressionTest()
				  == pinnedToastBefore
						 + (target.get() == pinnedController.get()))
			  && (blockedController
					  ->featureUnavailableOnServerToastCallsForRegressionTest()
				  == blockedToastBefore
						 + (target.get() == blockedController.get()));
		const auto noPaidUiOpen
			= !stockController->content()->isMainSectionShown()
			  && !pinnedController->content()->isMainSectionShown()
			  && !blockedController->content()->isMainSectionShown()
			  && !stockController->isLayerShown()
			  && !pinnedController->isLayerShown()
			  && !blockedController->isLayerShown();
		const auto refused = handled && targetToastOnly && noPaidUiOpen
							 && (app.activeWindow() == activeBefore);
		if (!refused) {
			std::fprintf(
				stderr,
				"Settings deep link regression failed: route=%s handled=%d "
				"toast-only=%d paid-ui-open=%d "
				"active=%p expected=%p.\n",
				url.toUtf8().constData(), handled, targetToastOnly,
				!noPaidUiOpen, static_cast<void *>(app.activeWindow()),
				static_cast<void *>(activeBefore));
		}
		return refused;
	};
	const auto dispatchMatrix
		= [&](not_null<Window::SessionController *> target,
			  Window::Controller *activeBefore) {
			  for (const auto &url : routes) {
				  if (!dispatchRefused(url, target, activeBefore)) {
					  return false;
				  }
			  }
			  for (const auto &url : storyRoutes) {
				  if (!dispatchRefused(url, target, activeBefore)) {
					  return false;
				  }
			  }
			  return true;
		  };
	const auto activateSettingsWindow = [&](Window::Controller *window) {
		return ActivateWindowForRegression(app, window);
	};

	if (!activateSettingsWindow(&stockController->window())) {
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "expected=1.\n",
					 app.activeWindow() == &stockController->window());
		std::fprintf(
			stderr,
			"Settings deep link regression failed: stock window did not "
			"activate before the custom-session matrix.\n");
		return false;
	}
	if (!dispatchMatrix(pinnedController, &stockController->window())) {
		return false;
	}
	if (!activateSettingsWindow(&pinnedController->window())) {
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "expected=1.\n",
					 app.activeWindow() == &pinnedController->window());
		std::fprintf(
			stderr,
			"Settings deep link regression failed: custom window did not "
			"activate before its URL matrix.\n");
		return false;
	}
	if (!dispatchMatrix(pinnedController, &pinnedController->window())) {
		return false;
	}
	if (!activateSettingsWindow(&stockController->window())) {
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "expected=1.\n",
					 app.activeWindow() == &stockController->window());
		std::fprintf(
			stderr,
			"Settings deep link regression failed: stock window did not "
			"activate before the blocked-session matrix.\n");
		return false;
	}
	if (!dispatchMatrix(blockedController, &stockController->window())) {
		return false;
	}

	if (!activateSettingsWindow(&pinnedController->window())) {
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "expected=1.\n",
					 app.activeWindow() == &pinnedController->window());
		std::fprintf(
			stderr,
			"Settings deep link regression failed: custom window did not "
			"activate before the unsupported-route checks.\n");
		return false;
	}
	{
		const auto pinnedWindow = pinnedController->widget();
		const auto blockedWindow = blockedController->widget();
		const auto pinnedPreviousSize = pinnedWindow->size();
		const auto blockedPreviousSize = blockedWindow->size();
		const auto minimumLayerWidth
			= Info::LayerWidget::MinimalSupportedWidth();
		const auto restoreSizes = gsl::finally([=] {
			pinnedWindow->resize(pinnedPreviousSize);
			blockedWindow->resize(blockedPreviousSize);
			QCoreApplication::processEvents();
		});
		for (const auto &window : {pinnedWindow, blockedWindow}) {
			window->resize(minimumLayerWidth - 1, window->height());
		}
		QCoreApplication::processEvents();
		if (pinnedWindow->width() >= minimumLayerWidth
			|| blockedWindow->width() >= minimumLayerWidth) {
			std::fprintf(
				stderr,
				"Settings deep link regression failed: could not constrain "
				"unsupported test windows below Info layer width: "
				"pinned_width=%d blocked_width=%d minimum=%d.\n",
				pinnedWindow->width(), blockedWindow->width(),
				minimumLayerWidth);
			return false;
		}
		const auto ordinaryToastBefore
			= pinnedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		if (!app.openLocalUrl(u"tg://settings"_q,
							  contextFor(pinnedController))) {
			return false;
		}
		const auto
			ordinarySettingsStateRestored = WaitForSettingsDeepLinkCondition([&] {
				return pinnedController->content()->isMainSectionShown()
					   && !pinnedController->isLayerShown()
					   && (pinnedController
							   ->featureUnavailableOnServerToastCallsForRegressionTest()
						   == ordinaryToastBefore)
					   && (app.activeWindow() == &pinnedController->window());
			});
		const auto ordinarySectionShown
			= pinnedController->content()->isMainSectionShown();
		const auto ordinaryLayerHidden = !pinnedController->isLayerShown();
		const auto ordinaryToastUnchanged
			= (pinnedController
				   ->featureUnavailableOnServerToastCallsForRegressionTest()
			   == ordinaryToastBefore);
		const auto ordinaryWindowActive
			= (app.activeWindow() == &pinnedController->window());
		if (!ordinarySettingsStateRestored || !ordinarySectionShown
			|| !ordinaryLayerHidden || !ordinaryToastUnchanged
			|| !ordinaryWindowActive) {
			std::fprintf(
				stderr,
				"Settings deep link ordinary Settings counts: section=%d "
				"layer-hidden=%d toast-unchanged=%d active-window=%d "
				"expected=1/1/1/1.\n",
				ordinarySectionShown, ordinaryLayerHidden,
				ordinaryToastUnchanged, ordinaryWindowActive);
			std::fprintf(
				stderr,
				"Settings deep link regression failed: ordinary Settings link "
				"lost its section or focus behavior.\n");
			return false;
		}
		pinnedController->clearSectionStack(Window::SectionShow(
			anim::type::instant, anim::activation::background));
		QCoreApplication::processEvents();

		for (const auto &controller : {pinnedController, blockedController}) {
			const auto toastBefore
				= controller
					  ->featureUnavailableOnServerToastCallsForRegressionTest();
			const auto storiesSupported
				= controller->session().storiesSupported();
			const auto routeCountsBefore
				= GetSettingsDeepLinkRouteCountsForRegressionTest(
					controller.get());
			if (!app.openLocalUrl(u"tg://settings/my-profile"_q,
								  contextFor(controller))) {
				return false;
			}
			const auto routeCountsAfter
				= GetSettingsDeepLinkRouteCountsForRegressionTest(
					controller.get());
			const auto myProfileRouteCount = routeCountsAfter.myProfileRoute
											 - routeCountsBefore.myProfileRoute;
			const auto informationFallbackCount
				= routeCountsAfter.informationFallback
				  - routeCountsBefore.informationFallback;
			const auto myProfileRouteRequested = (myProfileRouteCount == 1);
			const auto informationFallbackRequested
				= (informationFallbackCount == 1);
			const auto myAccountShown = [&](const auto &counts) {
				return (counts.mainSettings == 1)
					   && (counts.mainInformation == 1)
					   && (counts.mainSectionVisible == 1)
					   && (counts.mainSectionAnimating == 0)
					   && ShownInfoSectionMatches(
						   controller, [](const Info::Section &section) {
							   return (section.type()
									   == Info::Section::Type::Settings)
									  && (section.settingsType()
										  == Settings::InformationId());
						   });
			};
			const auto
				myProfileStateRestored = WaitForSettingsDeepLinkCondition([&] {
					return !storiesSupported
						   && controller->content()->isMainSectionShown()
						   && myAccountShown(
							   controller->content()
								   ->sectionStackCountsForRegressionTest())
						   && !ShownStoriesSection(controller)
						   && !controller->isLayerShown()
						   && (controller
								   ->featureUnavailableOnServerToastCallsForRegressionTest()
							   == toastBefore);
				});
			const auto stackCountsAtDeadline
				= controller->content()->sectionStackCountsForRegressionTest();
			const auto mainSectionShown
				= controller->content()->isMainSectionShown();
			const auto myAccountIsShown = myAccountShown(stackCountsAtDeadline);
			const auto storiesShown = ShownStoriesSection(controller);
			const auto layerHidden = !controller->isLayerShown();
			const auto toastUnchanged
				= (controller
					   ->featureUnavailableOnServerToastCallsForRegressionTest()
				   == toastBefore);
			if (storiesSupported || !myProfileStateRestored
				|| !myProfileRouteRequested || !informationFallbackRequested
				|| !mainSectionShown || !myAccountIsShown || storiesShown
				|| !layerHidden || !toastUnchanged) {
				const auto targetInfoCounts
					= CountSettingsDeepLinkInfoSections(controller);
				const auto stockInfoCounts
					= CountSettingsDeepLinkInfoSections(stockController);
				const auto targetWindowVisible
					= controller->widget()->isVisible();
				const auto targetWindowActive
					= (app.activeWindow() == &controller->window());
				const auto restoreConditionsMissing
					= int(!mainSectionShown) + int(!myAccountIsShown)
					  + int(storiesShown) + int(!layerHidden)
					  + int(!toastUnchanged);
				std::fprintf(
					stderr,
					"Settings deep link route and stack counts: "
					"my-profile-route=%d information-fallback=%d "
					"navigation-depth=%d navigation-top-history=%d "
					"navigation-top-section=%d navigation-top-info=%d "
					"navigation-top-settings=%d "
					"navigation-top-information=%d navigation-top-stories=%d "
					"main-history=%d main-other=%d main-info=%d "
					"main-section-visible=%d main-section-animating=%d "
					"main-settings=%d main-information=%d main-stories=%d "
					"restore-conditions-missing=%d.\n",
					myProfileRouteCount, informationFallbackCount,
					stackCountsAtDeadline.navigationDepth,
					stackCountsAtDeadline.navigationTopHistory,
					stackCountsAtDeadline.navigationTopSection,
					stackCountsAtDeadline.navigationTopInfo,
					stackCountsAtDeadline.navigationTopSettings,
					stackCountsAtDeadline.navigationTopInformation,
					stackCountsAtDeadline.navigationTopStories,
					stackCountsAtDeadline.mainHistory,
					stackCountsAtDeadline.mainOther,
					stackCountsAtDeadline.mainInfo,
					stackCountsAtDeadline.mainSectionVisible,
					stackCountsAtDeadline.mainSectionAnimating,
					stackCountsAtDeadline.mainSettings,
					stackCountsAtDeadline.mainInformation,
					stackCountsAtDeadline.mainStories,
					restoreConditionsMissing);
				std::fprintf(
					stderr,
					"Settings deep link my-profile observer counts: "
					"target-window-visible=%d target-window-active=%d "
					"target-wraps=%d target-visible-wraps=%d "
					"target-not-hidden-wraps=%d target-settings-wraps=%d "
					"target-information-wraps=%d "
					"target-information-visible=%d "
					"target-information-not-hidden=%d "
					"target-stories-wraps=%d stock-wraps=%d "
					"stock-information-wraps=%d stock-information-visible=%d "
					"stock-information-not-hidden=%d.\n",
					targetWindowVisible, targetWindowActive,
					targetInfoCounts.wraps, targetInfoCounts.visibleWraps,
					targetInfoCounts.notHiddenWraps,
					targetInfoCounts.settingsWraps,
					targetInfoCounts.informationWraps,
					targetInfoCounts.visibleInformationWraps,
					targetInfoCounts.notHiddenInformationWraps,
					targetInfoCounts.storiesWraps, stockInfoCounts.wraps,
					stockInfoCounts.informationWraps,
					stockInfoCounts.visibleInformationWraps,
					stockInfoCounts.notHiddenInformationWraps);
				std::fprintf(stderr,
							 "Settings deep link my-profile counts: "
							 "stories-supported=%d "
							 "state-restored=%d main-section=%d my-account=%d "
							 "stories=%d "
							 "layer-hidden=%d toast-unchanged=%d "
							 "expected=0/1/1/1/0/1/1.\n",
							 storiesSupported, myProfileStateRestored,
							 mainSectionShown, myAccountIsShown, storiesShown,
							 layerHidden, toastUnchanged);
				std::fprintf(
					stderr,
					"Settings deep link regression failed: my-profile did not "
					"open My Account on an unsupported session.\n");
				return false;
			}
			resetController(controller);
			QCoreApplication::processEvents();
		}
	}

	const auto stockWindowActivated
		= ActivateWindowForRegression(app, &stockController->window(), true);
	if (!stockWindowActivated) {
		const auto activeWindowRestored
			= (app.activeWindow() == &stockController->window());
		const auto activePrimaryWindowRestored
			= (app.activePrimaryWindow() == &stockController->window());
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "active-primary-window=%d expected=1/1.\n",
					 activeWindowRestored, activePrimaryWindowRestored);
		std::fprintf(
			stderr,
			"Settings deep link regression failed: stock window did not "
			"become active before missing-target checks.\n");
		return false;
	}
	resetController(stockController);
	QCoreApplication::processEvents();
	const auto missingTargetContext
		= QVariant::fromValue(ClickHandlerContext{});
	for (const auto &url : routes) {
		const auto stockToastBeforeMissingController
			= stockController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto pinnedToastBeforeMissingController
			= pinnedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto blockedToastBeforeMissingController
			= blockedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		const auto activeBeforeMissingController = app.activeWindow();
		const auto handled = app.openLocalUrl(url, missingTargetContext);
		QCoreApplication::processEvents();
		const auto unchanged
			= handled
			  && (stockController
					  ->featureUnavailableOnServerToastCallsForRegressionTest()
				  == stockToastBeforeMissingController)
			  && (pinnedController
					  ->featureUnavailableOnServerToastCallsForRegressionTest()
				  == pinnedToastBeforeMissingController)
			  && (blockedController
					  ->featureUnavailableOnServerToastCallsForRegressionTest()
				  == blockedToastBeforeMissingController)
			  && !stockController->content()->isMainSectionShown()
			  && !pinnedController->content()->isMainSectionShown()
			  && !blockedController->content()->isMainSectionShown()
			  && !stockController->isLayerShown()
			  && !pinnedController->isLayerShown()
			  && !blockedController->isLayerShown()
			  && (app.activeWindow() == activeBeforeMissingController);
		if (!unchanged) {
			std::fprintf(
				stderr,
				"Settings deep link regression failed: missing target was "
				"not a no-op for route=%s restored-blocked=%d.\n",
				url.toUtf8().constData(), restoredBlockedWithoutPin);
			return false;
		}
	}

	pinnedController->window().activate();
	QCoreApplication::processEvents();
	const auto stockWindow = stockController->widget();
	const auto stockPreviousSize = stockWindow->size();
	const auto minimumLayerWidth = Info::LayerWidget::MinimalSupportedWidth();
	const auto restoreStockSize = gsl::finally([=] {
		stockWindow->resize(stockPreviousSize);
		QCoreApplication::processEvents();
	});
	stockWindow->resize(minimumLayerWidth - 1, stockPreviousSize.height());
	QCoreApplication::processEvents();
	if (stockWindow->width() >= minimumLayerWidth) {
		std::fprintf(
			stderr,
			"Settings deep link regression failed: could not constrain stock "
			"test window below Info layer width: width=%d minimum=%d.\n",
			stockWindow->width(), minimumLayerWidth);
		return false;
	}
	const auto stockToastBefore
		= stockController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	if (!app.openLocalUrl(u"tg://settings/premium"_q,
						  contextFor(stockController))) {
		std::fprintf(
			stderr, "Settings deep link regression failed: stock Premium route "
					"was not handled.\n");
		return false;
	}
	QCoreApplication::processEvents();
	const auto stockOpensPremium
		= stockController->content()->isMainSectionShown()
		  && !stockController->isLayerShown()
		  && (stockController
				  ->featureUnavailableOnServerToastCallsForRegressionTest()
			  == stockToastBefore)
		  && (app.activeWindow() == &stockController->window());
	if (!stockOpensPremium) {
		std::fprintf(
			stderr,
			"Settings deep link regression failed: stock Premium route did "
			"not retain its section and focus behavior.\n");
		return false;
	}
	stockController->clearSectionStack(
		Window::SectionShow(anim::type::instant, anim::activation::background));
	QCoreApplication::processEvents();
	std::fprintf(stderr,
				 "Settings deep link regression passed: paid and story routes "
				 "refused in custom and blocked sessions; my-profile opened My "
				 "Account there; stock Premium and ordinary Settings routes "
				 "opened.\n");
	return true;
}

[[nodiscard]] HistoryItem *
AddPaidInvoiceRegressionMessage(not_null<Main::Session *> session, MsgId id,
								MsgId receiptMsgId) {
	using MediaFlag = MTPDmessageMediaInvoice::Flag;
	const auto media = MTP_messageMediaInvoice(
		MTP_flags(receiptMsgId ? MediaFlag::f_receipt_msg_id : MediaFlag()),
		MTP_string(u"Regression invoice"_q), MTP_string(u"Paid item"_q),
		MTPWebDocument(), MTP_int(int(receiptMsgId.bare)), MTP_string(u"USD"_q),
		MTP_long(100), MTP_string(u"regression"_q), MTPMessageExtendedMedia());
	const auto data = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_media),
		MTP_int(int(id.bare)), peerToMTP(session->userPeerId()),
		MTPint(),	 // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(session->userPeerId()),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTP_long(0), // via_bot_id
		MTPlong(),	 // via_business_bot_id
		MTPPeer(),	 // guestchat_via_from
		MTPMessageReplyHeader(),
		MTP_int(1), // date
		MTP_string(QString()), media, MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0), // views
		MTP_int(0), // forwards
		MTPMessageReplies(),
		MTP_int(0), // edit_date
		MTPstring(), MTPlong(), MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTP_int(0), // ttl_period
		MTPint(),	// quick_reply_shortcut_id
		MTPlong(),	// effect
		MTPFactCheck(),
		MTPint(),  // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(),	 // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
	const auto item = session->data().addNewMessage(data, MessageFlags(),
													NewMessageType::Existing);
	const auto invoice
		= (item && item->media()) ? item->media()->invoice() : nullptr;
	return (invoice && (invoice->receiptMsgId == receiptMsgId)) ? item
																: nullptr;
}

[[nodiscard]] bool RunPaidLinkRefusalRegression(
	Core::Application &app,
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> pinnedController,
	not_null<Window::SessionController *> blockedController) {
	const auto configured
		= stockController->session().paidFeaturesSupported()
		  && !pinnedController->session().paidFeaturesSupported()
		  && !blockedController->session().paidFeaturesSupported()
		  && pinnedController->session().mtp().dcOptions().hasCustomServer()
		  && blockedController->session().mtp().dcOptions().blocked();
	if (!configured) {
		std::fprintf(
			stderr,
			"Paid link regression failed: session fixtures did not match "
			"stock, custom, and blocked accounts.\n");
		return false;
	}
	const auto controllers = std::vector<not_null<Window::SessionController *>>{
		stockController,
		pinnedController,
		blockedController,
	};
	const auto resetControllers = [&] {
		for (const auto &controller : controllers) {
			if (controller->isLayerShown()) {
				controller->hideLayer(anim::type::instant);
			}
			controller->clearSectionStack(Window::SectionShow(
				anim::type::instant, anim::activation::background));
		}
		QCoreApplication::processEvents();
	};
	const auto previousWindow = app.activeWindow();
	const auto restoreWindows = gsl::finally([&] {
		resetControllers();
		if (previousWindow) {
			previousWindow->activate();
		}
	});
	resetControllers();

	const auto local = std::vector<QString>{
		u"tg://invoice?slug=regression"_q,
		u"tg://premium_offer"_q,
		u"tg://premium_offer?ref=regression"_q,
		u"tg://premium_multigift?ref=regression"_q,
		u"tg://stars_topup?balance=10&purpose=regression"_q,
		u"tg://nft?slug=regression-1"_q,
		u"tg://stargift_auction?slug=regression"_q,
		u"tg://stars"_q,
		u"tg://ton"_q,
		u"tg://grams"_q,
		Core::TryConvertUrlToLocal(u"https://t.me/$regression"_q),
		Core::TryConvertUrlToLocal(u"https://t.me/invoice/regression"_q),
		Core::TryConvertUrlToLocal(u"https://t.me/nft/regression-1"_q),
		Core::TryConvertUrlToLocal(u"https://t.me/auction/regression"_q),
	};
	const auto internal = std::vector<QString>{
		u"internal:edit_paid_messages_fee/"_q
			+ QString::number(pinnedController->session().userPeerId().value),
		u"internal:stars_examples"_q,
	};
	for (const auto &url : local) {
		if (!url.startsWith(u"tg://"_q)) {
			std::fprintf(
				stderr,
				"Paid link regression failed: t.me link was not converted to "
				"a local link: %s.\n",
				url.toUtf8().constData());
			return false;
		}
	}
	const auto toastCounts = [&] {
		auto result = std::vector<int>();
		for (const auto &controller : controllers) {
			result.push_back(
				controller
					->featureUnavailableOnServerToastCallsForRegressionTest());
		}
		return result;
	};
	const auto paidUiOpen = [&] {
		return std::any_of(
			controllers.begin(), controllers.end(), [](const auto &controller) {
				return controller->content()->isMainSectionShown()
					   || controller->isLayerShown();
			});
	};
	const auto open = [&](const QString &url, const QVariant &context) {
		return url.startsWith(u"internal:"_q)
				   ? app.openInternalUrl(url, context)
				   : app.openLocalUrl(url, context);
	};
	const auto dispatchRefused = [&](const QString &url,
									 Window::SessionController *target) {
		const auto activeBefore = app.activeWindow();
		auto expected = toastCounts();
		for (auto i = 0; i != int(controllers.size()); ++i) {
			expected[i] += (controllers[i].get() == target) ? 1 : 0;
		}
		const auto context = target
								 ? QVariant::fromValue(ClickHandlerContext{
									   .sessionWindow = base::make_weak(target),
								   })
								 : QVariant::fromValue(ClickHandlerContext{});
		const auto handled = open(url, context);
		QCoreApplication::processEvents();
		const auto refused = handled && (toastCounts() == expected)
							 && !paidUiOpen()
							 && (app.activeWindow() == activeBefore);
		if (!refused) {
			std::fprintf(
				stderr,
				"Paid link regression failed: link=%s target=%p handled=%d "
				"toast-only=%d paid-ui-open=%d.\n",
				url.toUtf8().constData(), static_cast<void *>(target), handled,
				toastCounts() == expected, paidUiOpen());
		}
		resetControllers();
		return refused;
	};
	const auto stockWindow = &stockController->window();
	stockWindow->activate();
	QCoreApplication::processEvents();
	if ((app.activeWindow() != stockWindow) && !WaitForRegressionCondition([&] {
			return app.activeWindow() == stockWindow;
		})) {
		std::fprintf(
			stderr,
			"Paid link regression failed: stock window did not activate.\n");
		return false;
	}
	const auto dispatchMatrix = [&](Window::SessionController *target) {
		for (const auto &url : local) {
			if (!dispatchRefused(url, target)) {
				return false;
			}
		}
		for (const auto &url : internal) {
			if (!dispatchRefused(url, target)) {
				return false;
			}
		}
		return true;
	};
	if (!dispatchMatrix(pinnedController) || !dispatchMatrix(blockedController)
		|| !dispatchMatrix(nullptr)) {
		return false;
	}

	const auto checkoutRefused = [&](not_null<Window::SessionController *>
										 target) {
		const auto session = &target->session();
		const auto user = session->user();
		const auto results
			= std::make_shared<std::vector<Payments::CheckoutResult>>();
		const auto reactivate = [=](Payments::CheckoutResult result) {
			results->push_back(result);
		};
		const auto invoiceItem
			= AddPaidInvoiceRegressionMessage(session, MsgId(0x7e5701), 0);
		const auto receiptItem = AddPaidInvoiceRegressionMessage(
			session, MsgId(0x7e5702), MsgId(0x7e5703));
		if (!invoiceItem || !receiptItem) {
			std::fprintf(
				stderr,
				"Paid link regression failed: invoice message fixtures were "
				"not created in session %p.\n",
				static_cast<void *>(target.get()));
			return false;
		}
		auto payments = PaymentRequestCapture{
			.instance = &session->mtp(),
		};
		const auto restorePaymentCapture = gsl::finally(
			[previous = std::exchange(gPaymentRequestCapture, &payments)] {
				gPaymentRequestCapture = previous;
			});
		session->api()
			.request(MTPpayments_GetPaymentReceipt(MTP_inputPeerSelf(),
												   MTP_int(0x7e5703)))
			.send();
		const auto captureObserved
			= (payments.constructors
			   == std::vector<uint32>{mtpc_payments_getPaymentReceipt});
		payments.constructors.clear();
		const auto before = toastCounts();
		Payments::CheckoutProcess::Start(invoiceItem, Payments::Mode::Payment,
										 reactivate, nullptr);
		Payments::CheckoutProcess::Start(receiptItem, Payments::Mode::Receipt,
										 reactivate, nullptr);
		Payments::CheckoutProcess::Start(session, u"regression"_q, reactivate,
										 nullptr);
		Payments::CheckoutProcess::Start(
			Payments::InvoiceCredits{
				.session = session,
				.randomId = 0x7e57'0001ULL,
				.credits = 10,
				.currency = u"XTR"_q,
				.amount = 10,
			},
			reactivate);
		Payments::CheckoutProcess::Start(
			Payments::InvoiceStarGift{
				.giftId = 1,
				.randomId = 0x7e57'0002ULL,
				.recipient = user,
			},
			reactivate, nullptr);
		Payments::CheckoutProcess::Start(
			Payments::InvoicePremiumGiftCode{
				.purpose = Payments::InvoicePremiumGiftCodeUsers{
					.users = {user},
				},
				.currency = u"XTR"_q,
				.randomId = 0x7e57'0003ULL,
				.amount = 10,
				.storeQuantity = 1,
				.users = 1,
				.months = 3,
			},
			reactivate, nullptr);
		const auto starsFormDone
			= [=](Payments::CheckoutResult result, const MTPUpdates *) {
				  results->push_back(result);
			  };
		const auto savedGift = MTP_inputSavedStarGiftUser(MTP_int(0x7e5704));
		Ui::RequestStarsFormAndSubmit(
			target->uiShow(),
			MTP_inputInvoiceStarGiftUpgrade(
				MTP_flags(MTPDinputInvoiceStarGiftUpgrade::Flag()), savedGift),
			starsFormDone);
		Ui::RequestStarsFormAndSubmit(
			target->uiShow(),
			MTP_inputInvoiceStarGiftTransfer(savedGift, MTP_inputPeerSelf()),
			starsFormDone);
		const auto directInvoice
			= MTP_inputInvoiceStarGiftTransfer(savedGift, MTP_inputPeerSelf());
		Ui::SubmitStarsForm(target->uiShow(), directInvoice, 1, 1,
							starsFormDone);
		Ui::SubmitTonForm(target->uiShow(), directInvoice, 1,
						  CreditsAmount(1, 0, CreditsType::Ton), starsFormDone);
		const auto activeChatBefore = target->activeChatCurrent();
		const auto giftDocument
			= session->data().document(DocumentId(0x7e5707));
		const auto transferUnique
			= std::make_shared<Data::UniqueGift>(Data::UniqueGift{
				.slug = u"regression-1"_q,
				.model = Data::UniqueGiftModel{{}, giftDocument},
				.pattern = Data::UniqueGiftPattern{{}, giftDocument},
			});
		Ui::HandleDefaultGiftForRegressionTest(
			target, session->data().user(UserId(0x7e5705)),
			Info::PeerGifts::GiftTypeStars{
				.transferId = Data::SavedStarGiftId::User(MsgId(0x7e5704)),
				.info = Data::StarGift{
					.id = 0x7e5706,
					.unique = transferUnique,
					.document = giftDocument,
				},
				.mine = true,
			});
		QCoreApplication::processEvents();
		const auto callbacksSettled
			= WaitForRegressionCondition([&] { return results->size() >= 10; });
		auto expected = before;
		for (auto i = 0; i != int(controllers.size()); ++i) {
			expected[i] += (controllers[i] == target) ? 11 : 0;
		}
		const auto refused
			= callbacksSettled
			  && (*results
				  == std::vector<Payments::CheckoutResult>(
					  10, Payments::CheckoutResult::Cancelled))
			  && captureObserved && payments.constructors.empty()
			  && (target->activeChatCurrent() == activeChatBefore)
			  && (toastCounts() == expected) && !paidUiOpen();
		if (!refused) {
			std::fprintf(
				stderr,
				"Paid link regression failed: checkout start was not refused "
				"in session %p: results=%d toast-only=%d capture=%d "
				"payment-requests=%d.\n",
				static_cast<void *>(target.get()), int(results->size()),
				toastCounts() == expected, captureObserved,
				int(payments.constructors.size()));
		}
		resetControllers();
		return refused;
	};
	if (!checkoutRefused(pinnedController)
		|| !checkoutRefused(blockedController)) {
		return false;
	}

	const auto stockSettingsOpened = [&](Settings::Type type) {
		const auto main
			= stockController->content()->sectionStackCountsForRegressionTest();
		const auto mainSettingsShown
			= !stockController->isLayerShown()
			  && stockController->content()->isMainSectionShown()
			  && ((type == Settings::CreditsId()) ? main.mainCredits
												  : main.mainCurrency);
		const auto layer = dynamic_cast<Info::WrapWidget *>(
			stockController->activeLayerSection());
		const auto layerSettingsShown
			= stockController->isLayerShown() && layer
			  && (layer->controller()->section().type()
				  == Info::Section::Type::Settings)
			  && (layer->controller()->section().settingsType() == type);
		return mainSettingsShown || layerSettingsShown;
	};
	const auto stockOpens = [&](const QString &url, Settings::Type setting) {
		pinnedController->window().activate();
		QCoreApplication::processEvents();
		const auto before = toastCounts();
		const auto handled = open(
			url, QVariant::fromValue(ClickHandlerContext{
					 .sessionWindow = base::make_weak(stockController.get()),
				 }));
		const auto stockUiOpened
			= handled && WaitForRegressionCondition([&] {
				  return setting ? stockSettingsOpened(setting)
								 : stockController->isLayerShown();
			  });
		const auto toastUnchanged = (toastCounts() == before);
		const auto pinnedUiUnchanged
			= !pinnedController->content()->isMainSectionShown()
			  && !pinnedController->isLayerShown();
		const auto stockSectionShown
			= stockController->content()->isMainSectionShown();
		const auto stockLayerShown = stockController->isLayerShown();
		const auto expectedSettingsShown
			= setting && stockSettingsOpened(setting);
		const auto opened
			= stockUiOpened && toastUnchanged && pinnedUiUnchanged;
		if (!opened) {
			std::fprintf(
				stderr,
				"Paid link regression failed: stock link=%s did not keep its "
				"behavior: handled=%d stock-ui-opened=%d stock-section=%d "
				"stock-layer=%d expected-settings=%d toast-unchanged=%d "
				"pinned-ui-unchanged=%d.\n",
				url.toUtf8().constData(), handled, stockUiOpened,
				stockSectionShown, stockLayerShown, expectedSettingsShown,
				toastUnchanged, pinnedUiUnchanged);
		}
		resetControllers();
		return opened;
	};
	if (!stockOpens(u"tg://stars"_q, Settings::CreditsId())
		|| !stockOpens(u"tg://ton"_q, Settings::CurrencyId())
		|| !stockOpens(u"internal:stars_examples"_q, nullptr)) {
		return false;
	}
	std::fprintf(
		stderr,
		"Paid link regression passed: %d paid links refused in custom and "
		"blocked sessions and ignored without a target; message invoice, "
		"receipt, slug, Stars, gift, gift-code, gift upgrade and transfer, "
		"direct Stars and TON submit, and gift-list transfer starts refused "
		"before payment requests; stock links opened.\n",
		int(local.size() + internal.size()));
	return true;
}

[[nodiscard]] bool RunPassportLinkRefusalRegression(
	Core::Application &app, Main::Domain &domain,
	not_null<Main::Account *> stock, not_null<Main::Account *> pinned,
	not_null<Main::Account *> blank, not_null<Window::Controller *> stockWindow,
	not_null<Window::Controller *> pinnedWindow,
	not_null<Window::Controller *> blankWindow) {
	const auto stockController = stockWindow->sessionController();
	const auto pinnedController = pinnedWindow->sessionController();
	const auto blankController = blankWindow->sessionController();
	if (!stockController || !pinnedController || !blankController) {
		std::fprintf(
			stderr,
			"Passport link regression: a session controller is missing.\n");
		return false;
	}
	if (!stock->session().passportSupported()
		|| pinned->session().passportSupported()
		|| blank->session().passportSupported()
		|| pinned->mtp().dcOptions().blocked()
		|| !pinned->mtp().dcOptions().hasCustomServer()
		|| !blank->mtp().dcOptions().blocked()
		|| blank->mtp().dcOptions().hasCustomServer()) {
		std::fprintf(
			stderr,
			"Passport link regression: fixture capabilities do not match "
			"stock, custom, and blocked sessions.\n");
		return false;
	}
	stockController->clearPassportForm();
	pinnedController->clearPassportForm();
	blankController->clearPassportForm();
	const auto activeBefore = &domain.active();
	const auto restore = gsl::finally([&] {
		stockController->clearPassportForm();
		pinnedController->clearPassportForm();
		blankController->clearPassportForm();
		domain.activate(activeBefore);
	});
	const auto orderedAccounts = domain.orderedAccounts();
	const auto indexForAccount = [&](not_null<Main::Account *> account) {
		for (auto i = 0; i != int(orderedAccounts.size()); ++i) {
			if (orderedAccounts[i].get() == account.get()) {
				return i + 1;
			}
		}
		return 0;
	};
	const auto stockIndex = indexForAccount(stock);
	const auto pinnedIndex = indexForAccount(pinned);
	const auto invalidAccountIndex = int(orderedAccounts.size()) + 1;
	if (!stockIndex || !pinnedIndex) {
		std::fprintf(
			stderr,
			"Passport link regression: account index fixture is incomplete.\n");
		return false;
	}
	const auto callback = u"&callback_url=https%3A%2F%2Fexample.com"_q;
	const auto passportUrl = [&](bool hasNonce) {
		return u"tg://passport?bot_id=1&scope=%7B%7D&public_key=x"_q
			   + (hasNonce ? u"&nonce=y"_q : QString()) + callback;
	};
	const auto resolveUrl = [&](bool hasNonce, int accountIndex) {
		return u"tg://resolve?domain=telegrampassport&bot_id=1&scope=%7B%7D"_q
			   + u"&public_key=x"_q + (hasNonce ? u"&nonce=y"_q : QString())
			   + callback
			   + (accountIndex ? u"&acc="_q + QString::number(accountIndex)
							   : QString());
	};
	const auto links = std::vector<QString>{
		passportUrl(true),
		resolveUrl(true, 0),
		passportUrl(false),
		resolveUrl(false, 0),
		resolveUrl(true, stockIndex),
		resolveUrl(false, invalidAccountIndex),
	};
	const auto queuedWithoutController = [&](const QString &url) {
		const auto command = url.mid(u"tg://"_q.size());
		const auto options = qthelp::RegExOption::CaseInsensitive;
		for (const auto &[expression, handler] : Core::LocalUrlHandlers()) {
			const auto match
				= qthelp::regex_match(expression, command, options);
			if (match) {
				return !handler(nullptr, match, {});
			}
		}
		return false;
	};
	for (const auto &url : links) {
		if (!queuedWithoutController(url)) {
			std::fprintf(
				stderr, "Passport link regression: missing controller consumed "
						"or missed a URL.\n");
			return false;
		}
	}
	const auto contextFor = [](Window::SessionController *controller) {
		return QVariant::fromValue(ClickHandlerContext{
			.sessionWindow = controller,
		});
	};
	const auto toastCalls = [](Window::SessionController *controller) {
		return controller
			->featureUnavailableOnServerToastCallsForRegressionTest();
	};
	const auto hasPassportForm = [](Window::SessionController *controller) {
		return controller->passportFormExistsForRegressionTest();
	};
	const auto refuse = [&](const char *label,
							Window::SessionController *controller,
							const QString &url) {
		const auto stockToastBefore = toastCalls(stockController);
		const auto pinnedToastBefore = toastCalls(pinnedController);
		const auto blankToastBefore = toastCalls(blankController);
		const auto handled = app.openLocalUrl(url, contextFor(controller));
		QCoreApplication::processEvents();
		const auto stockToastAfter = toastCalls(stockController);
		const auto pinnedToastAfter = toastCalls(pinnedController);
		const auto blankToastAfter = toastCalls(blankController);
		const auto refused
			= handled && !hasPassportForm(stockController)
			  && !hasPassportForm(pinnedController)
			  && !hasPassportForm(blankController)
			  && (stockToastAfter
				  == stockToastBefore + (controller == stockController))
			  && (pinnedToastAfter
				  == pinnedToastBefore + (controller == pinnedController))
			  && (blankToastAfter
				  == blankToastBefore + (controller == blankController));
		if (!refused) {
			std::fprintf(
				stderr,
				"Passport link regression: %s refusal mismatch for %s.\n",
				label, url.toUtf8().constData());
		}
		return refused;
	};
	domain.activate(stock);
	if (&domain.active() != stock.get()) {
		std::fprintf(
			stderr,
			"Passport link regression: stock account was not active.\n");
		return false;
	}
	for (const auto &url : links) {
		if (!refuse("custom", pinnedController, url)) {
			return false;
		}
	}
	domain.activate(pinned);
	if (&domain.active() != pinned.get()) {
		std::fprintf(
			stderr,
			"Passport link regression: custom account was not active.\n");
		return false;
	}
	for (const auto &url : std::vector<QString>{
			 passportUrl(true),
			 resolveUrl(true, 0),
			 resolveUrl(true, pinnedIndex),
			 resolveUrl(false, invalidAccountIndex),
		 }) {
		const auto toastBefore = toastCalls(stockController);
		const auto pinnedToastBefore = toastCalls(pinnedController);
		const auto blankToastBefore = toastCalls(blankController);
		const auto handled = app.openLocalUrl(url, contextFor(stockController));
		QCoreApplication::processEvents();
		const auto opened
			= handled && hasPassportForm(stockController)
			  && !hasPassportForm(pinnedController)
			  && !hasPassportForm(blankController)
			  && (toastCalls(stockController) == toastBefore)
			  && (toastCalls(pinnedController) == pinnedToastBefore)
			  && (toastCalls(blankController) == blankToastBefore);
		stockController->clearPassportForm();
		if (!opened) {
			std::fprintf(
				stderr,
				"Passport link regression: stock behavior changed for %s.\n",
				url.toUtf8().constData());
			return false;
		}
	}
	domain.activate(stock);
	if (&domain.active() != stock.get()) {
		std::fprintf(
			stderr,
			"Passport link regression: stock account was not active.\n");
		return false;
	}
	for (const auto &url : links) {
		if (!refuse("blocked-without-pin", blankController, url)) {
			return false;
		}
	}
	const auto restartToastBefore = toastCalls(pinnedController);
	pinnedController->showPassportForm(Passport::FormRequest(
		UserId(1), u"{}"_q, u"https://example.com"_q, u"x"_q, u"y"_q));
	if (hasPassportForm(pinnedController)
		|| (toastCalls(pinnedController) != restartToastBefore + 1)) {
		std::fprintf(
			stderr,
			"Passport link regression: restart bypassed the session gate.\n");
		return false;
	}
	std::fprintf(stderr,
				 "Passport link regression passed: URL variants followed their "
				 "owning stock, custom, and blocked sessions.\n");
	return true;
}

[[nodiscard]] int
AcceptedMiniAppOpens(not_null<Main::Session *> session,
					 not_null<UserData *> bot,
					 not_null<Window::SessionController *> controller) {
	const auto action = Api::SendAction(session->data().history(bot));
	const auto sources = std::vector<InlineBots::WebViewSource>{
		InlineBots::WebViewSource{InlineBots::WebViewSourceButton{}},
		InlineBots::WebViewSource{
			InlineBots::WebViewSourceButton{.simple = true}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceSwitch{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceLinkApp{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceLinkAttachMenu{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceLinkBotProfile{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceMainMenu{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceAttachMenu{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceBotMenu{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceGame{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceBotProfile{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceAgeVerification{}},
		InlineBots::WebViewSource{InlineBots::WebViewSourceJoinChat{}},
	};
	if (static_cast<int>(sources.size()) != kRegressionEntryClassCount) {
		return -1;
	}
	auto accepted = 0;
	for (const auto &source : sources) {
		if (session->attachWebView().open({
			.bot = bot,
			.context = {
				.controller = controller,
				.action = action,
				.maySkipConfirmation = true,
			},
			.source = source,
		})) {
			++accepted;
		}
	}
	return accepted;
}

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
[[nodiscard]] ChannelData *AddBoostRegressionChannel(Main::Session &session,
													 ChannelId id,
													 uint64 accessHash,
													 const QString &name,
													 const QString &username) {
	using Flag = MTPDchannel::Flag;
	const auto chat = MTP_channel(
		MTP_flags(Flag::f_broadcast | Flag::f_access_hash | Flag::f_username),
		MTP_long(id.bare), MTP_long(accessHash), MTP_string(name),
		MTP_string(username), MTP_chatPhotoEmpty(), MTP_int(1),
		MTPVector<MTPRestrictionReason>(), MTPChatAdminRights(),
		MTPChatBannedRights(), MTPChatBannedRights(), MTPint(),
		MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(),
		MTPPeerColor(), MTPEmojiStatus(), MTPint(), MTPint(), MTPlong(),
		MTPlong(), MTPlong(), MTPlong());
	const auto peer = session.data().processChats(MTP_vector<MTPChat>({chat}));
	return peer ? peer->asChannel() : nullptr;
}

[[nodiscard]] bool RunBoostEntryPathRefusalRegression(
	Core::Application &app, not_null<Main::Account *> stock,
	not_null<Main::Account *> pinned, not_null<Main::Account *> blocked,
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> pinnedController,
	not_null<Window::SessionController *> blockedController) {
	const auto activeAtStart = &app.domain().active();
	const auto restoreActive
		= gsl::finally([&] { app.domain().activate(activeAtStart); });
	const auto sameIdentity
		= stock->session().uniqueId() == pinned->session().uniqueId()
		  && stock->session().uniqueId() == blocked->session().uniqueId();
	const auto channelId = ChannelId(1251);
	const auto accessHash = uint64(125100);
	const auto username = u"regression_boost"_q;
	const auto stockChannel
		= AddBoostRegressionChannel(stock->session(), channelId, accessHash,
									u"Stock boost channel"_q, username);
	const auto pinnedChannel
		= AddBoostRegressionChannel(pinned->session(), channelId, accessHash,
									u"Pinned boost channel"_q, username);
	const auto blockedChannel
		= AddBoostRegressionChannel(blocked->session(), channelId, accessHash,
									u"Blocked boost channel"_q, username);
	if (!sameIdentity || !stockChannel || !pinnedChannel || !blockedChannel) {
		std::fprintf(
			stderr, "Boost entry regression failed: session or channel fixture "
					"was incomplete.\n");
		return false;
	}
	const auto pinnedBoostFlowsBefore
		= pinnedController->boostFlowsOpenedForRegressionTest();
	const auto blockedBoostFlowsBefore
		= blockedController->boostFlowsOpenedForRegressionTest();
	auto requests = BoostRequestCaptureForRegressionTest{
		.sessions = {
			{ .instance = &stock->session().mtp() },
			{ .instance = &pinned->session().mtp() },
			{ .instance = &blocked->session().mtp() },
		},
	};
	const auto previousCapture
		= SetBoostRequestCaptureForRegressionTest(&requests);
	const auto restoreCapture = gsl::finally([=] {
		static_cast<void>(
			SetBoostRequestCaptureForRegressionTest(previousCapture));
	});
	const auto countsFor = [&](const MTP::Instance *instance)
		-> const BoostRequestSessionCountsForRegressionTest & {
		const auto i = std::find_if(
			requests.sessions.begin(), requests.sessions.end(),
			[&](const auto &session) { return session.instance == instance; });
		return *i;
	};
	const auto noBoostRequestsFor = [&](const MTP::Instance *instance) {
		const auto &counts = countsFor(instance);
		return !counts.boostsStatus && !counts.myBoosts && !counts.applyBoost;
	};
	const auto openLink = [&](not_null<Window::SessionController *> controller,
							  const QString &link) {
		const auto context = QVariant::fromValue(ClickHandlerContext{
			.sessionWindow = base::make_weak(controller.get()),
		});
		return app.openLocalUrl(Core::TryConvertUrlToLocal(link), context);
	};
	const auto pinnedToastBefore
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	app.domain().activate(stock);
	const auto tmeBoostOpened
		= openLink(pinnedController, u"https://t.me/regression_boost?boost"_q);
	QCoreApplication::processEvents();
	const auto pinnedLinkOpenedFirst
		= tmeBoostOpened
		  && (pinnedController->activeChatCurrent().peer() == pinnedChannel)
		  && (pinnedController
				  ->featureUnavailableOnServerPeerForRegressionTest()
			  == pinnedChannel)
		  && (pinnedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest()
			  == pinnedToastBefore + 1);
	if (!pinnedLinkOpenedFirst || !noBoostRequestsFor(&pinned->session().mtp())
		|| !noBoostRequestsFor(&blocked->session().mtp())
		|| !noBoostRequestsFor(&stock->session().mtp())
		|| pinnedController->boostFlowsOpenedForRegressionTest()
			   != pinnedBoostFlowsBefore) {
		std::fprintf(stderr,
					 "Boost entry regression failed: custom t.me boost link "
					 "did not open the channel before the shared refusal.\n");
		return false;
	}
	const auto pinnedToastAfterLink
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	pinnedController->showPeerHistory(pinned->session().user());
	const auto pinnedHistoryRestored = WaitForRegressionCondition([&] {
		return pinnedController->activeChatCurrent().peer()
			   == pinned->session().user();
	});
	const auto normalPinnedLinkOpened
		= pinnedHistoryRestored
		  && openLink(pinnedController, u"https://t.me/regression_boost"_q);
	const auto normalPinnedChannelOpened
		= normalPinnedLinkOpened && WaitForRegressionCondition([&] {
			  return pinnedController->activeChatCurrent().peer()
					 == pinnedChannel;
		  });
	const auto normalPinnedToastUnchanged
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == pinnedToastAfterLink;
	const auto normalPinnedNoBoostRequests
		= noBoostRequestsFor(&pinned->session().mtp());
	if (!normalPinnedChannelOpened || !normalPinnedToastUnchanged
		|| !normalPinnedNoBoostRequests) {
		std::fprintf(
			stderr,
			"Boost entry regression failed: custom normal channel link "
			"opened=%d "
			"history_restored=%d channel_opened=%d toast_unchanged=%d "
			"requests=%d/%d/%d.\n",
			bool(normalPinnedLinkOpened), bool(pinnedHistoryRestored),
			bool(normalPinnedChannelOpened), bool(normalPinnedToastUnchanged),
			countsFor(&pinned->session().mtp()).boostsStatus,
			countsFor(&pinned->session().mtp()).myBoosts,
			countsFor(&pinned->session().mtp()).applyBoost);
		return false;
	}
	const auto pinnedToastBeforeSlowMode
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	pinnedController->resolveBoostState(pinnedChannel, 2);
	const auto pinnedSlowModeRefused
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == pinnedToastBeforeSlowMode + 1;
	const auto pinnedToastBeforeApply
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	pinnedController->applyBoostForRegressionTest(pinnedChannel);
	const auto pinnedApplyRefused
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == pinnedToastBeforeApply + 1;
	const auto pinnedToastBeforeApplyChecked
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	pinnedController->applyBoostsCheckedForRegressionTest(pinnedChannel);
	const auto pinnedApplyCheckedRefused
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == pinnedToastBeforeApplyChecked + 1;
	if (!pinnedSlowModeRefused || !pinnedApplyRefused
		|| !pinnedApplyCheckedRefused
		|| !noBoostRequestsFor(&pinned->session().mtp())
		|| pinnedController->boostFlowsOpenedForRegressionTest()
			   != pinnedBoostFlowsBefore) {
		std::fprintf(
			stderr, "Boost entry regression failed: custom direct or slow-mode "
					"boost path was not refused atomically.\n");
		return false;
	}

	const auto blockedToastBefore
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	app.domain().activate(stock);
	const auto blockedBoostOpened
		= openLink(blockedController, u"tg://boost?channel=1251"_q);
	QCoreApplication::processEvents();
	const auto blockedLinkOpenedFirst
		= blockedBoostOpened
		  && (blockedController->activeChatCurrent().peer() == blockedChannel)
		  && (blockedController
				  ->featureUnavailableOnServerPeerForRegressionTest()
			  == blockedChannel)
		  && (blockedController
				  ->featureUnavailableOnServerToastCallsForRegressionTest()
			  == blockedToastBefore + 1);
	if (!blockedLinkOpenedFirst
		|| !noBoostRequestsFor(&blocked->session().mtp())
		|| blockedController->boostFlowsOpenedForRegressionTest()
			   != blockedBoostFlowsBefore) {
		std::fprintf(stderr,
					 "Boost entry regression failed: blocked tg boost link "
					 "did not open the channel before the shared refusal.\n");
		return false;
	}
	const auto blockedToastAfterLink
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	blockedController->showPeerHistory(blocked->session().user());
	const auto blockedHistoryRestored = WaitForRegressionCondition([&] {
		return blockedController->activeChatCurrent().peer()
			   == blocked->session().user();
	});
	const auto normalBlockedLinkOpened
		= blockedHistoryRestored
		  && openLink(blockedController, u"https://t.me/regression_boost"_q);
	const auto normalBlockedChannelOpened
		= normalBlockedLinkOpened && WaitForRegressionCondition([&] {
			  return blockedController->activeChatCurrent().peer()
					 == blockedChannel;
		  });
	const auto normalBlockedToastUnchanged
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == blockedToastAfterLink;
	const auto normalBlockedNoBoostRequests
		= noBoostRequestsFor(&blocked->session().mtp());
	if (!normalBlockedChannelOpened || !normalBlockedToastUnchanged
		|| !normalBlockedNoBoostRequests) {
		std::fprintf(
			stderr,
			"Boost entry regression failed: blocked normal channel link "
			"opened=%d "
			"history_restored=%d channel_opened=%d toast_unchanged=%d "
			"requests=%d/%d/%d.\n",
			bool(normalBlockedLinkOpened), bool(blockedHistoryRestored),
			bool(normalBlockedChannelOpened), bool(normalBlockedToastUnchanged),
			countsFor(&blocked->session().mtp()).boostsStatus,
			countsFor(&blocked->session().mtp()).myBoosts,
			countsFor(&blocked->session().mtp()).applyBoost);
		return false;
	}
	const auto blockedToastBeforeSlowMode
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	blockedController->resolveBoostState(blockedChannel, 3);
	const auto blockedSlowModeRefused
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == blockedToastBeforeSlowMode + 1;
	const auto blockedToastBeforeApply
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	blockedController->applyBoostForRegressionTest(blockedChannel);
	const auto blockedApplyRefused
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == blockedToastBeforeApply + 1;
	const auto blockedToastBeforeApplyChecked
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	blockedController->applyBoostsCheckedForRegressionTest(blockedChannel);
	const auto blockedApplyCheckedRefused
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == blockedToastBeforeApplyChecked + 1;
	if (!blockedSlowModeRefused || !blockedApplyRefused
		|| !blockedApplyCheckedRefused
		|| !noBoostRequestsFor(&blocked->session().mtp())
		|| blockedController->boostFlowsOpenedForRegressionTest()
			   != blockedBoostFlowsBefore) {
		std::fprintf(
			stderr,
			"Boost entry regression failed: blocked direct or slow-mode "
			"boost path was not refused atomically.\n");
		return false;
	}

	const auto stockToastBefore
		= stockController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	stockController->showPeerHistory(stock->session().user());
	app.domain().activate(pinned);
	const auto stockBoostOpened
		= openLink(stockController, u"tg://boost?channel=1251"_q);
	const auto stockStatusKept
		= countsFor(&stock->session().mtp()).boostsStatus == 1;
	const auto stockToastKept
		= stockController
			  ->featureUnavailableOnServerToastCallsForRegressionTest()
		  == stockToastBefore;
	const auto stockNavigationKept = stockController->activeChatCurrent().peer()
									 == stock->session().user();
	stockController->applyBoostForRegressionTest(stockChannel);
	const auto stockApplicationKept
		= countsFor(&stock->session().mtp()).myBoosts == 1;
	stockController->applyBoostsCheckedForRegressionTest(stockChannel);
	const auto stockApplyKept
		= countsFor(&stock->session().mtp()).applyBoost == 1;
	if (!stockBoostOpened || !stockStatusKept || !stockToastKept
		|| !stockNavigationKept || !stockApplicationKept || !stockApplyKept
		|| !noBoostRequestsFor(&pinned->session().mtp())
		|| !noBoostRequestsFor(&blocked->session().mtp())) {
		std::fprintf(
			stderr,
			"Boost entry regression failed: stock boost behavior changed "
			"or the owning session was crossed.\n");
		return false;
	}

	const auto pinnedToastAtNoController
		= pinnedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	const auto stockToastAtNoController
		= stockController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	const auto blockedToastAtNoController
		= blockedController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	const auto stockRequestsAtNoController = countsFor(&stock->session().mtp());
	auto noControllerNoOp = false;
	for (const auto &handler : Core::LocalUrlHandlers()) {
		const auto match
			= qthelp::regex_match(handler.expression, u"boost?channel=1251"_q,
								  qthelp::RegExOption::CaseInsensitive);
		if (match) {
			noControllerNoOp = !handler.handler(nullptr, match, QVariant());
			break;
		}
	}
	if (!noControllerNoOp
		|| pinnedController
				   ->featureUnavailableOnServerToastCallsForRegressionTest()
			   != pinnedToastAtNoController
		|| stockController
				   ->featureUnavailableOnServerToastCallsForRegressionTest()
			   != stockToastAtNoController
		|| blockedController
				   ->featureUnavailableOnServerToastCallsForRegressionTest()
			   != blockedToastAtNoController
		|| countsFor(&stock->session().mtp()).boostsStatus
			   != stockRequestsAtNoController.boostsStatus
		|| countsFor(&stock->session().mtp()).myBoosts
			   != stockRequestsAtNoController.myBoosts
		|| countsFor(&stock->session().mtp()).applyBoost
			   != stockRequestsAtNoController.applyBoost
		|| !noBoostRequestsFor(&pinned->session().mtp())
		|| !noBoostRequestsFor(&blocked->session().mtp())) {
		std::fprintf(
			stderr, "Boost entry regression failed: missing controller context "
					"fell back to another window.\n");
		return false;
	}
	std::fprintf(
		stderr,
		"Boost entry regression passed: custom and blocked links opened the "
		"channel before refusal, unsupported sessions sent no boost requests, "
		"and stock behavior stayed on its owning session.\n");
	return true;
}
#endif

struct PeerMenuSnapshot final {
	QStringList actions;
	QStringList accessibleNames;
	QStringList keyboardNames;
};

[[nodiscard]] PeerMenuSnapshot SnapshotMenuActions(Ui::PopupMenu &menu) {
	auto result = PeerMenuSnapshot();
	for (const auto &action : menu.actions()) {
		if (action->isSeparator()) {
			continue;
		}
		const auto actionName = action->text().trimmed();
		result.actions.push_back(actionName);
		if (action->isEnabled()) {
			result.keyboardNames.push_back(actionName);
		}
		const auto item = menu.menu()->itemForAction(action);
		if (!item) {
			continue;
		}
		const auto accessible = QAccessible::queryAccessibleInterface(item);
		if (!accessible) {
			continue;
		}
		const auto accessibleName
			= accessible->text(QAccessible::Name).trimmed();
		if (!accessibleName.isEmpty()) {
			result.accessibleNames.push_back(accessibleName);
		}
	}
	return result;
}

[[nodiscard]] PeerMenuSnapshot
SnapshotPeerMenu(not_null<Window::SessionController *> controller,
				 Dialogs::Key key, Dialogs::EntryState::Section section) {
	auto menu = Ui::PopupMenu(nullptr);
	const auto addAction = Ui::Menu::CreateAddActionCallback(&menu);
	Window::FillDialogsEntryMenu(controller,
								 Dialogs::EntryState{
									 .key = key,
									 .section = section,
								 },
								 addAction);
	return SnapshotMenuActions(menu);
}

[[nodiscard]] PeerMenuSnapshot
SnapshotVideoChatMenu(not_null<Window::SessionController *> controller,
					  Dialogs::Key key) {
	auto menu = Ui::PopupMenu(nullptr);
	Window::FillVideoChatMenu(controller, {.key = key},
							  Ui::Menu::CreateAddActionCallback(&menu));
	return SnapshotMenuActions(menu);
}

[[nodiscard]] bool RunMessagesPrivacyBoxPassRegression(
	not_null<Window::SessionController *> controller, UserId selfId,
	bool supported, bool premium, const char *sessionName) {
	auto &session = controller->session();
	const auto wasPremium = session.premium();
	if (!SetRegressionSessionPremium(&session, selfId, premium)) {
		std::fprintf(stderr,
					 "Messages privacy regression failed: session=%s could "
					 "not set Premium=%d.\n",
					 sessionName, premium);
		return false;
	}
	const auto allowed = premium || session.appConfig().newRequirePremiumFree();
	const auto chargeAvailable = session.appConfig().paidMessagesAvailable();
	auto capture = ChannelSaveRequestCapture{
		.instance = &session.mtp(),
	};
	const auto restoreCapture = gsl::finally(
		[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
			gChannelSaveRequestCapture = previous;
		});
	if (controller->isLayerShown()) {
		controller->hideLayer(anim::type::instant);
	}
	auto box = controller->show(
		Box(EditMessagesPrivacyBox, controller.get(), QString()),
		Ui::LayerOption::KeepOther, anim::type::instant);
	QCoreApplication::processEvents();
	const auto content = box.get();
	const auto layer = content ? content->parentWidget() : nullptr;
	if (!layer) {
		std::fprintf(stderr,
					 "Messages privacy regression failed: session=%s "
					 "premium=%d box did not open.\n",
					 sessionName, premium);
		return false;
	}
	auto radios = QStringList();
	auto controls = QStringList();
	for (const auto widget : content->findChildren<QWidget *>()) {
		if (const auto radio = dynamic_cast<Ui::Radiobutton *>(widget)) {
			radios.push_back(radio->accessibilityName());
		} else if (const auto button
				   = dynamic_cast<Ui::SettingsButton *>(widget)) {
			controls.push_back(button->accessibilityName());
		}
	}
	const auto findButton = [&](const QString &text) {
		for (const auto widget : layer->findChildren<QWidget *>()) {
			const auto button = dynamic_cast<Ui::RoundButton *>(widget);
			if (button
				&& !button->accessibilityName().compare(text,
														Qt::CaseInsensitive)) {
				return button;
			}
		}
		return static_cast<Ui::RoundButton *>(nullptr);
	};
	const auto hasEverybody
		= radios.contains(tr::lng_messages_privacy_everyone(tr::now));
	const auto hasRestricted
		= radios.contains(tr::lng_messages_privacy_restricted(tr::now));
	const auto hasCharge
		= radios.contains(tr::lng_messages_privacy_charge(tr::now));
	const auto hasSubscribe
		= controls.contains(tr::lng_messages_privacy_premium_button(tr::now));
	const auto save = findButton(tr::lng_settings_save(tr::now));
	const auto done = findButton(tr::lng_about_done(tr::now));
	const auto expectSave = supported && allowed;
	const auto action = expectSave ? save : done;
	const auto optionsMatch
		= supported
			  ? (hasEverybody && hasRestricted && (hasCharge == chargeAvailable)
				 && (hasSubscribe == !allowed))
			  : ((radios.size() == 1) && hasEverybody && !hasRestricted
				 && !hasCharge && !hasSubscribe);
	const auto buttonsMatch = (!save == !expectSave) && (!done == expectSave);
	if (!optionsMatch || !buttonsMatch || !action) {
		std::fprintf(
			stderr,
			"Messages privacy regression failed: session=%s premium=%d "
			"radios=%d everybody=%d restricted=%d charge=%d subscribe=%d "
			"save=%d done=%d expected-paid=%d.\n",
			sessionName, premium, int(radios.size()), hasEverybody,
			hasRestricted, hasCharge, hasSubscribe, save != nullptr,
			done != nullptr, supported);
		return false;
	}
	action->clicked(Qt::KeyboardModifiers(), Qt::LeftButton);
	const auto closed = WaitForRegressionCondition([&] { return !box; });
	const auto privacyRequests = std::count_if(
		capture.requests.begin(), capture.requests.end(),
		[](const CapturedChannelSaveRequest &request) {
			return !request.body.isEmpty()
				   && (static_cast<uint32>(request.body.front())
					   == mtpc_account_setGlobalPrivacySettings);
		});
	const auto premiumRestored
		= SetRegressionSessionPremium(&session, selfId, wasPremium);
	if (!closed || (privacyRequests != (expectSave ? 1 : 0))
		|| !premiumRestored) {
		std::fprintf(
			stderr,
			"Messages privacy regression failed: session=%s premium=%d "
			"closed=%d setGlobalPrivacySettings=%d expected=%d "
			"premium-restored=%d.\n",
			sessionName, premium, closed, int(privacyRequests),
			expectSave ? 1 : 0, premiumRestored);
		return false;
	}
	return true;
}

[[nodiscard]] bool RunMessagesPrivacyBoxRegression(
	not_null<Window::SessionController *> controller, UserId selfId,
	bool supported, const char *sessionName) {
	if (controller->session().paidFeaturesSupported() != supported) {
		std::fprintf(stderr,
					 "Messages privacy regression failed: session=%s "
					 "capability=%d expected=%d.\n",
					 sessionName, controller->session().paidFeaturesSupported(),
					 supported);
		return false;
	}
	return RunMessagesPrivacyBoxPassRegression(controller, selfId, supported,
											   true, sessionName)
		   && RunMessagesPrivacyBoxPassRegression(controller, selfId, supported,
												  false, sessionName);
}

[[nodiscard]] bool RunBoostMenuVisibilityRegression(
	not_null<Window::SessionController *> controller, bool supported,
	const char *sessionName) {
	auto &session = controller->session();
	if (session.paidFeaturesSupported() != supported) {
		std::fprintf(stderr,
					 "Boost menu regression failed: session=%s capability=%d "
					 "expected=%d.\n",
					 sessionName, session.paidFeaturesSupported(), supported);
		return false;
	}
	const auto configureChannel
		= [](not_null<ChannelData *> channel, const QString &name,
			 ChannelDataFlags flags, uint64 accessHash) {
			  channel->setAccessHash(accessHash);
			  channel->setName(name, QString());
			  channel->setFlags(flags);
		  };
	using Flag = ChannelDataFlag;
	const auto broadcast = session.data().channel(ChannelId(1171));
	configureChannel(broadcast, u"Boost menu regression channel"_q,
					 Flag::Creator | Flag::Broadcast | Flag::CanGetStatistics
						 | Flag::CanViewRevenue,
					 117100);
	const auto group = session.data().channel(ChannelId(1172));
	configureChannel(group, u"Boost menu regression group"_q,
					 Flag::Creator | Flag::Forum | Flag::CanGetStatistics
						 | Flag::CanViewRevenue,
					 117200);
	const auto broadcastHistory = session.data().history(broadcast);
	const auto groupHistory = session.data().history(group);
	const auto forum = group->forum();
	const auto topic = forum
						   ? forum->applyTopicAdded(
								 MsgId(1173), u"Boost menu regression topic"_q,
								 0, DocumentId(), session.userPeerId(), 1, true)
						   : nullptr;
	if (!broadcastHistory || !groupHistory || !topic) {
		std::fprintf(stderr,
					 "Boost menu regression failed: session=%s could not "
					 "create menu peers.\n",
					 sessionName);
		return false;
	}
	const auto contains = [](const QStringList &names,
							 const QString &expected) {
		return std::find(names.begin(), names.end(), expected) != names.end();
	};
	const auto checkEntry = [&](const PeerMenuSnapshot &menu,
								const QString &expected, bool shouldBePresent,
								const char *surface) {
		const auto actionPresent = contains(menu.actions, expected);
		const auto accessiblePresent = contains(menu.accessibleNames, expected);
		const auto keyboardPresent = contains(menu.keyboardNames, expected);
		if ((actionPresent != shouldBePresent)
			|| (accessiblePresent != shouldBePresent)
			|| (keyboardPresent != shouldBePresent)) {
			std::fprintf(stderr,
						 "Boost menu regression failed: session=%s surface=%s "
						 "expected=%d "
						 "action=%d accessibility=%d keyboard=%d label=%s.\n",
						 sessionName, surface, shouldBePresent, actionPresent,
						 accessiblePresent, keyboardPresent,
						 qPrintable(expected));
			return false;
		}
		return true;
	};
	const auto channelChat
		= SnapshotPeerMenu(controller, Dialogs::Key(broadcastHistory),
						   Dialogs::EntryState::Section::History);
	const auto groupChat
		= SnapshotPeerMenu(controller, Dialogs::Key(groupHistory),
						   Dialogs::EntryState::Section::History);
	const auto groupChatsList
		= SnapshotPeerMenu(controller, Dialogs::Key(groupHistory),
						   Dialogs::EntryState::Section::ChatsList);
	const auto channelProfile
		= SnapshotPeerMenu(controller, Dialogs::Key(broadcastHistory),
						   Dialogs::EntryState::Section::Profile);
	const auto groupProfile
		= SnapshotPeerMenu(controller, Dialogs::Key(groupHistory),
						   Dialogs::EntryState::Section::Profile);
	const auto topicMenu = SnapshotPeerMenu(
		controller, Dialogs::Key(topic), Dialogs::EntryState::Section::Replies);
	const auto boostsChannel = tr::lng_boost_channel_button(tr::now);
	const auto boostsGroup = tr::lng_boost_group_button(tr::now);
	const auto boosts = tr::lng_boosts_title(tr::now);
	const auto statistics = tr::lng_stats_title(tr::now);
	const auto earnings = tr::lng_channel_earn_title(tr::now);
	const auto match
		= checkEntry(channelChat, boostsChannel, supported, "channel chat")
		  && checkEntry(groupChat, boostsGroup, supported, "group chat")
		  && checkEntry(groupChatsList, boostsGroup, supported, "chat list")
		  && checkEntry(channelProfile, boosts, supported, "channel profile")
		  && checkEntry(groupProfile, boosts, supported, "group profile")
		  && checkEntry(topicMenu, boostsGroup, supported, "topic")
		  && checkEntry(channelProfile, statistics, true, "channel profile")
		  && checkEntry(channelProfile, earnings, true, "channel profile")
		  && checkEntry(groupProfile, statistics, true, "group profile")
		  && checkEntry(groupProfile, earnings, true, "group profile");
	if (match) {
		std::fprintf(
			stderr,
			"Boost menu visibility regression passed: session=%s supported=%d; "
			"menu actions and accessibility items match.\n",
			sessionName, supported);
	}
	return match;
}

[[nodiscard]] bool WaitForMainQueueBarrier();

[[nodiscard]] bool
RunTeagramBrandingRegression(not_null<Window::SessionController *> controller,
							 const char *sessionName) {
	const auto fail = [&](const char *what) {
		std::fprintf(
			stderr, "Teagram branding regression failed: session=%s case=%s.\n",
			sessionName, what);
		return false;
	};
	{
		const auto menu = std::make_unique<Window::MainMenu>(
			controller->widget().get(), controller);
		const auto labels = menu->findChildren<QWidget *>();
		const auto hasTeagramFooter
			= std::any_of(labels.begin(), labels.end(), [](const auto widget) {
				  const auto label = dynamic_cast<Ui::FlatLabel *>(widget);
				  return label && label->accessibilityName() == u"Teagram"_q;
			  });
		if (!hasTeagramFooter) {
			return fail("main_menu_footer_name");
		}
	}
	if (controller->isLayerShown()) {
		controller->hideLayer(anim::type::instant);
	}
	const auto box = controller->show(Box(AboutBox), Ui::LayerOption::KeepOther,
									  anim::type::instant);
	QCoreApplication::processEvents();
	const auto content = box.get();
	const auto layer = content ? content->parentWidget() : nullptr;
	if (!layer) {
		controller->hideLayer(anim::type::instant);
		return fail("about_box_did_not_open");
	}
	auto visibleLabels = QStringList();
	for (const auto widget : layer->findChildren<QWidget *>()) {
		if (const auto label = dynamic_cast<Ui::FlatLabel *>(widget)) {
			visibleLabels.push_back(label->accessibilityName());
		}
	}
	const auto visibleText = visibleLabels.join(u"\n"_q);
	const auto hasTeagramTitle = visibleLabels.contains(u"Teagram"_q);
	const auto hasLicense = visibleText.contains(u"GNU GPL"_q);
	const auto hasSourceCode = visibleText.contains(u"GitHub"_q);
	const auto hasProtocolAttribution
		= visibleText.contains(u"Based on Telegram Desktop"_q);
	const auto hasNoTelegramHelpLinks
		= !visibleText.contains(u"Telegram API"_q)
		  && !visibleText.contains(u"Telegram FAQ"_q);
	const auto matches = hasTeagramTitle && hasLicense && hasSourceCode
						 && hasProtocolAttribution && hasNoTelegramHelpLinks;
	controller->hideLayer(anim::type::instant);
	if (!matches) {
		return fail("about_box_copy");
	}
	controller->showSettings();
	const auto closeSettings = gsl::finally([&] {
		controller->window().hideSettingsAndLayer(anim::type::instant);
	});
	if (!WaitForMainQueueBarrier()) {
		return fail("settings_did_not_open");
	}
	QCoreApplication::processEvents();
	const auto settings = controller->activeLayerSection();
	if (!settings) {
		return fail("settings_did_not_open");
	}
	const auto collectSettingsNames = [](QWidget *root) {
		auto result = QStringList();
		for (const auto widget : root->findChildren<QWidget *>()) {
			if (const auto label = dynamic_cast<Ui::FlatLabel *>(widget)) {
				result.push_back(label->accessibilityName());
			} else if (const auto button
					   = dynamic_cast<Ui::SettingsButton *>(widget)) {
				result.push_back(button->accessibilityName());
			}
		}
		return result;
	};
	const auto hasNoTelegramHelp = [](const QStringList &names) {
		const auto text = names.join(u"\n"_q);
		return !text.contains(u"Telegram FAQ"_q)
			   && !text.contains(u"Telegram Features"_q)
			   && !text.contains(u"Ask a Question"_q)
			   && !text.contains(u"Telegram Support"_q);
	};
	for (const auto &restoredEntry : {
			 u"Telegram FAQ"_q,
			 u"Telegram Features"_q,
			 u"Ask a Question"_q,
		 }) {
		QWidget probe;
		const auto button = std::make_unique<Ui::SettingsButton>(
			&probe, rpl::single(restoredEntry));
		const auto probeNames = collectSettingsNames(&probe);
		if (button->accessibilityName() != restoredEntry
			|| !probeNames.contains(restoredEntry)
			|| hasNoTelegramHelp(probeNames)) {
			return fail("settings_help_restoration_detection");
		}
	}
	if (!hasNoTelegramHelp(collectSettingsNames(settings))) {
		return fail("settings_telegram_help");
	}
	return true;
}

// Stories and call controls are presentation-gated per owning session. The
// admin box checks run their own save computation, so checkbox-only saves
// that drop hidden rights fail here and in the standalone save cases.
[[nodiscard]] bool RunStoryControlsVisibilityRegression(
	not_null<Window::SessionController *> controller, bool supported,
	const char *sessionName) {
	auto &session = controller->session();
	const auto fail = [&](const char *what) {
		std::fprintf(stderr,
					 "Session controls regression failed: session=%s "
					 "supported=%d case=%s.\n",
					 sessionName, supported, what);
		return false;
	};
	if (session.storiesSupported() != supported) {
		return fail("capability");
	}
	const auto callsSupported = session.callsSupported();
	if (callsSupported != supported) {
		return fail("call_capability");
	}
	if (controller->window().isLayerShown()) {
		controller->window().hideSettingsAndLayer(anim::type::instant);
	}
	QCoreApplication::processEvents();
	if (controller->window().isLayerShown()) {
		return fail("layer_stack_reset");
	}

	{
		const auto menu = std::make_unique<Window::MainMenu>(
			controller->widget().get(), controller);
		if (menu->myProfileRowPresentForRegressionTest() != supported
			|| menu->topDividerShownForRegressionTest() != supported
			|| menu->callsRowPresentForRegressionTest() != callsSupported) {
			return fail("main_menu_feature_rows");
		}
	}

	using Right = ChatAdminRight;
	using Rights = ChatAdminRights;
	const auto stories = Ui::EditPeer::AdminRights::StoryRights();
	const auto labelsHaveStories = [&](Data::AdminRightsSetOptions options) {
		auto all = Rights();
		for (const auto &entry : AdminRightLabels(options)) {
			all |= entry.flags;
		}
		const auto transfer = AdminRightsForOwnershipTransfer(options);
		return ((all & stories) == stories)
			   && ((transfer & stories) == stories);
	};
	const auto labelsHaveNoStories = [&](Data::AdminRightsSetOptions options) {
		auto all = Rights();
		for (const auto &entry : AdminRightLabels(options)) {
			all |= entry.flags;
		}
		const auto transfer = AdminRightsForOwnershipTransfer(options);
		return !(all & stories) && !(transfer & stories);
	};
	const auto labelsHaveManageCall = [&](Data::AdminRightsSetOptions options) {
		auto all = Rights();
		for (const auto &entry : AdminRightLabels(options)) {
			all |= entry.flags;
		}
		const auto transfer = AdminRightsForOwnershipTransfer(options);
		return (all & Right::ManageCall) && (transfer & Right::ManageCall);
	};
	const auto labelOptions = std::vector<Data::AdminRightsSetOptions>{
		{.storiesSupported = supported, .callsSupported = callsSupported},
		{.isGroup = true,
		 .storiesSupported = supported,
		 .callsSupported = callsSupported},
		{.isGroup = true,
		 .isForum = true,
		 .storiesSupported = supported,
		 .callsSupported = callsSupported},
	};
	for (const auto &options : labelOptions) {
		if (supported ? !labelsHaveStories(options)
					  : !labelsHaveNoStories(options)) {
			return fail("admin_right_labels_and_transfer");
		}
		if (labelsHaveManageCall(options) != callsSupported) {
			return fail("admin_right_manage_call_label");
		}
	}

	using Flag = ChannelDataFlag;
	const auto channel = session.data().channel(ChannelId(1181));
	channel->setAccessHash(118100);
	channel->setName(u"Story controls regression channel"_q, QString());
	channel->setFlags(Flag::Creator | Flag::Broadcast);
	channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
	const auto channelPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*channel));
	if (channelPeer->canManageGroupCall() != callsSupported) {
		return fail("group_call_permission");
	}
	const auto history = session.data().history(channel);
	const auto group = session.data().channel(ChannelId(1184));
	group->setAccessHash(118400);
	group->setName(u"Call controls regression group"_q, QString());
	group->setFlags(Flag::Creator | Flag::Forum);
	group->setLoadedStatus(PeerData::LoadedStatus::Normal);
	const auto groupPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*group));
	if (groupPeer->canManageGroupCall() != callsSupported) {
		return fail("group_call_permission");
	}
	const auto groupHistory = session.data().history(group);
	const auto forum = group->forum();
	const auto topic
		= forum ? forum->applyTopicAdded(
					  MsgId(1185), u"Call controls regression topic"_q, 0,
					  DocumentId(), session.userPeerId(), 1, true)
				: nullptr;
	const auto channelCallMenu
		= SnapshotVideoChatMenu(controller, Dialogs::Key(history));
	const auto groupCallMenu
		= SnapshotVideoChatMenu(controller, Dialogs::Key(groupHistory));
	const auto contains = [](const QStringList &names,
							 const QString &expected) {
		return std::find(names.begin(), names.end(), expected) != names.end();
	};
	const auto menuEntryMatches = [&](const PeerMenuSnapshot &menu,
									  const QString &expected) {
		const auto inActions = contains(menu.actions, expected);
		const auto inAccessibility = contains(menu.accessibleNames, expected);
		const auto inKeyboard = contains(menu.keyboardNames, expected);
		return (inActions == callsSupported)
			   && (inAccessibility == callsSupported)
			   && (inKeyboard == callsSupported);
	};
	const auto groupCall = tr::lng_menu_start_group_call(tr::now);
	const auto groupCallScheduled
		= tr::lng_menu_start_group_call_scheduled(tr::now);
	const auto groupCallWith = tr::lng_menu_start_group_call_with(tr::now);
	const auto liveStream = tr::lng_menu_start_group_call_channel(tr::now);
	const auto liveStreamScheduled
		= tr::lng_menu_start_group_call_scheduled_channel(tr::now);
	const auto liveStreamWith
		= tr::lng_menu_start_group_call_with_channel(tr::now);
	const auto videoChatOptions
		= tr::lng_menu_start_group_call_options(tr::now);
	const auto forumMenu
		= SnapshotPeerMenu(controller, Dialogs::Key(groupHistory),
						   Dialogs::EntryState::Section::ChatsList);
	if (!topic || !menuEntryMatches(groupCallMenu, groupCall)
		|| !menuEntryMatches(groupCallMenu, groupCallScheduled)
		|| !menuEntryMatches(groupCallMenu, groupCallWith)
		|| !menuEntryMatches(channelCallMenu, liveStream)
		|| !menuEntryMatches(channelCallMenu, liveStreamScheduled)
		|| !menuEntryMatches(channelCallMenu, liveStreamWith)
		|| !menuEntryMatches(forumMenu, videoChatOptions)) {
		return fail("video_chat_menus_accessibility_and_keyboard");
	}
	{
		const auto privateUser
			= session.data().processUsers(MTP_vector<MTPUser>({
				RegressionUser(UserId(1186), false, u"1186"_q, false),
			}));
		if (!privateUser) {
			return fail("private_call_fixture");
		}
		const auto privateHistory = session.data().history(privateUser);
		if (!privateHistory) {
			return fail("private_call_history");
		}
		Info::Profile::TopBar profileTopBar(
			controller->widget().get(),
			Info::Profile::TopBar::Descriptor{
				.controller = controller,
				.key = Info::Key(not_null<PeerData *>(privateUser)),
				.wrap = rpl::single(Info::Wrap::Layer),
				.backToggles = rpl::single(false),
				.showFinished = rpl::never<>(),
				.customStatus = rpl::never<TextWithEntities>(),
			});
		if (profileTopBar.callActionPresentForRegressionTest()
			!= callsSupported) {
			return fail("profile_call_button");
		}
		HistoryView::TopBarWidget topBar(controller->widget().get(),
										 controller);
		topBar.setActiveChat(
			Dialogs::EntryState{.key = Dialogs::Key(privateHistory)}, nullptr);
		topBar.updateControlsVisibility();
		if (topBar.callButtonHiddenForRegressionTest() != !callsSupported) {
			return fail("private_call_button");
		}
		topBar.setActiveChat(
			Dialogs::EntryState{.key = Dialogs::Key(groupHistory)}, nullptr);
		topBar.updateControlsVisibility();
		if (topBar.groupCallButtonHiddenForRegressionTest()
			!= !callsSupported) {
			return fail("group_call_button");
		}
	}
	const auto archive = tr::lng_stories_archive_button(tr::now);
	for (const auto section : {
			 Dialogs::EntryState::Section::History,
			 Dialogs::EntryState::Section::Profile,
		 }) {
		const auto menu
			= SnapshotPeerMenu(controller, Dialogs::Key(history), section);
		if (contains(menu.actions, archive) != supported
			|| contains(menu.accessibleNames, archive) != supported
			|| contains(menu.keyboardNames, archive) != supported) {
			return fail("own_channel_story_archive");
		}
	}

	const auto admin = session.data().user(UserId(1182));
	const auto openAdminBox = [&](Rights existing) {
		auto box = Box<EditAdminBox>(channelPeer, admin,
									 ChatAdminRightsInfo(existing), QString(),
									 TimeId(0), nullptr);
		box->setSaveCallback([](ChatAdminRightsInfo, ChatAdminRightsInfo,
								const std::optional<QString> &) {});
		auto result = controller->window().show(
			std::move(box), Ui::LayerOption::KeepOther, anim::type::instant);
		const auto opened = bool(result);
		QCoreApplication::processEvents();
		if (!result) {
			std::fprintf(
				stderr,
				"Session controls regression: admin_box_lifetime opened=%d "
				"after_process_events=0 layer_shown=%d.\n",
				opened, controller->window().isLayerShown());
		}
		return result;
	};
	const auto visibleChannel = Rights(Right::ChangeInfo) | Right::PostMessages
								| Right::EditMessages | Right::DeleteMessages
								| Right::InviteByLinkOrAdd | Right::ManageCall
								| Right::ManageDirect | Right::BanUsers;
	const auto adminCase = [&](const char *name, Rights existingStories) {
		const auto existing = visibleChannel | existingStories | Right::Other;
		auto box = openAdminBox(existing);
		if (!box) {
			std::fprintf(
				stderr,
				"Session controls regression failed: session=%s supported=%d "
				"case=admin_box_not_shown admin_case=%s layer_shown=%d.\n",
				sessionName, supported, name,
				controller->window().isLayerShown());
			return false;
		}
		const auto visible = box->visibleRightsForRegressionTest();
		const auto checked = box->checkedRightsForRegressionTest();
		const auto saved = box->rightsToSaveForRegressionTest(
			checked & ~Rights(Right::PostMessages));
		const auto expected
			= (existing & ~Rights(Right::PostMessages)) | Right::Other;
		const auto rowsMatch
			= supported ? ((visible & stories) == stories)
						: (!(visible & stories) && !(checked & stories));
		const auto callRowMatches
			= bool(visible & Right::ManageCall) == callsSupported;
		controller->hideLayer(anim::type::instant);
		QCoreApplication::processEvents();
		if (!rowsMatch || !callRowMatches
			|| (saved.value() != expected.value())) {
			std::fprintf(stderr,
						 "Story controls regression: case=%s visible=%d "
						 "checked=%d saved=%d expected=%d.\n",
						 name, int(visible.value()), int(checked.value()),
						 int(saved.value()), int(expected.value()));
			return fail(name);
		}
		return true;
	};
	if (!adminCase("existing_admin_all_story_rights", stories)
		|| !adminCase("existing_admin_no_story_rights", Rights())
		|| !adminCase("existing_admin_mixed_story_rights",
					  Rights(Right::PostStories) | Right::DeleteStories)) {
		return false;
	}
	{
		auto box = openAdminBox(Rights());
		if (!box) {
			return fail("new_admin_box");
		}
		const auto checked = box->checkedRightsForRegressionTest();
		const auto saved = box->rightsToSaveForRegressionTest(checked);
		controller->hideLayer(anim::type::instant);
		QCoreApplication::processEvents();
		const auto savedStories = (saved & stories);
		const auto expectedStories = supported ? stories : Rights();
		if ((savedStories.value() != expectedStories.value())
			|| (!callsSupported && (saved & Right::ManageCall))
			|| ((saved & visibleChannel).value()
				!= (checked & visibleChannel).value())) {
			return fail("new_admin_story_defaults");
		}
	}
	if (!supported) {
		// Offering a bot's default rights looks up its current rights
		// first. Closing the chooser before the reply must leave nothing
		// to run against the destroyed controller.
		auto capture = ChannelSaveRequestCapture{
			.instance = &session.mtp(),
		};
		const auto restoreCapture = gsl::finally(
			[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
				gChannelSaveRequestCapture = previous;
			});
		const auto bot = session.data().processUsers(
			MTP_vector<MTPUser>({RegressionBotUser(UserId(1183))}));
		if (!bot || !bot->botInfo) {
			return fail("bot_lookup_fixture");
		}
		bot->botInfo->channelAdminRights = Rights(Right::ChangeInfo) | stories;
		const auto lookup
			= MTPchannels_GetParticipant(channel->inputChannel(), bot->input());
		const auto reply = MTP_channels_channelParticipant(
			MTP_channelParticipantAdmin(
				MTP_flags(0), MTP_long(peerToUser(bot->id).bare), MTPlong(),
				MTP_long(session.userId().bare), MTP_int(0),
				AdminRightsToMTP(ChatAdminRightsInfo(Rights(Right::ChangeInfo)
													 | Right::PostStories)),
				MTPstring()),
			MTP_vector<MTPChat>(0), MTP_vector<MTPUser>(0));
		const auto startLookup = [&] {
			capture.requests.clear();
			auto chooser = std::make_unique<AddBotToGroupBoxController>(
				controller, bot, AddBotToGroupBoxController::Scope::All,
				QString(), ChatAdminRights());
			auto row = PeerListRow(channelPeer);
			chooser->rowClicked(&row);
			return chooser;
		};
		const auto hideLayers = [&] {
			if (controller->isLayerShown()) {
				controller->hideLayer(anim::type::instant);
			}
			QCoreApplication::processEvents();
		};
		hideLayers();
		auto chooser = startLookup();
		if (controller->isLayerShown()
			|| !ReplyToChannelSaveRequestForRegressionTest(capture, 0, lookup,
														   reply)) {
			return fail("bot_lookup_request");
		}
		QCoreApplication::processEvents();
		const auto opened = controller->isLayerShown();
		hideLayers();
		chooser = nullptr;
		if (!opened) {
			return fail("bot_lookup_opens_editor");
		}

		chooser = startLookup();
		chooser = nullptr;
		if (!ReplyToChannelSaveRequestForRegressionTest(capture, 0, lookup,
														reply)) {
			return fail("bot_lookup_after_dismissal_request");
		}
		QCoreApplication::processEvents();
		if (controller->isLayerShown()) {
			hideLayers();
			return fail("bot_lookup_after_dismissal");
		}
	}

	const auto resetSections = [&] {
		if (controller->isLayerShown()) {
			controller->hideLayer(anim::type::instant);
		}
		controller->clearSectionStack(Window::SectionShow(
			anim::type::instant, anim::activation::background));
		QCoreApplication::processEvents();
	};
	auto &storiesData = session.data().stories();
	const auto linkTo = [&](QString storyParam, int albumId) {
		auto info = Window::PeerByLinkInfo();
		info.usernameOrId = peerToChannel(channel->id);
		info.storyParam = storyParam;
		info.storyAlbumId = albumId;
		return info;
	};
	if (supported) {
		resetSections();
		const auto toastBefore
			= controller
				  ->featureUnavailableOnServerToastCallsForRegressionTest();
		controller->showPeerByLink(linkTo(u"1"_q, 0));
		QCoreApplication::processEvents();
		if ((controller->featureUnavailableOnServerToastCallsForRegressionTest()
			 != toastBefore)
			|| (controller->activeChatCurrent().peer() == channelPeer.get())) {
			return fail("stock_story_link_not_refused");
		}
		resetSections();
	} else {
		if (storiesData.requestsPendingForRegressionTest()) {
			return fail("story_requests_pending_before_links");
		}
		const auto links
			= std::vector<std::pair<const char *, Window::PeerByLinkInfo>>{
				{"story_link", linkTo(u"1"_q, 0)},
				{"story_link_in_album", linkTo(u"1"_q, 2)},
				{"live_story_link", linkTo(u"live"_q, 0)},
				{"album_link", linkTo(QString(), 2)},
		};
		for (const auto &[name, info] : links) {
			resetSections();
			const auto toastBefore
				= controller
					  ->featureUnavailableOnServerToastCallsForRegressionTest();
			controller->showPeerByLink(info);
			QCoreApplication::processEvents();
			const auto toastShown
				= (controller
					   ->featureUnavailableOnServerToastCallsForRegressionTest()
				   == toastBefore + 1);
			const auto peerShown
				= (controller->activeChatCurrent().peer() == channelPeer.get());
			const auto noStoriesUi = controller->content()->isMainSectionShown()
									 && !ShownStoriesSection(controller);
			const auto noStoriesRequest
				= !storiesData.requestsPendingForRegressionTest();
			if (!toastShown || !peerShown || !noStoriesUi
				|| !noStoriesRequest) {
				std::fprintf(stderr,
							 "Story controls regression: case=%s toast=%d "
							 "peer=%d no-stories-ui=%d no-request=%d.\n",
							 name, toastShown, peerShown, noStoriesUi,
							 noStoriesRequest);
				return fail(name);
			}
		}
		resetSections();
	}

	std::fprintf(stderr,
				 "Story controls regression passed: session=%s supported=%d; "
				 "main menu, own-channel menus, admin rights and story links "
				 "match.\n",
				 sessionName, supported);
	return true;
}

[[nodiscard]] Window::SessionController *PrepareBlockedWithoutPinMiniAppFixture(
	not_null<Main::Account *> blank, UserId selfId,
	not_null<Window::Controller *> blankWindow) {
	blank->setSessionUserId(selfId);
	if (!blank->createSession(RegressionUser(selfId, true, QString()),
							  std::make_unique<Main::SessionSettings>())) {
		std::fprintf(
			stderr,
			"Mini-app open regression: could not create the blocked-session "
			"fixture.\n");
		return nullptr;
	}
	blank->mtp().dcOptions().constructBlocked();
	const auto blankController = blankWindow->sessionController()
									 ? blankWindow->sessionController()
									 : blank->session().tryResolveWindow();
	if (!blankController || (&blankController->session() != &blank->session())
		|| blank->mtp().dcOptions().hasCustomServer()
		|| !blank->mtp().dcOptions().blocked()
		|| blank->session().botAppsSupported()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: blocked fixture is not blocked-without-"
			"pin: blocked=%d custom=%d apps=%d controller=%p\n",
			blank->mtp().dcOptions().blocked(),
			blank->mtp().dcOptions().hasCustomServer(),
			blank->session().botAppsSupported(),
			static_cast<const void *>(blankController));
		blank->forcedLogOut();
		return nullptr;
	}
	const auto blankWasPremium = blank->session().premium();
	const auto blankPremiumEnabled
		= SetRegressionSessionPremium(&blank->session(), selfId, true);
	const auto blankSearchMatches = blankPremiumEnabled
									&& SettingsSearchMatchesExpected(
										&blank->session(), false, false, false);
	const auto blankPremiumRestored = SetRegressionSessionPremium(
		&blank->session(), selfId, blankWasPremium);
	if (!blankSearchMatches || !blankPremiumRestored) {
		std::fprintf(
			stderr, "Mini-app open regression: blocked fixture settings search "
					"did not match.\n");
		blank->forcedLogOut();
		return nullptr;
	}
	return blankController;
}

[[nodiscard]] bool RunExpiredSettingsDeepLinkTargetRegression(
	Core::Application &app, not_null<Main::Account *> blank,
	not_null<Window::Controller *> blankWindow,
	not_null<Window::SessionController *> stockController) {
	const auto cleanupBlank = gsl::finally([&] { blank->forcedLogOut(); });
	const auto expiredController = blankWindow->sessionController();
	if (!expiredController
		|| (&expiredController->session() != &blank->session())) {
		std::fprintf(
			stderr,
			"Settings deep link regression failed: expired controller did "
			"not belong to the blocked account.\n");
		return false;
	}
	const auto expiredControllerWeak = base::make_weak(expiredController);
	const auto expiredContext = QVariant::fromValue(ClickHandlerContext{
		.sessionWindow = expiredControllerWeak,
	});
	app.closeWindow(blankWindow.get());
	QCoreApplication::processEvents();
	if (!expiredControllerWeak.empty()) {
		std::fprintf(stderr,
					 "Settings deep link regression failed: closed controller "
					 "did not expire.\n");
		return false;
	}
	const auto stockWindowActivationRestored
		= ActivateWindowForRegression(app, &stockController->window(), true);
	const auto activeWindowRestored
		= app.activeWindow() == &stockController->window();
	const auto activePrimaryWindowRestored
		= app.activePrimaryWindow() == &stockController->window();
	if (!stockWindowActivationRestored || !activeWindowRestored
		|| !activePrimaryWindowRestored) {
		std::fprintf(stderr,
					 "Settings deep link activation counts: active-window=%d "
					 "active-primary-window=%d expected=1/1.\n",
					 activeWindowRestored, activePrimaryWindowRestored);
		std::fprintf(
			stderr,
			"Settings deep link regression failed: stock window activation "
			"was not restored.\n");
		return false;
	}
	const auto expiredToastBefore
		= stockController
			  ->featureUnavailableOnServerToastCallsForRegressionTest();
	const auto expiredActiveBefore = app.activeWindow();
	const auto expiredHandled
		= app.openLocalUrl(u"tg://settings/premium"_q, expiredContext);
	QCoreApplication::processEvents();
	if (!expiredHandled || (stockController->content()->isMainSectionShown())
		|| stockController->isLayerShown()
		|| (stockController
				->featureUnavailableOnServerToastCallsForRegressionTest()
			!= expiredToastBefore)
		|| (app.activeWindow() != expiredActiveBefore)) {
		std::fprintf(
			stderr,
			"Settings deep link regression failed: expired target fell back "
			"to the stock account.\n");
		return false;
	}
	return true;
}

// The accepted refusal contract, observed instead of inferred: a refused
// mini-app open shows the shared unavailable-server toast on the owning
// session and leaves the webview path untouched, while a stock session keeps
// its opens, its existing app, and the activation of that app. A session
// pinned to a custom server, a session blocked with no custom pin, and
// a session whose window belongs to another account are all covered.
[[nodiscard]] bool RunMiniAppOpenRefusalRegression(
	Core::Application &app, UserId selfId, not_null<Main::Account *> stock,
	not_null<Main::Account *> pinned, not_null<Main::Account *> blank,
	not_null<Window::SessionController *> blankController,
	not_null<Window::Controller *> stockWindow,
	not_null<Window::Controller *> pinnedWindow) {
	const auto stockController = stockWindow->sessionController();
	const auto pinnedController = pinnedWindow->sessionController();
	if (!stockController || !pinnedController) {
		std::fprintf(
			stderr,
			"Mini-app open regression: no session controller: stock=%p "
			"pinned=%p\n",
			static_cast<const void *>(stockController),
			static_cast<const void *>(pinnedController));
		return false;
	}
	const auto stockControllerLifetime = base::make_weak(stockController);
	const auto pinnedControllerLifetime = base::make_weak(pinnedController);
	const auto blankControllerLifetime = base::make_weak(blankController.get());
	if (stock->mtp().dcOptions().hasCustomServer()
		|| stock->mtp().dcOptions().blocked()
		|| !pinned->mtp().dcOptions().hasCustomServer()
		|| pinned->mtp().dcOptions().blocked()
		|| !stock->session().botAppsSupported()
		|| pinned->session().botAppsSupported()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: fixture configuration is not stock "
			"and custom-pinned.\n");
		return false;
	}
	const auto stockChatAutomationRow
		= Settings::InformationChatAutomationRowPresentForRegressionTest(
			stockController);
	const auto pinnedChatAutomationRow
		= Settings::InformationChatAutomationRowPresentForRegressionTest(
			pinnedController);
	const auto stockSearchTabOrder = Dialogs::Suggestions::
		SearchTabOrderMatchesAppsSupportForRegressionTest(stockController);
	const auto pinnedSearchTabOrder = Dialogs::Suggestions::
		SearchTabOrderMatchesAppsSupportForRegressionTest(pinnedController);
	if (!stockChatAutomationRow || pinnedChatAutomationRow
		|| !stockSearchTabOrder || !pinnedSearchTabOrder) {
		std::fprintf(
			stderr,
			"Custom-server controls regression failed: automation=%d/%d, "
			"search tabs=%d/%d.\n",
			stockChatAutomationRow, pinnedChatAutomationRow,
			stockSearchTabOrder, pinnedSearchTabOrder);
		return false;
	}
	{
		const auto stockWasPremium = stock->session().premium();
		const auto pinnedWasPremium = pinned->session().premium();
		const auto restorePremium = gsl::finally([&] {
			const auto stockRestored = SetRegressionSessionPremium(
				&stock->session(), selfId, stockWasPremium);
			const auto pinnedRestored = SetRegressionSessionPremium(
				&pinned->session(), selfId, pinnedWasPremium);
			Expects(stockRestored && pinnedRestored);
		});
		if (!SetRegressionSessionPremium(&stock->session(), selfId, true)
			|| !SetRegressionSessionPremium(&pinned->session(), selfId, true)
			|| !SettingsSearchMatchesExpected(&stock->session(), true, true,
											  true)
			|| !SettingsSearchMatchesExpected(&pinned->session(), false, false,
											  false)
			|| !SettingsSearchMatchesExpected(&pinned->session(), false, false,
											  false)
			|| !SettingsSearchMatchesExpected(&stock->session(), true, true,
											  true)) {
			std::fprintf(
				stderr,
				"Mini-app open regression: Settings search results crossed "
				"stock and custom sessions.\n");
			return false;
		}
	}
	const auto regressionUserId = UserId(777);
	const auto stockBot = stock->session()
							  .data()
							  .peer(peerFromUser(regressionUserId))
							  ->asUser();
	const auto pinnedBot = pinned->session()
							   .data()
							   .peer(peerFromUser(regressionUserId))
							   ->asUser();
	if (!stockBot || !pinnedBot) {
		std::fprintf(
			stderr,
			"Mini-app open regression: no bot peer fixture in the sessions.\n");
		return false;
	}
	const auto stockAction = Api::SendAction(
		stock->session().data().history(stock->session().user()));
	const auto pinnedAction = Api::SendAction(
		pinned->session().data().history(pinned->session().user()));
	const auto appSource = InlineBots::WebViewSourceLinkApp{
		.appname = u"regression_app"_q,
		.token = u"token"_q,
	};

	// A stock session accepts the app open: that is the app the refusal on the
	// custom session must not touch.
	const auto cleanupStockWebViewOnExit = gsl::finally([&] {
		stock->session().attachWebView().cancel();
		stock->session().attachWebView().closeAll();
	});
	const auto stockBefore = ReadWebViewOpenCounters(&stock->session());
	if (!stock->session().attachWebView().open({
          .bot = stockBot,
          .context =
              {
                  .controller = stockController,
                  .action = stockAction,
                  .maySkipConfirmation = true,
              },
          .source = appSource,
      })) {
		std::fprintf(
			stderr,
			"Mini-app open regression: stock session refused an app link.\n");
		return false;
	}
	const auto stockOpened = ReadWebViewOpenCounters(&stock->session());
	if (!WebViewOpenDeltaMatches("stock app link", stockBefore, stockOpened, 0,
								 1, 1, 0, 1)) {
		return false;
	}

	// A custom-pinned session refuses every entry class and the username entry,
	// with the toast as the only observable effect.
	const auto pinnedBefore = ReadWebViewOpenCounters(&pinned->session());
	if (AcceptedMiniAppOpens(&pinned->session(), pinnedBot, pinnedController)
		!= 0) {
		std::fprintf(
			stderr,
			"Mini-app open regression: custom-pinned session accepted an "
			"entry-class open.\n");
		return false;
	}
	const auto pinnedRefused = ReadWebViewOpenCounters(&pinned->session());
	if (!WebViewOpenDeltaMatches("custom-pinned entry classes", pinnedBefore,
								 pinnedRefused, kRegressionEntryClassCount, 0,
								 0, 0, 0)) {
		return false;
	}
	if (pinned->session().attachWebView().openByUsername(
			pinnedController, pinnedAction, u"regression_bot"_q, QString(),
			false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: custom-pinned session accepted a "
			"username open.\n");
		return false;
	}
	const auto pinnedUsername = ReadWebViewOpenCounters(&pinned->session());
	if (!WebViewOpenDeltaMatches("custom-pinned username open", pinnedRefused,
								 pinnedUsername, 1, 0, 0, 0, 0)) {
		return false;
	}
	std::fprintf(
		stderr,
		"Mini-app open refusal passed: custom-session toasts=%d/%d "
		"requests=%d/0 activations=%d/0 resolves=%d/0 instances=%d/0.\n",
		pinnedUsername.toasts - pinnedBefore.toasts,
		kRegressionEntryClassCount + 1,
		pinnedUsername.requests - pinnedBefore.requests,
		pinnedUsername.activations - pinnedBefore.activations,
		pinnedUsername.resolves - pinnedBefore.resolves,
		pinnedUsername.instances - pinnedBefore.instances);

	// An open whose window belongs to another account is refused without
	// a toast and without any webview side effect: no active-account
	// substitution, on the session that would have been substituted into.
	const auto guardBefore = ReadWebViewOpenCounters(&stock->session());
	if (stock->session().attachWebView().open({
          .bot = stockBot,
          .context =
              {
                  .controller = pinnedController,
                  .action = stockAction,
                  .maySkipConfirmation = true,
              },
          .source = InlineBots::WebViewSourceBotProfile{},
      })) {
		std::fprintf(
			stderr,
			"Mini-app open regression: a window of another account opened an "
			"app.\n");
		return false;
	}
	if (stock->session().attachWebView().openByUsername(
			pinnedController, stockAction, u"regression_bot"_q, QString(),
			false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: a window of another account resolved "
			"an app.\n");
		return false;
	}
	const auto guardAfter = ReadWebViewOpenCounters(&stock->session());
	if (!WebViewOpenDeltaMatches("foreign window substitution", guardBefore,
								 guardAfter, 0, 0, 0, 0, 0)) {
		return false;
	}

	// The stock app survived every refusal on the custom session, and the same
	// open activates it instead of building a second one.
	const auto stockAlive = ReadWebViewOpenCounters(&stock->session());
	if (!WebViewOpenDeltaMatches("stock app after custom refusals", stockOpened,
								 stockAlive, 0, 0, 0, 0, 0)) {
		return false;
	}
	if (!stock->session().attachWebView().open({
          .bot = stockBot,
          .context =
              {
                  .controller = stockController,
                  .action = stockAction,
                  .maySkipConfirmation = true,
              },
          .source = appSource,
      })) {
		std::fprintf(
			stderr,
			"Mini-app open regression: stock session refused a repeat open.\n");
		return false;
	}
	const auto stockActivated = ReadWebViewOpenCounters(&stock->session());
	if (!WebViewOpenDeltaMatches("stock app activation", stockAlive,
								 stockActivated, 0, 0, 1, 0, 0)) {
		return false;
	}
	stock->session().attachWebView().cancel();
	stock->session().attachWebView().closeAll();

	// A link that names a chat opens that chat first, then the attach-bot open
	// is refused with the toast, and the chat stays shown.
	const auto contact = pinned->session().data().peer(peerFromUser(UserId(2)));
	const auto chatBefore = ReadWebViewOpenCounters(&pinned->session());
	pinnedController->showPeerHistory(contact);
	QCoreApplication::processEvents();
	const auto chatShown = pinnedController->dialogsEntryStateCurrent();
	const auto chatOpened = ReadWebViewOpenCounters(&pinned->session());
	if ((chatShown.key.peer() != contact.get())
		|| !WebViewOpenDeltaMatches("chat link opened the chat first",
									chatBefore, chatOpened, 0, 0, 0, 0, 0)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: attach link chat did not open before "
			"the refusal: peer=%p expected=%p\n",
			static_cast<const void *>(chatShown.key.peer()),
			static_cast<const void *>(contact.get()));
		return false;
	}
	if (pinned->session().attachWebView().openByUsername(
			pinnedController,
			Api::SendAction(pinned->session().data().history(contact)),
			u"regression_bot"_q, QString(), false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: custom session accepted the attach link "
			"app.\n");
		return false;
	}
	const auto afterChatRefusal = pinnedController->dialogsEntryStateCurrent();
	const auto chatRefused = ReadWebViewOpenCounters(&pinned->session());
	if ((afterChatRefusal.key.peer() != contact.get())
		|| !WebViewOpenDeltaMatches("chat link attach refusal", chatOpened,
									chatRefused, 1, 0, 0, 0, 0)) {
		return false;
	}

	// A session blocked with no custom pin to fall back on refuses the same
	// matrix: the block alone is enough, with no custom server set.
	const auto blankBot = blank->session()
							  .data()
							  .peer(peerFromUser(regressionUserId))
							  ->asUser();
	const auto blankBefore = ReadWebViewOpenCounters(&blank->session());
	if (!blankBot
		|| AcceptedMiniAppOpens(&blank->session(), blankBot, blankController)
			   != 0) {
		std::fprintf(
			stderr,
			"Mini-app open regression: blocked-without-pin session accepted "
			"an open.\n");
		return false;
	}
	if (blank->session().attachWebView().openByUsername(
			blankController,
			Api::SendAction(
				blank->session().data().history(peerFromUser(selfId))),
			u"regression_bot"_q, QString(), false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: blocked-without-pin session accepted "
			"a username open.\n");
		return false;
	}
	const auto blankRefused = ReadWebViewOpenCounters(&blank->session());
	if (!WebViewOpenDeltaMatches("blocked-without-pin refusals", blankBefore,
								 blankRefused, kRegressionEntryClassCount + 1,
								 0, 0, 0, 0)) {
		return false;
	}

	// A previously custom-pinned account can be restored into a blocked
	// config when its stored pin is unreadable; the durable marker retains it.
	auto pinnedServerRestored = false;
	const auto restorePinnedServerOnExit = gsl::finally([&] {
		if (!pinnedServerRestored) {
			const auto restored
				= ConfigurePinnedServer(pinned, RegressionServerKey());
			if (!restored || !pinned->mtp().dcOptions().hasCustomServer()
				|| pinned->mtp().dcOptions().blocked()) {
				std::fprintf(stderr,
							 "Mini-app open regression: scope-exit could not "
							 "restore the "
							 "custom pin: configured=%d custom=%d blocked=%d\n",
							 restored,
							 pinned->mtp().dcOptions().hasCustomServer(),
							 pinned->mtp().dcOptions().blocked());
			}
		}
	});
	pinned->local().writeCustomServerBlocked(false);
	pinned->mtp().dcOptions().constructBlocked();
	if (!pinned->mtp().config().blocked()
		|| pinned->mtp().dcOptions().hasCustomServer()
		|| !pinned->local().hasStoredCustomServer()
		|| pinned->local().customServerPinUnknown()
		|| pinned->session().botAppsSupported()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: fixture is not custom-plus-blocked: "
			"blocked=%d custom=%d stored-custom=%d pin-unknown=%d apps=%d\n",
			pinned->mtp().config().blocked(),
			pinned->mtp().dcOptions().hasCustomServer(),
			pinned->local().hasStoredCustomServer(),
			pinned->local().customServerPinUnknown(),
			pinned->session().botAppsSupported());
		return false;
	}
	const auto customBlockedBefore
		= ReadWebViewOpenCounters(&pinned->session());
	if (AcceptedMiniAppOpens(&pinned->session(), pinnedBot, pinnedController)
		!= 0) {
		std::fprintf(
			stderr,
			"Mini-app open regression: custom-plus-blocked session accepted "
			"an entry-class open.\n");
		return false;
	}
	if (pinned->session().attachWebView().openByUsername(
			pinnedController, pinnedAction, u"regression_bot"_q, QString(),
			false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: custom-plus-blocked session accepted "
			"a username open.\n");
		return false;
	}
	const auto customBlockedRefused
		= ReadWebViewOpenCounters(&pinned->session());
	if (!WebViewOpenDeltaMatches("custom-plus-blocked refusals",
								 customBlockedBefore, customBlockedRefused,
								 kRegressionEntryClassCount + 1, 0, 0, 0, 0)) {
		return false;
	}
	const auto customPinConfigured
		= ConfigurePinnedServer(pinned, RegressionServerKey());
	pinnedServerRestored = true;
	if (!customPinConfigured || !pinned->mtp().dcOptions().hasCustomServer()
		|| pinned->mtp().dcOptions().blocked()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: could not restore the custom pin after "
			"the combined-state matrix: custom=%d blocked=%d\n",
			pinned->mtp().dcOptions().hasCustomServer(),
			pinned->mtp().dcOptions().blocked());
		return false;
	}

	// Deferred resolution, completed on demand. The seam runs the production
	// completion with its real guards: it opens for a live window of the
	// owning session, it refuses after that window is destroyed instead of
	// opening in the window that replaced it, and it dies with a session so
	// that a replacement session for the same user id gets nothing.
	const auto deferred = app.domain().add(MTP::Environment::Production);
	deferred->mtp().stopForServerEnrollment();
	deferred->setSessionUserId(selfId);
	if (!deferred->createSession(RegressionUser(selfId, true, QString()),
								 std::make_unique<Main::SessionSettings>())) {
		std::fprintf(stderr,
					 "Mini-app open regression: could not create the deferred "
					 "resolution fixture.\n");
		return false;
	}
	const auto botUser = RegressionBotUser(regressionUserId);
	const auto deferredBot = deferred->session().data().processUsers(
		MTP_vector<MTPUser>({botUser}));
	const auto firstWindow = app.ensureSeparateWindowFor(deferred);
	const auto firstController
		= firstWindow ? firstWindow->sessionController() : nullptr;
	if (!firstController || !deferredBot || !deferredBot->isBot()
		|| !deferred->session().botAppsSupported()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: deferred fixture has no stock session "
			"window with a bot peer.\n");
		return false;
	}

	// The seam opens when its guards pass, so every zero asserted below is a
	// refusal and not a no-op.
	const auto seamBefore = ReadWebViewOpenCounters(&deferred->session());
	if (!OpenMiniAppByUsername(&deferred->session(), firstController)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: stock session refused a username "
			"open.\n");
		return false;
	}
	const auto seamResolving = ReadWebViewOpenCounters(&deferred->session());
	if (!WebViewOpenDeltaMatches("stock deferred resolve", seamBefore,
								 seamResolving, 0, 0, 0, 1, 0)) {
		return false;
	}
	deferred->session().attachWebView().completePendingResolveForRegressionTest(
		not_null<PeerData *>(deferredBot));
	QCoreApplication::processEvents();
	const auto seamOpened = ReadWebViewOpenCounters(&deferred->session());
	if (!WebViewOpenDeltaMatches("completion with a live window opens the app",
								 seamResolving, seamOpened, 0, 1, 1, 0, 1)) {
		return false;
	}
	deferred->session().attachWebView().cancel();
	deferred->session().attachWebView().closeAll();

	// A completion that arrives after its window was destroyed must not use
	// the window that replaced it.
	const auto goneBefore = ReadWebViewOpenCounters(&deferred->session());
	if (!OpenMiniAppByUsername(&deferred->session(), firstController)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: stock session refused the second "
			"username open.\n");
		return false;
	}
	const auto goneResolving = ReadWebViewOpenCounters(&deferred->session());
	if (!WebViewOpenDeltaMatches("resolve in flight at window teardown",
								 goneBefore, goneResolving, 0, 0, 0, 1, 0)) {
		return false;
	}
	// Weak observations of the objects the pending completion is bound to,
	// taken before the teardown. Expiry is asserted, so no address that the
	// allocator can hand to the replacement is ever compared.
	const auto firstControllerWeak = base::make_weak(firstController);
	app.closeWindow(firstWindow);
	QCoreApplication::processEvents();
	QCoreApplication::processEvents();
	if (!firstControllerWeak.empty()) {
		std::fprintf(stderr,
					 "Mini-app open regression: the closed window's controller "
					 "outlived the teardown.\n");
		return false;
	}
	if (!deferred->session().windows().empty()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the closed window is still registered "
			"with the session.\n");
		return false;
	}
	const auto secondWindow = app.ensureSeparateWindowFor(deferred);
	QCoreApplication::processEvents();
	const auto secondController
		= secondWindow ? secondWindow->sessionController() : nullptr;
	// The premise of the refusal below, read off the live session: it has
	// exactly one window registered, the replacement, the account shows that
	// window, and the controller of that window serves this session.
	const auto firstRegistration = deferred->session().windows();
	if (!secondController || (firstRegistration.size() != 1)
		|| (*firstRegistration.begin()
			!= not_null<Window::SessionController *>(secondController))
		|| (app.separateWindowFor(deferred) != secondWindow)
		|| (&secondController->session() != &deferred->session())) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the replacement window is not the one "
			"registered with the session: windows=%d\n",
			static_cast<int>(firstRegistration.size()));
		return false;
	}
	deferred->session().attachWebView().completePendingResolveForRegressionTest(
		not_null<PeerData *>(deferredBot));
	QCoreApplication::processEvents();
	const auto goneAfter = ReadWebViewOpenCounters(&deferred->session());
	if (!WebViewOpenDeltaMatches("completion after its window was destroyed",
								 goneResolving, goneAfter, 0, 0, 0, 0, 0)) {
		return false;
	}
	deferred->session().attachWebView().cancel();
	deferred->session().attachWebView().closeAll();

	// A resolution in flight when the session is destroyed. The completion is
	// taken out of the session first, so it can be run against the
	// replacement session: its guard is production code, and a retained copy
	// must refuse without touching the session that replaced the one it came
	// from.
	const auto diedBefore = ReadWebViewOpenCounters(&deferred->session());
	if (!OpenMiniAppByUsername(&deferred->session(), secondController)) {
		std::fprintf(
			stderr, "Mini-app open regression: stock session refused the third "
					"username open.\n");
		return false;
	}
	const auto diedResolving = ReadWebViewOpenCounters(&deferred->session());
	if (!WebViewOpenDeltaMatches("resolve in flight at session teardown",
								 diedBefore, diedResolving, 0, 0, 0, 1, 0)) {
		return false;
	}
	auto retained = deferred->session()
						.attachWebView()
						.takePendingResolveForRegressionTest();
	const auto diedWebviewWeak
		= base::make_weak(&deferred->session().attachWebView());
	const auto diedSessionWeak = base::make_weak(&deferred->session());
	const auto diedControllerWeak = base::make_weak(secondController);
	if (!retained || !diedWebviewWeak || !diedControllerWeak) {
		std::fprintf(stderr,
					 "Mini-app open regression: no completion retained for the "
					 "session teardown.\n");
		return false;
	}
	deferred->forcedLogOut();
	if (!diedSessionWeak.empty()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: teardown kept the session alive with a "
			"resolve in flight.\n");
		return false;
	}
	// Expiry, not an address: the session that owned the retained completion
	// is destroyed, and so is the window that owned its controller.
	if (!diedWebviewWeak.empty()) {
		std::fprintf(stderr,
					 "Mini-app open regression: the destroyed session kept its "
					 "webview.\n");
		return false;
	}
	app.closeWindow(secondWindow);
	QCoreApplication::processEvents();
	QCoreApplication::processEvents();
	if (!diedControllerWeak.empty()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the destroyed session kept its window "
			"controller.\n");
		return false;
	}
	// Closing the old window lets the domain remove its now-sessionless
	// account, so the replacement must be a fresh account with the same user
	// id.
	const auto replacement = app.domain().add(MTP::Environment::Production);
	replacement->mtp().stopForServerEnrollment();
	replacement->setSessionUserId(selfId);
	const auto replacementLifetime = base::make_weak(replacement.get());
	const auto stockWasPremium = stock->session().premium();
	const auto pinnedWasPremium = pinned->session().premium();
	const auto blankWasPremium = blank->session().premium();
	auto accountLimitFixtureRestored = false;
	base::weak_ptr<Window::Controller> disposablePrimaryWindowLifetime;
	const auto restoreAccountLimitFixture = [&] {
		if (accountLimitFixtureRestored) {
			return;
		}
		accountLimitFixtureRestored = true;
		if (const auto strong = replacementLifetime.get()) {
			if (strong->sessionExists()) {
				strong->forcedLogOut();
			}
			if (const auto window = app.separateWindowFor(not_null(strong))) {
				app.closeWindow(window);
			}
		}
		if (const auto window = disposablePrimaryWindowLifetime.get()) {
			if (app.separateWindowFor(window->id()) == window) {
				app.closeWindow(window);
			}
		}
		app.domain().activate(stock);
		QCoreApplication::processEvents();
		QCoreApplication::processEvents();
		app.domain().removeRedundantAccounts();
		QCoreApplication::processEvents();
		const auto stockRestored = SetRegressionSessionPremium(
			&stock->session(), selfId, stockWasPremium);
		const auto pinnedRestored = SetRegressionSessionPremium(
			&pinned->session(), selfId, pinnedWasPremium);
		const auto blankRestored = SetRegressionSessionPremium(
			&blank->session(), selfId, blankWasPremium);
		Expects(stockRestored && pinnedRestored && blankRestored);
	};
	const auto restoreAccountLimitFixtureOnExit
		= gsl::finally(restoreAccountLimitFixture);
	if (!replacement->createSession(
			RegressionUser(selfId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		std::fprintf(
			stderr,
			"Mini-app open regression: could not create the replacement "
			"session.\n");
		return false;
	}
	const auto disposablePrimaryWindow
		= app.ensureSeparateWindowFor(replacement);
	if (disposablePrimaryWindow) {
		disposablePrimaryWindowLifetime
			= base::make_weak(disposablePrimaryWindow);
	}
	QCoreApplication::processEvents();
	QCoreApplication::processEvents();
	const auto replacementController
		= disposablePrimaryWindow ? disposablePrimaryWindow->sessionController()
								  : nullptr;
	// The replacement is a live session of this account with its own window
	// registered, established without comparing any destroyed address.
	const auto secondRegistration = replacement->session().windows();
	if (!disposablePrimaryWindow || !replacementController
		|| (secondRegistration.size() != 1)
		|| (*secondRegistration.begin()
			!= not_null<Window::SessionController *>(replacementController))
		|| (app.separateWindowFor(replacement) != disposablePrimaryWindow)
		|| (&replacementController->session() != &replacement->session())) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the replacement session did not take "
			"the window: windows=%d\n",
			static_cast<int>(secondRegistration.size()));
		return false;
	}
	const auto replacementCounters
		= ReadWebViewOpenCounters(&replacement->session());
	if ((replacementCounters.instances != 0)
		|| (replacementCounters.requests != 0)
		|| (replacementCounters.activations != 0)
		|| (replacementCounters.resolves != 0)
		|| (replacementCounters.toasts != 0)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the replacement session was not "
			"clean at the teardown boundary: instances=%d requests=%d "
			"activations=%d resolves=%d toasts=%d\n",
			replacementCounters.instances, replacementCounters.requests,
			replacementCounters.activations, replacementCounters.resolves,
			replacementCounters.toasts);
		return false;
	}
	// The retained completion of the destroyed session, run against a live
	// peer of the replacement session: its guard fires, and the replacement
	// session sees no access and no side effect.
	const auto replacementBot = replacement->session().data().processUsers(
		MTP_vector<MTPUser>({botUser}));
	if (!replacementBot) {
		std::fprintf(stderr,
					 "Mini-app open regression: no bot peer in the replacement "
					 "session.\n");
		return false;
	}
	retained(not_null<PeerData *>(replacementBot));
	QCoreApplication::processEvents();
	const auto afterRetained = ReadWebViewOpenCounters(&replacement->session());
	if (!WebViewOpenDeltaMatches("retained completion of the destroyed session",
								 replacementCounters, afterRetained, 0, 0, 0, 0,
								 0)) {
		return false;
	}
	// Positive control: the same seam opens on the replacement session,
	// so the zeros above are a refusal and not an inert webview.
	if (!OpenMiniAppByUsername(&replacement->session(),
							   replacementController)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: the replacement session refused a "
			"username open.\n");
		return false;
	}
	replacement->session()
		.attachWebView()
		.completePendingResolveForRegressionTest(
			not_null<PeerData *>(replacementBot));
	QCoreApplication::processEvents();
	const auto replacementOpened
		= ReadWebViewOpenCounters(&replacement->session());
	if (!WebViewOpenDeltaMatches("replacement session opens its own app",
								 afterRetained, replacementOpened, 0, 1, 1, 1,
								 1)) {
		return false;
	}
	replacement->session().attachWebView().cancel();
	replacement->session().attachWebView().closeAll();
	if (!SetRegressionSessionPremium(&stock->session(), selfId, false)
		|| !SetRegressionSessionPremium(&pinned->session(), selfId, false)
		|| !SetRegressionSessionPremium(&blank->session(), selfId, false)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: could not set up the account-limit "
			"fixture.\n");
		return false;
	}
	if (app.domain().maxAccounts() != Main::Domain::kMaxAccounts) {
		std::fprintf(
			stderr,
			"Mini-app open regression: account-limit fixture retained a "
			"premium allowance: max=%d expected=%d.\n",
			app.domain().maxAccounts(), Main::Domain::kMaxAccounts);
		return false;
	}
	app.domain().activate(stock);
	const auto accountsBeforeRedundantCleanup = app.domain().accounts().size();
	app.domain().removeRedundantAccounts();
	const auto accountsAfterRedundantCleanup = app.domain().accounts().size();
	if (accountsAfterRedundantCleanup >= Main::Domain::kPremiumMaxAccounts) {
		std::fprintf(
			stderr,
			"Mini-app open regression: account-limit fixture has no slot for "
			"the overflow account: before=%d after_cleanup=%d max=%d.\n",
			int(accountsBeforeRedundantCleanup),
			int(accountsAfterRedundantCleanup),
			Main::Domain::kPremiumMaxAccounts);
		return false;
	}
	const auto overflowAccount = app.domain().add(MTP::Environment::Production);
	overflowAccount->mtp().stopForServerEnrollment();
	const auto orderedBeforeReplacementLogout = app.domain().orderedAccounts();
	const auto overflowAccountPosition
		= ranges::find(orderedBeforeReplacementLogout, overflowAccount);
	const auto overflowAccountIndex = int(std::distance(
		begin(orderedBeforeReplacementLogout), overflowAccountPosition));
	if ((overflowAccountPosition == end(orderedBeforeReplacementLogout))
		|| (overflowAccountIndex < app.domain().maxAccounts())
		|| overflowAccount->sessionExists()
		|| app.separateWindowFor(overflowAccount)) {
		std::fprintf(
			stderr,
			"Mini-app open regression: account-limit fixture did not place a "
			"sessionless account beyond the limit: accounts=%d max=%d "
			"index=%d session=%d window=%d.\n",
			int(orderedBeforeReplacementLogout.size()),
			app.domain().maxAccounts(), overflowAccountIndex,
			overflowAccount->sessionExists(),
			app.separateWindowFor(overflowAccount) != nullptr);
		return false;
	}
	app.setActivePrimaryWindow(disposablePrimaryWindow);
	if (app.activePrimaryWindow() != disposablePrimaryWindow) {
		std::fprintf(stderr, "Mini-app open regression: disposable primary "
							 "window did not become "
							 "the active primary window.\n");
		return false;
	}
	app.domain().activate(overflowAccount);
	if (&app.domain().active() != overflowAccount.get()) {
		std::fprintf(
			stderr,
			"Mini-app open regression: sessionless account beyond the account "
			"limit did not become active.\n");
		return false;
	}
	replacement->forcedLogOut();
	restoreAccountLimitFixture();
	const auto fixtureControllersSurvived = !stockControllerLifetime.empty()
											&& !pinnedControllerLifetime.empty()
											&& !blankControllerLifetime.empty();
	if (!fixtureControllersSurvived) {
		std::fprintf(
			stderr,
			"Mini-app open regression: account-limit cleanup destroyed an "
			"existing fixture controller: stock=%d pinned=%d blocked=%d.\n",
			!stockControllerLifetime.empty(), !pinnedControllerLifetime.empty(),
			!blankControllerLifetime.empty());
		return false;
	}
	std::fprintf(
		stderr,
		"Mini-app open regression passed: %d entry classes refused on "
		"custom-pinned, blocked-without-pin, and custom-plus-blocked "
		"sessions with the toast as "
		"the only effect, a foreign window open refused, the stock app "
		"kept and activated, the chat opened before the refusal, and every "
		"deferred completion refused after its window or session was "
		"destroyed.\n",
		kRegressionEntryClassCount);
	return true;
}

[[nodiscard]] int FailChatParticipantsRegression(const char *reason) {
	std::fprintf(stderr, "Chat participants regression failed: %s\n", reason);
	return 1;
}

[[nodiscard]] bool BasicGroupAdminRightsMatch(not_null<ChatData *> chat,
											  const char *scenario,
											  bool moderationAllowed) {
	const auto hasAdminRights = chat->hasAdminRights();
	const auto canEditPermissions = chat->canEditPermissions();
	const auto canDeleteMessages = chat->canDeleteMessages();
	const auto canBanMembers = chat->canBanMembers();
	const auto canHaveInviteLink = chat->canHaveInviteLink();
	const auto canAddAdmins = chat->canAddAdmins();
	const auto matches = (hasAdminRights == moderationAllowed)
						 && (canEditPermissions == moderationAllowed)
						 && (canDeleteMessages == moderationAllowed)
						 && (canBanMembers == moderationAllowed)
						 && (canHaveInviteLink == moderationAllowed)
						 && !canAddAdmins;
	if (!matches) {
		std::fprintf(
			stderr,
			"Basic-group admin rights mismatch: scenario=%s "
			"hasAdminRights=%d canEditPermissions=%d canDeleteMessages=%d "
			"canBanMembers=%d canHaveInviteLink=%d canAddAdmins=%d\n",
			scenario, hasAdminRights, canEditPermissions, canDeleteMessages,
			canBanMembers, canHaveInviteLink, canAddAdmins);
	}
	return matches;
}

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
[[nodiscard]] ChatAdminRights OverbroadBasicGroupAdminRightsForRegression() {
	using Flag = ChatAdminRight;
	return Flag::Other | Flag::ChangeInfo | Flag::DeleteMessages
		   | Flag::BanUsers | Flag::InviteByLinkOrAdd | Flag::PinMessages
		   | Flag::ManageCall;
}

[[nodiscard]] HistoryItem *
AddTranslationRegressionMessage(not_null<Main::Session *> session,
								not_null<ChannelData *> channel, MsgId id,
								bool rich) {
	const auto data = MTP_message(
		MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_post),
		MTP_int(id), peerToMTP(session->userPeerId()),
		MTPint(),	 // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(channel->id),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTP_long(0), // via_bot_id
		MTPlong(),	 // via_business_bot_id
		MTPPeer(),	 // guestchat_via_from
		MTPMessageReplyHeader(),
		MTP_int(1), // date
		MTP_string(rich ? u"rich page summary"_q : u"bonjour tout le monde"_q),
		MTP_messageMediaEmpty(), MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0), // views
		MTP_int(0), // forwards
		MTPMessageReplies(),
		MTP_int(0), // edit_date
		MTPstring(), MTPlong(), MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTP_int(0), // ttl_period
		MTPint(),	// quick_reply_shortcut_id
		MTPlong(),	// effect
		MTPFactCheck(),
		MTPint(),  // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(),	 // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
	auto item = session->data().addNewMessage(data, MessageFlags(),
											  NewMessageType::Existing);
	if (item && rich) {
		item->setRichPage(std::make_shared<Iv::RichPage>());
	}
	return item;
}

[[nodiscard]] MTPChat TranslationRegressionChannel(ChannelId id,
												   uint64 accessHash,
												   const QString &title) {
	using Flag = MTPDchannel::Flag;
	return MTP_channel(
		MTP_flags(Flag::f_creator | Flag::f_broadcast | Flag::f_access_hash
				  | Flag::f_autotranslation),
		MTP_long(id.bare), MTP_long(accessHash), MTP_string(title), MTPstring(),
		MTP_chatPhotoEmpty(), MTP_int(1), MTPVector<MTPRestrictionReason>(),
		MTPChatAdminRights(), MTPChatBannedRights(), MTPChatBannedRights(),
		MTPint(), MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(),
		MTPPeerColor(), MTPEmojiStatus(), MTPint(), MTPint(), MTPlong(),
		MTPlong(), MTPlong(), MTPlong());
}

[[nodiscard]] MTPDchannelFull TranslationRegressionChannelFull(ChannelId id) {
	const auto notifySettings = MTP_peerNotifySettings(
		MTP_flags(MTPDpeerNotifySettings::Flags()), MTP_boolFalse(),
		MTP_boolFalse(), MTP_int(0), MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(), MTP_notificationSoundDefault(),
		MTP_boolFalse(), MTP_boolFalse(), MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(), MTP_notificationSoundDefault());
	return MTPDchannelFull(
		MTP_flags(MTPDchannelFull::Flags()), MTP_long(id.bare),
		MTP_string(QString()), MTPint(), MTPint(), MTPint(), MTPint(), MTPint(),
		MTP_int(0), MTP_int(0), MTP_int(0), MTP_photoEmpty(MTP_long(0)),
		notifySettings, MTPExportedChatInvite(), MTP_vector<MTPBotInfo>(0),
		MTPlong(), MTPint(), MTPint(), MTPStickerSet(), MTPint(), MTPint(),
		MTPlong(), MTPChannelLocation(), MTPint(), MTPint(), MTPint(),
		MTP_int(0), MTPInputGroupCall(), MTPint(), MTP_vector<MTPstring>(0),
		MTPPeer(), MTPstring(), MTPint(), MTP_vector<MTPlong>(0), MTPPeer(),
		MTP_chatReactionsNone(), MTPint(), MTPPeerStories(), MTPWallPaper(),
		MTPint(), MTPint(), MTPStickerSet(), MTPBotVerification(), MTPint(),
		MTPlong(), MTPProfileTab(), MTPlong());
}

[[nodiscard]] bool RunTranslationControlsLifecycleRegression(
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> customController) {
	auto &settings = Core::App().settings();
	const auto previousTranslateChat = settings.translateChatEnabled();
	const auto previousTranslateButton = settings.translateButtonEnabled();
	const auto previousPlatformTranslate = settings.usePlatformTranslation();
	const auto previousTranslateTo = settings.translateTo();
	const auto previousSkipLanguages = settings.skipTranslationLanguages();
	const auto restoreSettings = gsl::finally([&] {
		settings.setTranslateChatEnabled(previousTranslateChat);
		settings.setTranslateButtonEnabled(previousTranslateButton);
		settings.setUsePlatformTranslation(previousPlatformTranslate);
		settings.setTranslateTo(previousTranslateTo);
		settings.setSkipTranslationLanguages(previousSkipLanguages);
	});
	const auto target = LanguageId::FromName(Lang::DefaultLanguageId());
	const auto offered = LanguageId::FromName(u"French"_q);
	if (!target.known() || !offered.known()) {
		std::fprintf(stderr, "Translation controls regression failed: language "
							 "fixtures are unavailable.\n");
		return false;
	}
	settings.setTranslateChatEnabled(true);
	settings.setTranslateButtonEnabled(true);
	settings.setUsePlatformTranslation(false);
	settings.setTranslateTo(target);
	settings.setSkipTranslationLanguages({offered});

	auto &stockSession = stockController->session();
	auto &customSession = customController->session();
	const auto customProviderWithPlatformOff
		= Ui::TranslateProviderChoiceForSession(&customSession);
	const auto stockProviderWithPlatformOff
		= Ui::TranslateProviderChoiceForSession(&stockSession);
	settings.setUsePlatformTranslation(true);
	const auto stockProviderWithPlatformOn
		= Ui::TranslateProviderChoiceForSession(&stockSession);
	const auto customProviderAfterStockChange
		= Ui::TranslateProviderChoiceForSession(&customSession);
	settings.setUsePlatformTranslation(false);
	const auto customProviderAfterPreferenceReset
		= Ui::TranslateProviderChoiceForSession(&customSession);
	const auto stockProviderAfterCustom
		= Ui::TranslateProviderChoiceForSession(&stockSession);
	if (customProviderWithPlatformOff != customProviderAfterStockChange
		|| customProviderWithPlatformOff != customProviderAfterPreferenceReset
		|| stockProviderWithPlatformOff
			   == Ui::details::TranslateProviderChoice::Unavailable
		|| stockProviderWithPlatformOn
			   == Ui::details::TranslateProviderChoice::Unavailable
		|| stockProviderAfterCustom
			   == Ui::details::TranslateProviderChoice::Unavailable
		|| customProviderWithPlatformOff
			   == Ui::details::TranslateProviderChoice::MTProto) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: provider choice leaked "
			"the stock window preference into the custom session.\n");
		return false;
	}
	const auto text = tr::marked(u"Доброго дня"_q);
	const auto customHasProvider
		= (customProviderWithPlatformOff
		   != Ui::details::TranslateProviderChoice::Unavailable);
	const auto customPlainHidden = Ui::SkipTranslate(&customSession, text);
	const auto customRichHidden = Ui::SkipTranslate(&customSession, text, true);
	const auto stockPlainHidden = Ui::SkipTranslate(&stockSession, text);
	if ((customProviderWithPlatformOff
			 == Ui::details::TranslateProviderChoice::Unavailable
		 && !customPlainHidden)
		|| (customHasProvider && customPlainHidden)
		|| customProviderWithPlatformOff
			   == Ui::details::TranslateProviderChoice::MTProto
		|| !customRichHidden || stockPlainHidden) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: custom translation "
			"actions were exposed.\n");
		return false;
	}
	auto localFailure = false;
	if (customProviderWithPlatformOff
		== Ui::details::TranslateProviderChoice::Unavailable) {
		auto provider = Ui::CreateTranslateProvider(&customSession);
		provider->request(Ui::TranslateProviderRequest{.text = text}, target,
						  [&](Ui::TranslateProviderResult result) {
							  localFailure
								  = !result.text
									&& (result.error
										== Ui::TranslateProviderError::Unknown);
						  });
		if (!localFailure) {
			std::fprintf(stderr,
						 "Translation controls regression failed: stale custom "
						 "translation did not fail locally.\n");
			return false;
		}
	}
	std::fprintf(
		stderr,
		"Translation action routing: custom_provider=%d custom_hidden=%d "
		"custom_rich_hidden=%d local_failure=%d stock_provider=%d "
		"stock_hidden=%d.\n",
		int(customProviderWithPlatformOff), customPlainHidden, customRichHidden,
		localFailure, int(stockProviderWithPlatformOff), stockPlainHidden);
	stockSession.data().processChat(TranslationRegressionChannel(
		ChannelId(1151), 115100, u"Stock channel"_q));
	customSession.data().processChat(TranslationRegressionChannel(
		ChannelId(1152), 115100, u"Custom channel"_q));
	const auto stockChannel = stockSession.data().channel(ChannelId(1151));
	const auto customChannel = customSession.data().channel(ChannelId(1152));
	Data::ApplyChannelUpdate(stockChannel,
							 TranslationRegressionChannelFull(ChannelId(1151)));
	Data::ApplyChannelUpdate(customChannel,
							 TranslationRegressionChannelFull(ChannelId(1152)));

	const auto openEditBox
		= [](not_null<Window::SessionController *> controller,
			 not_null<ChannelData *> channel) {
			  return controller->window().show(
				  object_ptr<EditPeerInfoBox>(controller->widget().get(),
											  controller, channel),
				  Ui::LayerOption::KeepOther, anim::type::normal);
		  };
	ResetTranslationRpcCountsForRegressionTest();
	auto customEditBox = openEditBox(customController, customChannel);
	QCoreApplication::processEvents();
	if (!customEditBox
		|| customEditBox->autoTranslateControlPresentForRegressionTest()) {
		std::fprintf(stderr, "Translation controls regression failed: custom "
							 "channel exposed Auto-translate.\n");
		return false;
	}
	const auto renamedChannel = u"Renamed custom channel"_q;
	auto channelSaveRequests = ChannelSaveRequestCapture{
		.instance = &customController->session().mtp(),
	};
	const auto restoreChannelSaveCapture
		= gsl::finally([previous = std::exchange(gChannelSaveRequestCapture,
												 &channelSaveRequests)] {
			  gChannelSaveRequestCapture = previous;
		  });
	customEditBox->saveRenameForRegressionTest(renamedChannel);
	if (!customEditBox || customChannel->name() != u"Custom channel"_q) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: custom channel rename "
			"completed before its server response.\n");
		return false;
	}
	const auto expectedChannel
		= MTP_inputChannel(MTP_long(1152), MTP_long(115100));
	if (!ReplyToChannelSaveRequestForRegressionTest(
			channelSaveRequests, 0,
			MTPchannels_DeactivateAllUsernames(expectedChannel),
			MTP_boolTrue())) {
		return false;
	}
	using ChannelFlag = MTPDchannel::Flag;
	const auto renamedChat = MTP_channel(
		MTP_flags(ChannelFlag::f_creator | ChannelFlag::f_broadcast
				  | ChannelFlag::f_access_hash
				  | ChannelFlag::f_autotranslation),
		MTP_long(1152), MTP_long(115100), MTP_string(renamedChannel),
		MTPstring(), MTP_chatPhotoEmpty(), MTP_int(1),
		MTPVector<MTPRestrictionReason>(), MTPChatAdminRights(),
		MTPChatBannedRights(), MTPChatBannedRights(), MTPint(),
		MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(),
		MTPPeerColor(), MTPEmojiStatus(), MTPint(), MTPint(), MTPlong(),
		MTPlong(), MTPlong(), MTPlong());
	const auto renameReply = MTP_updates(
		MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>({renamedChat}), MTP_int(1), MTP_int(0));
	if (!ReplyToChannelSaveRequestForRegressionTest(
			channelSaveRequests, 1,
			MTPchannels_EditTitle(expectedChannel, MTP_string(renamedChannel)),
			renameReply)) {
		return false;
	}
	const auto saveCompleted
		= WaitForRegressionCondition([&] { return !customEditBox; });
	const auto namePersisted = (customChannel->name() == renamedChannel);
	const auto toggles = GetTranslationRpcCountsForRegressionTest()
							 .channelAutoTranslationToggle;
	if (!saveCompleted || !namePersisted
		|| channelSaveRequests.requests.size() != 2 || toggles != 0) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: custom channel Save "
			"completed=%d persisted=%d requests=%zu toggle RPCs=%d.\n",
			saveCompleted, namePersisted, channelSaveRequests.requests.size(),
			toggles);
		return false;
	}
	customEditBox = openEditBox(customController, customChannel);
	QCoreApplication::processEvents();
	if (!customEditBox
		|| customEditBox->titleForRegressionTest() != renamedChannel
		|| customEditBox->autoTranslateControlPresentForRegressionTest()) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: reopening the saved "
			"custom channel lost its name or exposed Auto-translate.\n");
		return false;
	}
	customEditBox->saveAutoTranslateForRegressionTest(false);
	if (GetTranslationRpcCountsForRegressionTest().channelAutoTranslationToggle
			!= 0
		|| channelSaveRequests.requests.size() != 2) {
		std::fprintf(stderr,
					 "Translation controls regression failed: reopened custom "
					 "channel attempted an Auto-translate toggle.\n");
		return false;
	}
	customController->hideLayer(anim::type::instant);

	ResetTranslationRpcCountsForRegressionTest();
	auto stockEditBox = openEditBox(stockController, stockChannel);
	QCoreApplication::processEvents();
	if (!stockEditBox
		|| !stockEditBox->autoTranslateControlPresentForRegressionTest()) {
		std::fprintf(stderr, "Translation controls regression failed: stock "
							 "channel lost Auto-translate.\n");
		return false;
	}
	stockEditBox->saveAutoTranslateForRegressionTest(false);
	if (GetTranslationRpcCountsForRegressionTest().channelAutoTranslationToggle
		!= 1) {
		std::fprintf(
			stderr, "Translation controls regression failed: stock channel did "
					"not attempt its Auto-translate toggle.\n");
		return false;
	}
	stockController->hideLayer(anim::type::instant);

	const auto openLanguageBox
		= [](not_null<Window::SessionController *> controller,
			 const QString &highlightId) {
			  if (!highlightId.isEmpty()) {
				  controller->setHighlightControlId(highlightId);
			  }
			  return controller->window().show(
				  object_ptr<LanguageBox>(controller->widget().get(),
										  controller, highlightId),
				  Ui::LayerOption::KeepOther, anim::type::instant);
		  };
	const auto stockHasPlatformProvider
		= Platform::IsTranslateProviderAvailable();
	const auto settingsSkipLanguages = settings.skipTranslationLanguages();
	auto customLanguageBox
		= openLanguageBox(customController, u"language/show-button"_q);
	QCoreApplication::processEvents();
	const auto customHighlightConsumed
		= customLanguageBox && WaitForRegressionCondition([&] {
			  return customController->highlightControlId().isEmpty();
		  });
	if (!customLanguageBox || !customHighlightConsumed
		|| customLanguageBox->translateButtonRowPresentForRegressionTest()
			   != customHasProvider
		|| customLanguageBox->doNotTranslateRowPresentForRegressionTest()
			   != customHasProvider
		|| customLanguageBox->platformTranslationRowPresentForRegressionTest()
		|| customLanguageBox->translationAboutPresentForRegressionTest()
			   != customHasProvider
		|| customLanguageBox->platformPackNotePresentForRegressionTest()
			   != (customProviderWithPlatformOff
					   == Ui::details::TranslateProviderChoice::Platform
				   && Platform::IsMac())
		|| customLanguageBox->translateChatsRowPresentForRegressionTest()
		|| !customController->highlightControlId().isEmpty()) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: custom Language settings "
			"box=%d highlight_consumed=%d highlight_pending=%d "
			"button=%d/%d skip=%d/%d platform=%d about=%d/%d "
			"pack_note=%d/%d chats=%d.\n",
			bool(customLanguageBox), customHighlightConsumed,
			!customController->highlightControlId().isEmpty(),
			customLanguageBox
				? customLanguageBox
					  ->translateButtonRowPresentForRegressionTest()
				: false,
			customHasProvider,
			customLanguageBox
				? customLanguageBox->doNotTranslateRowPresentForRegressionTest()
				: false,
			customHasProvider,
			customLanguageBox
				? customLanguageBox
					  ->platformTranslationRowPresentForRegressionTest()
				: false,
			customLanguageBox
				? customLanguageBox->translationAboutPresentForRegressionTest()
				: false,
			customHasProvider,
			customLanguageBox
				? customLanguageBox->platformPackNotePresentForRegressionTest()
				: false,
			(customProviderWithPlatformOff
				 == Ui::details::TranslateProviderChoice::Platform
			 && Platform::IsMac()),
			customLanguageBox
				? customLanguageBox->translateChatsRowPresentForRegressionTest()
				: false);
		return false;
	}
	customController->hideLayer(anim::type::instant);
	for (const auto &link :
		 {u"language/show-button"_q, u"language/translate-chats"_q,
		  u"language/do-not-translate"_q}) {
		auto linkBox = openLanguageBox(customController, link);
		QCoreApplication::processEvents();
		const auto highlightConsumed
			= linkBox && WaitForRegressionCondition([&] {
				  return customController->highlightControlId().isEmpty();
			  });
		if (!linkBox || !highlightConsumed) {
			std::fprintf(
				stderr, "Translation controls regression failed: Language link "
						"left an unsupported highlight pending.\n");
			return false;
		}
		customController->hideLayer(anim::type::instant);
	}
	auto stockLanguageBox = openLanguageBox(stockController, QString());
	QCoreApplication::processEvents();
	if (!stockLanguageBox
		|| !stockLanguageBox->translateButtonRowPresentForRegressionTest()
		|| !stockLanguageBox->doNotTranslateRowPresentForRegressionTest()
		|| !stockLanguageBox->translationAboutPresentForRegressionTest()
		|| (stockLanguageBox->platformTranslationRowPresentForRegressionTest()
			!= stockHasPlatformProvider)
		|| (stockLanguageBox->platformPackNotePresentForRegressionTest()
			!= (stockHasPlatformProvider && Platform::IsMac()))
		|| !stockLanguageBox->translateChatsRowPresentForRegressionTest()) {
		std::fprintf(stderr, "Translation controls regression failed: stock "
							 "Language settings lost supported rows.\n");
		return false;
	}
	stockController->hideLayer(anim::type::instant);
	if (!settings.translateButtonEnabled() || settings.usePlatformTranslation()
		|| settings.translateChatEnabled() != true
		|| settings.skipTranslationLanguages() != settingsSkipLanguages) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: displaying Language "
			"settings wrote shared translation preferences.\n");
		return false;
	}

	const auto stockLoaded = stockChannel->isLoaded();
	const auto stockServerSupported = stockSession.serverTranslationSupported();
	const auto stockNetworkAllowed
		= stockSession.mtp().isServerEnrollmentNetworkAllowed();
	const auto stockCreator = stockChannel->amCreator();
	const auto stockBroadcast = stockChannel->isBroadcast();
	const auto stockAutoTranslation = stockChannel->autoTranslation();
	const auto stockTranslationEnabled
		= (stockChannel->translationFlag()
		   == PeerData::TranslationFlag::Enabled);
	const auto customLoaded = customChannel->isLoaded();
	const auto customServerSupported
		= customSession.serverTranslationSupported();
	const auto customNetworkAllowed
		= customSession.mtp().isServerEnrollmentNetworkAllowed();
	const auto customCreator = customChannel->amCreator();
	const auto customBroadcast = customChannel->isBroadcast();
	const auto customAutoTranslation = customChannel->autoTranslation();
	const auto customTranslationEnabled
		= (customChannel->translationFlag()
		   == PeerData::TranslationFlag::Enabled);
	std::fprintf(
		stderr,
		"Translation controls precondition: stock loaded=%d "
		"server_supported=%d network_allowed=%d creator=%d broadcast=%d "
		"auto_translation=%d translation_enabled=%d; custom loaded=%d "
		"server_supported=%d network_allowed=%d creator=%d broadcast=%d "
		"auto_translation=%d translation_enabled=%d.\n",
		stockLoaded, stockServerSupported, stockNetworkAllowed, stockCreator,
		stockBroadcast, stockAutoTranslation, stockTranslationEnabled,
		customLoaded, customServerSupported, customNetworkAllowed,
		customCreator, customBroadcast, customAutoTranslation,
		customTranslationEnabled);
	if (!stockLoaded || !stockServerSupported || stockNetworkAllowed
		|| !stockCreator || !stockBroadcast || !stockAutoTranslation
		|| !stockTranslationEnabled || !customLoaded || customServerSupported
		|| customNetworkAllowed || !customCreator || !customBroadcast
		|| !customAutoTranslation || !customTranslationEnabled) {
		return false;
	}

	const auto stockHistory
		= stockController->session().data().history(stockChannel);
	const auto customHistory
		= customController->session().data().history(customChannel);
	customHistory->translateOfferFrom(offered);
	{
		const auto customTracker = HistoryView::TranslateTracker(customHistory);
		if (customTracker.trackingLanguageForRegressionTest()) {
			std::fprintf(stderr,
						 "Translation controls regression failed: custom "
						 "session started automatic translation tracking.\n");
			return false;
		}
		HistoryView::TranslateBar bar(customController->widget().get(),
									  customController, customHistory);
		bar.finishAnimating();
		if (bar.shouldBeShownForRegressionTest()) {
			std::fprintf(stderr,
						 "Translation controls regression failed: custom "
						 "session showed the automatic translation bar.\n");
			return false;
		}
	}
	{
		const auto stockTracker = HistoryView::TranslateTracker(stockHistory);
		if (!stockTracker.trackingLanguageForRegressionTest()) {
			std::fprintf(stderr,
						 "Translation controls regression failed: stock "
						 "session stopped automatic translation tracking.\n");
			return false;
		}
		stockHistory->translateOfferFrom(offered);
		HistoryView::TranslateBar bar(stockController->widget().get(),
									  stockController, stockHistory);
		bar.finishAnimating();
		if (!bar.shouldBeShownForRegressionTest()) {
			std::fprintf(stderr,
						 "Translation controls regression failed: stock "
						 "session lost its automatic translation bar.\n");
			return false;
		}
	}
	const auto stockTextItem = AddTranslationRegressionMessage(
		&stockController->session(), stockChannel, MsgId(7101), false);
	const auto stockRichItem = AddTranslationRegressionMessage(
		&stockController->session(), stockChannel, MsgId(7102), true);
	const auto customTextItem = AddTranslationRegressionMessage(
		&customController->session(), customChannel, MsgId(7201), false);
	const auto customRichItem = AddTranslationRegressionMessage(
		&customController->session(), customChannel, MsgId(7202), true);
	if (!stockTextItem || !stockRichItem || !customTextItem
		|| !customRichItem) {
		std::fprintf(stderr,
					 "Translation controls regression failed: translation "
					 "message fixtures were not created.\n");
		return false;
	}
	const auto stockTextRegistered
		= (stockSession.data().message(stockTextItem->fullId())
		   == stockTextItem);
	const auto stockRichRegistered
		= (stockSession.data().message(stockRichItem->fullId())
		   == stockRichItem);
	const auto customTextRegistered
		= (customSession.data().message(customTextItem->fullId())
		   == customTextItem);
	const auto customRichRegistered
		= (customSession.data().message(customRichItem->fullId())
		   == customRichItem);
	if (!stockTextRegistered || !stockRichRegistered || !customTextRegistered
		|| !customRichRegistered) {
		std::fprintf(stderr, "Translation controls regression failed: "
							 "synthetic message fixtures "
							 "were not registered for message lookup.\n");
		return false;
	}
	auto customAdminLogProvider
		= Ui::details::TranslateProviderChoice::Unavailable;
	auto customAdminLogRichTranslateVisible = true;
	{
		auto &translateUrl
			= base::options::lookup<QString>("translate-url-template");
		const auto previousTranslateUrl = translateUrl.value();
		const auto restoreTranslateUrl
			= gsl::finally([&] { translateUrl.set(previousTranslateUrl); });
		translateUrl.set(u"https://translation.example/?q=%q"_q);
		customAdminLogProvider
			= Ui::TranslateProviderChoiceForSession(&customSession);
		customAdminLogRichTranslateVisible
			= AdminLog::ShouldShowTranslateAction(customRichItem);
	}
	if (customAdminLogProvider != Ui::details::TranslateProviderChoice::Url
		|| !customRichItem->richPage() || customAdminLogRichTranslateVisible) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: admin-log rich action "
			"visibility did not match (provider=%d visible=%d).\n",
			int(customAdminLogProvider), customAdminLogRichTranslateVisible);
		return false;
	}
	auto staleRichBox = customController->show(
		Box(Ui::TranslateBox, customRichItem->history()->peer,
			customRichItem->id, customRichItem->originalText(), false),
		Ui::LayerOption::KeepOther, anim::type::normal);
	if (!WaitForRegressionCondition([&] { return !staleRichBox; })) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: a stale custom rich "
			"translation action did not close locally.\n");
		return false;
	}

	gTranslationRpcRoleForRegressionTest
		= TranslationRpcRoleForRegressionTest::Custom;
	ResetTranslationRpcCountsForRegressionTest();
	{
		HistoryView::TranslateTracker textTracker(customHistory);
		textTracker.requestTextForRegressionTest(customTextItem, target);
		HistoryView::TranslateTracker richTracker(customHistory);
		richTracker.requestRichForRegressionTest(customRichItem, target);
	}
	const auto customRpcs = GetTranslationRpcCountsForRegressionTest();
	if (customRpcs.text != 0 || customRpcs.rich != 0) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: custom session sent "
			"translation RPCs (text=%d rich=%d).\n",
			customRpcs.text, customRpcs.rich);
		return false;
	}
	std::fprintf(stderr,
				 "Translation controls custom requests: text=%d rich=%d.\n",
				 customRpcs.text, customRpcs.rich);
	gTranslationRpcRoleForRegressionTest
		= TranslationRpcRoleForRegressionTest::Stock;
	ResetTranslationRpcCountsForRegressionTest();
	{
		HistoryView::TranslateTracker textTracker(stockHistory);
		textTracker.requestTextForRegressionTest(stockTextItem, target);
		HistoryView::TranslateTracker richTracker(stockHistory);
		richTracker.requestRichForRegressionTest(stockRichItem, target);
	}
	const auto stockRpcs = GetTranslationRpcCountsForRegressionTest();
	if (stockRpcs.text != 1 || stockRpcs.rich != 1) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: stock translation "
			"RPCs were not sent (text=%d rich=%d).\n",
			stockRpcs.text, stockRpcs.rich);
		return false;
	}
	std::fprintf(stderr,
				 "Translation controls stock requests: text=%d rich=%d.\n",
				 stockRpcs.text, stockRpcs.rich);
	const auto barrier = WaitForMainQueueBarrierForRegressionTest(1000)
						 == MainQueueBarrierResult::Completed;
	const auto stockAfterBarrier = GetTranslationRpcCountsForRegressionTest();
	const auto stockNetworkAllowedAfter
		= stockSession.mtp().isServerEnrollmentNetworkAllowed();
	const auto customNetworkAllowedAfter
		= customSession.mtp().isServerEnrollmentNetworkAllowed();
	const auto customCountsUnchanged
		= (gCustomTranslationRpcCounts.text == customRpcs.text)
		  && (gCustomTranslationRpcCounts.rich == customRpcs.rich);
	const auto stockRoleCountsUnchanged
		= (gStockTranslationRpcCounts.text == stockRpcs.text)
		  && (gStockTranslationRpcCounts.rich == stockRpcs.rich);
	const auto stockCountsUnchanged
		= (stockAfterBarrier.text == stockRpcs.text)
		  && (stockAfterBarrier.rich == stockRpcs.rich);
	if (!barrier || !customCountsUnchanged || !stockRoleCountsUnchanged
		|| !stockCountsUnchanged || stockNetworkAllowedAfter
		|| customNetworkAllowedAfter) {
		std::fprintf(
			stderr,
			"Translation controls regression failed: barrier=%d custom "
			"text=%d rich=%d stock text=%d rich=%d network_allowed "
			"stock=%d custom=%d.\n",
			barrier, gCustomTranslationRpcCounts.text,
			gCustomTranslationRpcCounts.rich, gStockTranslationRpcCounts.text,
			gStockTranslationRpcCounts.rich, stockNetworkAllowedAfter,
			customNetworkAllowedAfter);
		return false;
	}
	std::fprintf(
		stderr,
		"Translation controls counts unchanged after 1000 ms main-queue "
		"barrier: custom text=%d rich=%d; stock text=%d rich=%d; "
		"network_allowed stock=%d custom=%d.\n",
		gCustomTranslationRpcCounts.text, gCustomTranslationRpcCounts.rich,
		gStockTranslationRpcCounts.text, gStockTranslationRpcCounts.rich,
		stockNetworkAllowedAfter, customNetworkAllowedAfter);
	gTranslationRpcRoleForRegressionTest
		= TranslationRpcRoleForRegressionTest::None;
	std::fprintf(
		stderr,
		"Translation controls regression passed: custom action gates followed "
		"session provider availability, rich actions stayed hidden, stale "
		"actions failed locally, Language links skipped absent rows without "
		"writing preferences, automatic chat translation stayed off, and "
		"stock controls and RPCs remained enabled.\n");
	return true;
}

[[nodiscard]] bool RunManageControlsLifecycleRegression(
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> customController) {
	const auto stockChannel
		= stockController->session().data().channel(ChannelId(1161));
	const auto customChannel
		= customController->session().data().channel(ChannelId(1162));
	const auto configureChannel
		= [](not_null<ChannelData *> channel, const QString &name) {
			  channel->setAccessHash(channel->id.value * 100);
			  channel->setName(name, QString());
			  channel->setFlags(ChannelDataFlag::Creator
								| ChannelDataFlag::Broadcast);
		  };
	configureChannel(stockChannel, u"Stock manage channel"_q);
	configureChannel(customChannel, u"Custom manage channel"_q);
	const auto stockGroup
		= stockController->session().data().chat(ChatId(1163));
	const auto customGroup
		= customController->session().data().chat(ChatId(1164));
	stockGroup->setName(u"Stock manage group"_q);
	stockGroup->setFlags(ChatDataFlag::Creator);
	customGroup->setName(u"Custom manage group"_q);
	customGroup->setFlags(ChatDataFlag::Creator);
	const auto customDiscussion
		= customController->session().data().channel(ChannelId(1165));
	customDiscussion->setAccessHash(customDiscussion->id.value * 100);
	customDiscussion->setName(u"Custom discussion group"_q, QString());
	customDiscussion->setFlags(ChannelDataFlag::Creator
							   | ChannelDataFlag::Megagroup);
	const auto stockChannelPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*stockChannel));
	const auto customChannelPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*customChannel));
	const auto stockGroupPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*stockGroup));
	const auto customGroupPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*customGroup));

	const auto openManageBox
		= [](not_null<Window::SessionController *> controller,
			 not_null<PeerData *> peer) {
			  return controller->window().show(
				  object_ptr<EditPeerInfoBox>(controller->widget().get(),
											  controller, peer),
				  Ui::LayerOption::KeepOther, anim::type::normal);
		  };
	const auto channelRowsMatch
		= [&](EditPeerInfoBox *box, bool paidSupported) {
			  const auto rows = box->manageRowsForRegressionTest();
			  return rows.directMessages == paidSupported
					 && rows.reactions == paidSupported
					 && rows.appearance == paidSupported;
		  };
	for (const auto customFirst : {false, true}) {
		const auto firstController
			= customFirst ? customController : stockController;
		const auto secondController
			= customFirst ? stockController : customController;
		const auto firstPeer
			= customFirst ? customChannelPeer : stockChannelPeer;
		const auto secondPeer
			= customFirst ? stockChannelPeer : customChannelPeer;
		auto firstBox = openManageBox(firstController, firstPeer);
		QCoreApplication::processEvents();
		if (!firstBox || !channelRowsMatch(firstBox.get(), !customFirst)) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: case=channel_visibility_"
				"first_custom=%d.\n",
				customFirst);
			return false;
		}
		auto secondBox = openManageBox(secondController, secondPeer);
		QCoreApplication::processEvents();
		if (!secondBox || !channelRowsMatch(secondBox.get(), customFirst)) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: case=channel_visibility_"
				"second_custom=%d.\n",
				!customFirst);
			return false;
		}
		firstController->hideLayer(anim::type::instant);
		secondController->hideLayer(anim::type::instant);
	}
	std::fprintf(
		stderr,
		"Manage controls regression passed: cases=channel_visibility_"
		"separate_windows_stock_then_custom,channel_visibility_separate_"
		"windows_custom_then_stock\n");

	auto stockGroupBox = openManageBox(stockController, stockGroupPeer);
	auto customGroupBox = openManageBox(customController, customGroupPeer);
	QCoreApplication::processEvents();
	if (!stockGroupBox || !customGroupBox
		|| !stockGroupBox->manageRowsForRegressionTest().reactions
		|| customGroupBox->manageRowsForRegressionTest().reactions) {
		std::fprintf(stderr,
					 "Manage controls regression failed: case=group_reactions_"
					 "separate_windows.\n");
		return false;
	}
	stockController->hideLayer(anim::type::instant);
	customController->hideLayer(anim::type::instant);
	std::fprintf(
		stderr,
		"Manage controls regression passed: case=group_reactions_separate_"
		"windows.\n");
	{
		auto customStageBox
			= openManageBox(customController, customChannelPeer);
		if (!customStageBox
			|| customStageBox->manageRowsForRegressionTest().directMessages) {
			std::fprintf(stderr, "Manage controls regression failed: custom "
								 "paid row was present "
								 "for the stale-price save.\n");
			return false;
		}
		ChannelSaveRequestCapture capture{
			.instance = &customController->session().mtp(),
		};
		const auto restoreCapture = gsl::finally(
			[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
				gChannelSaveRequestCapture = previous;
			});
		customStageBox->saveDirectMessagesPriceStageForRegressionTest(42);
		if (!capture.requests.empty()) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: stale hidden price sent "
				"a paid request.\n");
			return false;
		}
		customController->hideLayer(anim::type::instant);
	}
	std::fprintf(stderr,
				 "Manage controls regression passed: case=custom_save_stage_"
				 "ignores_stale_nonzero_price.\n");

	{
		auto stockSaveBox = openManageBox(stockController, stockChannelPeer);
		if (!stockSaveBox
			|| !stockSaveBox->manageRowsForRegressionTest().directMessages) {
			std::fprintf(stderr, "Manage controls regression failed: stock "
								 "paid row was hidden.\n");
			return false;
		}
		ChannelSaveRequestCapture capture{
			.instance = &stockController->session().mtp(),
		};
		const auto restoreCapture = gsl::finally(
			[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
				gChannelSaveRequestCapture = previous;
			});
		stockSaveBox->saveDirectMessagesPriceForRegressionTest(42);
		const auto stockInput = stockChannel->inputChannel();
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 0, MTPchannels_DeactivateAllUsernames(stockInput),
				MTP_boolTrue())) {
			return false;
		}
		using PriceFlag = MTPchannels_UpdatePaidMessagesPrice::Flag;
		if (!FailChannelSaveRequestForRegressionTest(
				capture, 1,
				MTPchannels_UpdatePaidMessagesPrice(
					MTP_flags(PriceFlag::f_broadcast_messages_allowed),
					stockInput, MTP_long(42)),
				u"CHAT_NOT_MODIFIED"_q)) {
			return false;
		}
		if (!WaitForRegressionCondition([&] { return !stockSaveBox; })) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: stock price save did not "
				"finish.\n");
			return false;
		}
	}
	std::fprintf(stderr,
				 "Manage controls regression passed: case=stock_direct_message_"
				 "price_save.\n");

	const auto savedTitle = u"Manage saved title"_q;
	const auto savedDescription = u"Manage saved description"_q;
	{
		auto customSaveBox = openManageBox(customController, customChannelPeer);
		if (!customSaveBox) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: custom save box did not "
				"open.\n");
			return false;
		}
		ChannelSaveRequestCapture capture{
			.instance = &customController->session().mtp(),
		};
		const auto restoreCapture = gsl::finally(
			[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
				gChannelSaveRequestCapture = previous;
			});
		customSaveBox->saveManageChangesForRegressionTest(
			savedTitle, savedDescription, customDiscussion, 42);
		const auto customInput = customChannel->inputChannel();
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 0, MTPchannels_DeactivateAllUsernames(customInput),
				MTP_boolTrue())) {
			return false;
		}
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 1,
				MTPchannels_SetDiscussionGroup(
					customInput, customDiscussion->inputChannel()),
				MTP_boolTrue())) {
			return false;
		}
		using ChannelFlag = MTPDchannel::Flag;
		const auto titleUpdateChannel = MTP_channel(
			MTP_flags(ChannelFlag::f_creator | ChannelFlag::f_broadcast
					  | ChannelFlag::f_access_hash),
			MTP_long(customChannel->id.value),
			MTP_long(customChannel->id.value * 100), MTP_string(savedTitle),
			MTPstring(), MTP_chatPhotoEmpty(), MTP_int(1),
			MTPVector<MTPRestrictionReason>(), MTPChatAdminRights(),
			MTPChatBannedRights(), MTPChatBannedRights(), MTPint(),
			MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(),
			MTPPeerColor(), MTPEmojiStatus(), MTPint(), MTPint(), MTPlong(),
			MTPlong(), MTPlong(), MTPlong());
		const auto titleReply = MTP_updates(
			MTP_vector<MTPUpdate>(), MTP_vector<MTPUser>(),
			MTP_vector<MTPChat>({titleUpdateChannel}), MTP_int(1), MTP_int(0));
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 2,
				MTPchannels_EditTitle(customInput, MTP_string(savedTitle)),
				titleReply)) {
			return false;
		}
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 3,
				MTPmessages_EditChatAbout(customChannel->input(),
										  MTP_string(savedDescription)),
				MTP_boolTrue())) {
			return false;
		}
		if (!WaitForRegressionCondition([&] { return !customSaveBox; })
			|| capture.requests.size() != 4
			|| customChannel->name() != savedTitle
			|| customChannel->about() != savedDescription
			|| customChannel->discussionLink() != &*customDiscussion) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: custom supported edits "
				"did not save cleanly without a paid price request.\n");
			return false;
		}
	}
	auto reopenedCustomBox = openManageBox(customController, customChannelPeer);
	QCoreApplication::processEvents();
	if (!reopenedCustomBox || !channelRowsMatch(reopenedCustomBox.get(), false)
		|| reopenedCustomBox->titleForRegressionTest() != savedTitle
		|| reopenedCustomBox->descriptionForRegressionTest()
			   != savedDescription) {
		std::fprintf(
			stderr,
			"Manage controls regression failed: custom saved values did not "
			"reopen with paid rows hidden.\n");
		return false;
	}
	customController->hideLayer(anim::type::instant);
	std::fprintf(
		stderr, "Manage controls regression passed: case=custom_hidden_nonzero_"
				"price_omitted_title_description_link_saved.\n");

	{
		auto customErrorBox
			= openManageBox(customController, customChannelPeer);
		if (!customErrorBox) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: custom error box did not "
				"open.\n");
			return false;
		}
		ChannelSaveRequestCapture capture{
			.instance = &customController->session().mtp(),
		};
		const auto restoreCapture = gsl::finally(
			[previous = std::exchange(gChannelSaveRequestCapture, &capture)] {
				gChannelSaveRequestCapture = previous;
			});
		const auto rejectedDescription = u"Rejected description"_q;
		customErrorBox->saveDescriptionForRegressionTest(rejectedDescription,
														 42);
		const auto customInput = customChannel->inputChannel();
		if (!ReplyToChannelSaveRequestForRegressionTest(
				capture, 0, MTPchannels_DeactivateAllUsernames(customInput),
				MTP_boolTrue())) {
			return false;
		}
		if (!FailChannelSaveRequestForRegressionTest(
				capture, 1,
				MTPmessages_EditChatAbout(customChannel->input(),
										  MTP_string(rejectedDescription)),
				u"CHAT_ABOUT_INVALID"_q)) {
			return false;
		}
		QCoreApplication::processEvents();
		if (!customErrorBox
			|| !customErrorBox->descriptionErrorShownForRegressionTest()
			|| capture.requests.size() != 2) {
			std::fprintf(
				stderr,
				"Manage controls regression failed: supported description "
				"error was lost or the box closed.\n");
			return false;
		}
		customController->hideLayer(anim::type::instant);
	}
	std::fprintf(
		stderr, "Manage controls regression passed: case=custom_supported_edit_"
				"error_keeps_box_open.\n");
	return true;
}

// Returns an empty string when every entry is keyboard-selectable and
// accessible, otherwise the first sub-check that failed.
[[nodiscard]] QString
PopupMenuKeyboardAndAccessibilityFailure(Ui::PopupMenu &menu) {
	const auto &actions = menu.actions();
	if (actions.empty()) {
		return u"no-actions"_q;
	} else if (menu.accessibilityFocusPolicy() == Qt::NoFocus) {
		return u"accessibility-focus-policy"_q;
	}
	// The menu is never shown, so it stays wherever it was created. The first
	// keyboard step flushes the inner menu's pending move event, and the menu
	// re-reads the cursor position on every move, so the item under the
	// cursor is selected by mouse before the key selects it. Keep the cursor
	// off the menu.
	menu.move(QCursor::pos() + QPoint(1, 1));
	const auto inner = menu.menu();
	inner->clearSelection();
	auto hasAction = false;
	for (auto i = 0; i != actions.size(); ++i) {
		const auto action = actions[i];
		if (action->isSeparator()) {
			if (!i || (i + 1 == actions.size()) || actions[i - 1]->isSeparator()
				|| actions[i + 1]->isSeparator()) {
				return u"separator-position index=%1"_q.arg(i);
			}
		} else if (action->text().isEmpty()) {
			return u"empty-text index=%1"_q.arg(i);
		} else {
			const auto item = inner->itemForAction(action);
			const auto accessible
				= item ? QAccessible::queryAccessibleInterface(item) : nullptr;
			if (!accessible) {
				return u"accessibility-missing action=\"%1\""_q.arg(
					action->text());
			} else if (accessible->role() != QAccessible::MenuItem) {
				return u"accessibility-role role=%1 action=\"%2\""_q
					.arg(int(accessible->role()))
					.arg(action->text());
			} else if (accessible->text(QAccessible::Name) != action->text()) {
				return u"accessibility-name name=\"%1\" action=\"%2\""_q.arg(
					accessible->text(QAccessible::Name), action->text());
			}
			if (item->isEnabled()) {
				auto down
					= QKeyEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
				QCoreApplication::sendEvent(&menu, &down);
				const auto selected = inner->findSelectedAction();
				if (!selected || selected->action() != action
					|| selected->lastTriggeredSource()
						   != Ui::Menu::TriggeredSource::Keyboard) {
					return u"keyboard-selection action=\"%1\" selected=\"%2\" "
						   "source=%3"_q
							   .arg(action->text(),
									selected ? selected->action()->text()
											 : u"<none>"_q)
							   .arg(selected
										? int(selected->lastTriggeredSource())
										: -1);
				}
				hasAction = true;
			}
		}
	}
	const auto accessible = QAccessible::queryAccessibleInterface(&menu);
	if (!hasAction) {
		return u"keyboard-no-enabled-action"_q;
	} else if (!accessible) {
		return u"accessibility-menu-missing"_q;
	} else if (accessible->role() != QAccessible::PopupMenu) {
		return u"accessibility-menu-role role=%1"_q.arg(
			int(accessible->role()));
	}
	return QString();
}

[[nodiscard]] int PopupMenuDividerCount(const Ui::PopupMenu &menu) {
	const auto actions = menu.actions();
	return int(
		std::count_if(actions.cbegin(), actions.cend(),
					  [](QAction *action) { return action->isSeparator(); }));
}

template <typename Widget>
[[nodiscard]] bool RunScheduledSubmissionForWidget(
	Widget &widget, Main::Session &session, not_null<History *> history,
	bool supported, TimeId scheduled, TimeId repeat, const QString &label) {
	const auto text = u"Scheduled submission "_q + label;
	const auto count = session.scheduledMessages().count(history);
	widget.setDraftTextForRegressionTest(text);
	widget.sendForRegressionTest(Api::SendOptions{
		.scheduled = scheduled,
		.scheduleRepeatPeriod = repeat,
	});
	if (!supported) {
		return (session.scheduledMessages().count(history) == count)
			   && (widget.draftTextForRegressionTest() == text);
	} else if ((session.scheduledMessages().count(history) != count + 1)
			   || !widget.draftTextForRegressionTest().isEmpty()) {
		return false;
	}
	const auto messages = session.scheduledMessages().list(history);
	for (const auto id : messages.ids) {
		const auto item = session.scheduledMessages().lookupItem(id);
		if (item && (item->date() == scheduled)
			&& (item->scheduleRepeatPeriod() == repeat)
			&& (item->originalText().text == text)) {
			return true;
		}
	}
	return false;
}

template <typename Widget>
[[nodiscard]] bool RunOrdinarySubmissionForWidget(Widget &widget,
												  not_null<History *> history,
												  const QString &label) {
	const auto before = history->lastMessage();
	const auto text = u"Ordinary submission "_q + label;
	widget.setDraftTextForRegressionTest(text);
	widget.sendForRegressionTest(Api::SendOptions());
	const auto after = history->lastMessage();
	return after && (after != before) && (after->originalText().text == text)
		   && widget.draftTextForRegressionTest().isEmpty();
}

[[nodiscard]] bool RunScheduledSubmissionLifecycleRegression(
	not_null<Window::SessionController *> controller, bool supported,
	const QString &label) {
	auto &session = controller->session();
	const auto self
		= not_null<PeerData *>(static_cast<PeerData *>(&*session.user()));
	const auto history = session.data().history(self);
	controller->showPeerHistory(self);
	QCoreApplication::processEvents();
	const auto content = controller->content();
	const auto historyWidget = content->historyWidgetForRegressionTest();
	if (!historyWidget || (historyWidget->peer() != self.get())) {
		return false;
	}
	// A chat section needs a replies root or a sublist to list messages.
	// The self chat's own sublist sends into the same history.
	auto chatWidget = std::make_unique<HistoryView::ChatWidget>(
		content.get(), controller,
		HistoryView::ChatViewId{
			.history = history,
			.sublist = session.data().savedMessages().sublist(self),
		});
	const auto tomorrow = base::unixtime::now() + 24 * 60 * 60;
	const auto repeat = TimeId(7 * 24 * 60 * 60);
	const auto regularScheduled = RunScheduledSubmissionForWidget(
		*historyWidget, session, history, supported, tomorrow, 0,
		label + u" history"_q);
	const auto repeatedScheduled = RunScheduledSubmissionForWidget(
		*historyWidget, session, history, supported, tomorrow, repeat,
		label + u" history repeat"_q);
	const auto regularChatScheduled = RunScheduledSubmissionForWidget(
		*chatWidget, session, history, supported, tomorrow, 0,
		label + u" chat section"_q);
	const auto repeatedChatScheduled = RunScheduledSubmissionForWidget(
		*chatWidget, session, history, supported, tomorrow, repeat,
		label + u" chat section repeat"_q);
	const auto ordinaryHistory = RunOrdinarySubmissionForWidget(
		*historyWidget, history, label + u" history"_q);
	const auto ordinaryChat = RunOrdinarySubmissionForWidget(
		*chatWidget, history, label + u" chat section"_q);
	return regularScheduled && repeatedScheduled && regularChatScheduled
		   && repeatedChatScheduled && ordinaryHistory && ordinaryChat;
}

[[nodiscard]] bool RunChatFeatureMenusLifecycleRegression(
	not_null<Window::SessionController *> stockController,
	not_null<Window::SessionController *> customController,
	const QString &activationOrder) {
	struct DividerCounts {
		int history = 0;
		int profile = 0;
		int send = 0;
	};
	const auto checkController = [&](not_null<Window::SessionController *>
										 controller,
									 bool supported, const QString &label,
									 DividerCounts &dividers) {
		auto &session = controller->session();
		session.data().processUsers(MTP_vector<MTPUser>({
			RegressionUser(UserId(2), false, u"2"_q),
		}));
		const auto peer = session.data().peer(peerFromUser(UserId(2)));
		const auto history = session.data().history(peer);
		auto &cloudThemes = session.data().cloudThemes();
		cloudThemes.setChatThemesForRegressionTest({
			Data::CloudTheme{
				.id = 1,
				.emoticon = QString::fromUtf8("\xF0\x9F\x8F\xA0"),
			},
		});
		if (cloudThemes.chatThemes().empty()
			|| !cloudThemes.myGiftThemesReady()) {
			return false;
		}
		controller->showPeerHistory(peer);
		QCoreApplication::processEvents();
		const auto historyWidget
			= controller->content()->historyWidgetForRegressionTest();
		if (!historyWidget || (historyWidget->peer() != peer.get())) {
			return false;
		}
		const auto buildPeerMenu = [&](Dialogs::EntryState::Section section) {
			auto result = std::make_unique<Ui::PopupMenu>(nullptr);
			Window::FillDialogsEntryMenu(
				controller,
				{
					.key = history,
					.section = section,
				},
				Ui::Menu::CreateAddActionCallback(result.get()));
			return result;
		};
		const auto findAction
			= [](const Ui::PopupMenu &menu, const QString &text) -> QAction * {
			for (const auto &action : menu.actions()) {
				if (action->text() == text) {
					return action;
				}
			}
			return nullptr;
		};
		const auto hasSharingAction = [&](const Ui::PopupMenu &menu) {
			return findAction(menu, tr::lng_disable_sharing(tr::now))
				   || findAction(menu, tr::lng_enable_sharing(tr::now));
		};
		auto historyMenu = buildPeerMenu(Dialogs::EntryState::Section::History);
		auto profileMenu = buildPeerMenu(Dialogs::EntryState::Section::Profile);
		dividers.history = PopupMenuDividerCount(*historyMenu);
		dividers.profile = PopupMenuDividerCount(*profileMenu);
		const auto theme
			= findAction(*historyMenu, tr::lng_chat_theme_wallpaper(tr::now));
		const auto historyFailure
			= PopupMenuKeyboardAndAccessibilityFailure(*historyMenu);
		const auto profileFailure
			= PopupMenuKeyboardAndAccessibilityFailure(*profileMenu);
		const auto menusAreNavigable
			= historyFailure.isEmpty() && profileFailure.isEmpty();
		const auto expectedEntries
			= supported ? (theme && hasSharingAction(*historyMenu)
						   && hasSharingAction(*profileMenu))
						: (!theme && !hasSharingAction(*historyMenu)
						   && !hasSharingAction(*profileMenu));
		if (!menusAreNavigable || !expectedEntries) {
			std::fprintf(
				stderr,
				"Chat feature menu regression failed: label=%s supported=%d "
				"keyboard-accessible=%d entries=%d history-menu=%s "
				"profile-menu=%s\n",
				qPrintable(label), supported, menusAreNavigable,
				expectedEntries,
				historyFailure.isEmpty() ? "ok" : qPrintable(historyFailure),
				profileFailure.isEmpty() ? "ok" : qPrintable(profileFailure));
			return false;
		}
		auto sendMenu = std::make_unique<Ui::PopupMenu>(nullptr);
		const auto sendMenuResult = SendMenu::FillSendMenu(
			sendMenu.get(), nullptr, controller->content()->sendMenuDetails(),
			[](SendMenu::Action, SendMenu::Details) {});
		dividers.send = PopupMenuDividerCount(*sendMenu);
		const auto hasSchedule
			= findAction(*sendMenu, tr::lng_schedule_message(tr::now))
			  || findAction(*sendMenu,
							tr::lng_scheduled_send_until_online(tr::now));
		const auto hasSilentSend
			= findAction(*sendMenu, tr::lng_send_silent_message(tr::now));
		const auto sendFailure
			= PopupMenuKeyboardAndAccessibilityFailure(*sendMenu);
		if ((sendMenuResult != SendMenu::FillMenuResult::Prepared)
			|| !sendFailure.isEmpty()
			|| (supported ? !hasSchedule : (hasSchedule || !hasSilentSend))) {
			std::fprintf(stderr,
						 "Chat feature send menu regression failed: label=%s "
						 "supported=%d schedule=%d silent=%d send-menu=%s\n",
						 qPrintable(label), supported, bool(hasSchedule),
						 bool(hasSilentSend),
						 sendFailure.isEmpty() ? "ok"
											   : qPrintable(sendFailure));
			return false;
		}
		if (supported) {
			if (!theme) {
				return false;
			}
			theme->trigger();
			QCoreApplication::processEvents();
			const auto controlsAccessible
				= historyWidget->chatThemeControlsAccessibleForRegressionTest();
			controller->toggleChooseChatTheme(peer, false);
			if (!controlsAccessible) {
				std::fprintf(
					stderr,
					"Chat theme strip regression failed: stock controls were "
					"missing, unfocusable, or inaccessible in %s.\n",
					qPrintable(label));
				return false;
			}
		} else {
			controller->toggleChooseChatTheme(peer);
			QCoreApplication::processEvents();
			if (historyWidget->chatThemeControlsAccessibleForRegressionTest()) {
				std::fprintf(
					stderr,
					"Chat theme strip regression failed: custom controls were "
					"shown in %s.\n",
					qPrintable(label));
				return false;
			}
		}
		return RunScheduledSubmissionLifecycleRegression(
			controller, supported, activationOrder + u" "_q + label);
	};
	const auto stockLabel = u"stock window "_q + activationOrder;
	const auto customLabel = u"custom window "_q + activationOrder;
	auto stockDividerCounts = DividerCounts();
	auto customDividerCounts = DividerCounts();
	if (!checkController(stockController, true, stockLabel, stockDividerCounts)
		|| !checkController(customController, false, customLabel,
							customDividerCounts)) {
		return false;
	} else if (stockDividerCounts.history != customDividerCounts.history
			   || stockDividerCounts.profile != customDividerCounts.profile
			   || stockDividerCounts.send != customDividerCounts.send) {
		std::fprintf(stderr,
					 "Chat feature menu divider regression failed: order=%s "
					 "stock={%d,%d,%d} custom={%d,%d,%d}\n",
					 qPrintable(activationOrder), stockDividerCounts.history,
					 stockDividerCounts.profile, stockDividerCounts.send,
					 customDividerCounts.history, customDividerCounts.profile,
					 customDividerCounts.send);
		return false;
	}
	return true;
}
#endif

[[nodiscard]] bool
WaitForBindingFilesRemovedForRegression(const QStringList &files) {
	return WaitForRegressionCondition([&] {
		return std::none_of(files.begin(), files.end(), [](const auto &path) {
			return QFileInfo::exists(path);
		});
	});
}

[[nodiscard]] bool WaitForSessionSwitchUpdatesForTest(Main::Session &stock,
													  int stockExpected,
													  Main::Session &pinned,
													  int pinnedExpected) {
	return WaitForRegressionCondition([&] {
		return SessionSwitchUpdateCountForTest(
				   stock.updates().onlineUpdateCountsForRegressionTest())
				   == stockExpected
			   && SessionSwitchUpdateCountForTest(
					  pinned.updates().onlineUpdateCountsForRegressionTest())
					  == pinnedExpected;
	});
}

[[nodiscard]] bool WaitForMainQueueBarrier() {
	return WaitForMainQueueBarrierForRegressionTest()
		   == MainQueueBarrierResult::Completed;
}

[[nodiscard]] bool
CallStartEffectsStayedQuiet(const CallStartRegressionSnapshot &snapshot) {
	return !snapshot.permissionRequests && !snapshot.callRpcs
		   && !snapshot.voiceChatLinkJoinAttempts && !snapshot.activations
		   && !snapshot.leavePrompts;
}

[[nodiscard]] bool
RunIncomingCallAnswerRegression(not_null<Main::Account *> account,
								bool supported) {
	const auto user = account->session().data().userLoaded(UserId(2));
	if (!user) {
		return false;
	}
	for (const auto conferenceInvite : {false, true}) {
		ResetCallStartRegressionForTest();
		auto call = CallsInstanceRegressionAccess::
			CreateIncomingCallForAnswerRegression(user, conferenceInvite);
		SetCallStartRegressionInterceptForTest(true);
		call->answer();
		SetCallStartRegressionInterceptForTest(false);
		const auto snapshot = GetCallStartRegressionSnapshotForTest();
		const auto expectedEffects = supported ? 1 : 0;
		const auto expectedState = supported
									   ? Calls::Call::State::ExchangingKeys
									   : Calls::Call::State::Failed;
		if (snapshot.permissionRequests != expectedEffects
			|| snapshot.callRpcs != expectedEffects
			|| snapshot.unavailableToasts.size() != (supported ? 0U : 1U)
			|| (!supported
				&& snapshot.unavailableToasts.front() != &account->session())
			|| call->state() != expectedState) {
			return false;
		}
	}
	return true;
}

void PrepareRegressionChannel(not_null<ChannelData *> channel) {
	channel->setName(u"Regression channel"_q, QString());
	channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
}

[[nodiscard]] bool RunCallsInstanceStartRegression(
	not_null<Main::Account *> account, not_null<Window::Controller *> window,
	not_null<Calls::Call *> stockCall, bool supported) {
	const auto controller = window->sessionController();
	if (!controller || &controller->session() != &account->session()) {
		return false;
	}
	const auto user = account->session().data().userLoaded(UserId(2));
	if (!user) {
		return false;
	}
	auto &calls = Core::App().calls();
	const auto groupCall = calls.currentGroupCall();
	const auto chat = account->session().data().chat(ChatId(1051));
	const auto peer = not_null<PeerData *>(static_cast<PeerData *>(&*chat));
	const auto show = controller->uiShow();
	ResetCallStartRegressionForTest();
	SetCallStartRegressionInterceptForTest(true);
	ReportCallRegressionCheckpoint("direct starts begin");
	calls.startOutgoingCall(user, {});
	ReportCallRegressionCheckpoint("outgoing start returned");
	calls.startOrJoinGroupCall(show, peer, {});
	ReportCallRegressionCheckpoint("group start returned");
	calls.startOrJoinConferenceCall({.show = show});
	ReportCallRegressionCheckpoint("conference start returned");
	calls.showStartWithRtmp(show, peer);
	ReportCallRegressionCheckpoint("RTMP start returned");
	SetCallStartRegressionInterceptForTest(false);
	const auto snapshot = GetCallStartRegressionSnapshotForTest();
	const auto expectedPassed = supported ? std::array<int, 4>{1, 1, 1, 1}
										  : std::array<int, 4>{0, 0, 0, 0};
	const auto correctToastCount
		= snapshot.unavailableToasts.size() == (supported ? 0U : 4U);
	const auto allToastsBelongToAccount = std::all_of(
		begin(snapshot.unavailableToasts), end(snapshot.unavailableToasts),
		[&](const Main::Session *session) {
			return session == &account->session();
		});
	return snapshot.startPassed == expectedPassed && correctToastCount
		   && allToastsBelongToAccount && CallStartEffectsStayedQuiet(snapshot)
		   && calls.currentCall() == stockCall.get()
		   && stockCall->state() == Calls::Call::State::Established
		   && calls.currentGroupCall() == groupCall && !window->isLayerShown();
}

[[nodiscard]] bool
RunOutgoingStartSelectionRegression(not_null<Main::Account *> account,
									not_null<Window::Controller *> window,
									bool supported) {
	const auto controller = window->sessionController();
	if (!controller || &controller->session() != &account->session()) {
		return false;
	}
	const auto user = account->session().data().userLoaded(UserId(2));
	if (!user) {
		return false;
	}
	auto &calls = Core::App().calls();
	const auto current = calls.currentCall();
	const auto group = calls.currentGroupCall();
	ResetCallStartRegressionForTest();
	SetCallStartRegressionInterceptForTest(true);
	ReportCallRegressionCheckpoint("selection outgoing start begin");
	calls.startOutgoingCall(user, {});
	ReportCallRegressionCheckpoint("selection outgoing start returned");
	SetCallStartRegressionInterceptForTest(false);
	const auto snapshot = GetCallStartRegressionSnapshotForTest();
	const auto expectedPassed = supported ? std::array<int, 4>{1, 0, 0, 0}
										  : std::array<int, 4>{0, 0, 0, 0};
	return snapshot.startPassed == expectedPassed
		   && snapshot.unavailableToasts.size() == (supported ? 0U : 1U)
		   && (snapshot.unavailableToasts.empty()
			   || snapshot.unavailableToasts.front() == &account->session())
		   && CallStartEffectsStayedQuiet(snapshot)
		   && calls.currentCall() == current
		   && calls.currentGroupCall() == group;
}

[[nodiscard]] bool RunConferenceLinkPreservesStockCallRegression(
	not_null<Main::Account *> account, not_null<Window::Controller *> window,
	not_null<Calls::Call *> stockCall) {
	const auto controller = window->sessionController();
	if (!controller || &controller->session() != &account->session()
		|| &stockCall->user()->session() == &account->session()) {
		return false;
	}
	const auto context = QVariant::fromValue(ClickHandlerContext{
		.sessionWindow = base::make_weak(controller),
	});
	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("conference link begin");
	if (!Core::App().openLocalUrl(u"tg://call?slug=regression-conference"_q,
								  context)) {
		return false;
	}
	ReportCallRegressionCheckpoint("conference link returned");
	const auto snapshot = GetCallStartRegressionSnapshotForTest();
	return snapshot.navigationEvents == std::vector<
		SessionNavigationRegressionEvent>{
			SessionNavigationRegressionEvent::FeatureUnavailableToast,
		}
		&& CallStartEffectsStayedQuiet(snapshot)
		&& Core::App().calls().currentCall() == stockCall.get()
		&& stockCall->state() == Calls::Call::State::Established
		&& !window->isLayerShown();
}

[[nodiscard]] bool
RunChannelCallLinkRegression(not_null<Main::Account *> account,
							 not_null<Window::Controller *> window) {
	const auto controller = window->sessionController();
	if (!controller || &controller->session() != &account->session()) {
		return false;
	}
	const auto channelId = ChannelId(2051);
	const auto channel = account->session().data().channel(channelId);
	PrepareRegressionChannel(channel);
	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("channel call link begin");
	controller->showPeerByLink(Window::PeerByLinkInfo{
		.usernameOrId = channelId,
		.voicechatHash = u"regression-link"_q,
	});
	ReportCallRegressionCheckpoint("channel call link dispatched");
	const auto drained = WaitForMainQueueBarrier();
	QCoreApplication::processEvents();
	ReportCallRegressionCheckpoint("channel call link events processed");
	const auto linkEvents = GetCallStartRegressionSnapshotForTest();
	const auto navigationMatches
		= linkEvents.navigationEvents
		  == std::vector<SessionNavigationRegressionEvent>{
			  SessionNavigationRegressionEvent::ChannelOpened,
			  SessionNavigationRegressionEvent::FeatureUnavailableToast,
		  };
	if (!drained || !navigationMatches
		|| linkEvents.openedPeers
			   != std::vector<const PeerData *>{channel.get()}
		|| controller->content()->peer() != channel.get()
		|| !CallStartEffectsStayedQuiet(linkEvents) || window->isLayerShown()) {
		std::fprintf(
			stderr,
			"Channel call link regression: custom=%d blocked=%d drained=%d "
			"navigation=%d events=%zu opened=%zu peer=%d layer=%d "
			"permissions=%d calls=%d joins=%d activations=%d "
			"leave-prompts=%d\n",
			account->local().hasStoredCustomServer(),
			account->mtp().dcOptions().blocked(), drained, navigationMatches,
			linkEvents.navigationEvents.size(), linkEvents.openedPeers.size(),
			controller->content()->peer() == channel.get(),
			window->isLayerShown(), linkEvents.permissionRequests,
			linkEvents.callRpcs, linkEvents.voiceChatLinkJoinAttempts,
			linkEvents.activations, linkEvents.leavePrompts);
		return false;
	}
	return true;
}

[[nodiscard]] bool
RunCallLinkAndSettingsRegression(not_null<Main::Account *> account,
								 not_null<Window::Controller *> window) {
	const auto controller = window->sessionController();
	if (!controller || &controller->session() != &account->session()
		|| !RunChannelCallLinkRegression(account, window)
		|| !RunTeagramBrandingRegression(controller, "settings smoke")) {
		return false;
	}
	const auto closeSettingsLayer = gsl::finally([&] {
		if (window->isLayerShown()) {
			window->hideSettingsAndLayer(anim::type::instant);
		}
	});
	const auto channel = account->session().data().channel(ChannelId(2051));

	const auto context = QVariant::fromValue(ClickHandlerContext{
		.sessionWindow = base::make_weak(controller),
	});
	const auto showsSettings = [&](Settings::Type expected) {
		const auto wrapped = dynamic_cast<Info::WrapWidget *>(
			controller->activeLayerSection());
		return wrapped
			   && (wrapped->controller()->section().type()
				   == Info::Section::Type::Settings)
			   && (wrapped->controller()->section().settingsType() == expected);
	};
	auto settingLinkNumber = 0;
	for (const auto &url : {
			 u"tg://settings/privacy/calls"_q,
			 u"tg://settings/privacy/calls/never"_q,
			 u"tg://settings/privacy/calls/always"_q,
			 u"tg://settings/privacy/calls/p2p"_q,
			 u"tg://settings/privacy/calls/p2p/never"_q,
			 u"tg://settings/privacy/calls/p2p/always"_q,
			 u"tg://settings/calls/all"_q,
			 u"tg://settings/calls/start-call"_q,
		 }) {
		std::fprintf(stderr,
					 "Call regression checkpoint: setting link %d begin\n",
					 settingLinkNumber);
		std::fflush(stderr);
		ResetCallStartRegressionForTest();
		if (!Core::App().openLocalUrl(url, context)) {
			return false;
		}
		++settingLinkNumber;
		ReportCallRegressionCheckpoint("call setting link returned");
		const auto events = GetCallStartRegressionSnapshotForTest();
		if (events.navigationEvents != std::vector<
				SessionNavigationRegressionEvent>{
					SessionNavigationRegressionEvent::FeatureUnavailableToast,
				}
			|| !CallStartEffectsStayedQuiet(events)
			|| controller->content()->peer() != channel.get()
			|| window->isLayerShown()) {
			return false;
		}
	}

	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("calls settings begin");
	if (!Core::App().openLocalUrl(u"tg://settings/calls"_q, context)) {
		return false;
	}
	if (!WaitForMainQueueBarrier()) {
		return false;
	}
	QCoreApplication::processEvents();
	ReportCallRegressionCheckpoint("calls settings events processed");
	const auto callsSettingsEvents = GetCallStartRegressionSnapshotForTest();
	const auto callsSettingsShown = showsSettings(Settings::CallsId());
	const auto callsSettingsQuiet
		= callsSettingsEvents.navigationEvents.empty()
		  && CallStartEffectsStayedQuiet(callsSettingsEvents);
	if (!callsSettingsQuiet || !callsSettingsShown) {
		std::fprintf(
			stderr,
			"Call settings link regression: quiet=%d shown=%d layer=%d "
			"navigation=%zu permissions=%d calls=%d joins=%d activations=%d "
			"leave-prompts=%d\n",
			callsSettingsQuiet, callsSettingsShown, window->isLayerShown(),
			callsSettingsEvents.navigationEvents.size(),
			callsSettingsEvents.permissionRequests,
			callsSettingsEvents.callRpcs,
			callsSettingsEvents.voiceChatLinkJoinAttempts,
			callsSettingsEvents.activations, callsSettingsEvents.leavePrompts);
		return false;
	}

	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("device settings begin");
	if (!Core::App().openLocalUrl(u"tg://settings/devices"_q, context)) {
		return false;
	}
	if (!WaitForMainQueueBarrier()) {
		return false;
	}
	QCoreApplication::processEvents();
	ReportCallRegressionCheckpoint("device settings events processed");
	const auto devicesSettingsEvents = GetCallStartRegressionSnapshotForTest();
	return devicesSettingsEvents.navigationEvents.empty()
		   && CallStartEffectsStayedQuiet(devicesSettingsEvents)
		   && showsSettings(Settings::SessionsId());
}

struct NativeLifecycleResults {
	std::vector<const char *> failures;
	QStringList reported;
	const char *activeCase = nullptr;
	bool summaryReported = false;
	bool asyncPending = false;
	bool finalized = false;
	bool prerequisitesPassed = false;

	void StopActiveCase() {
		if (!activeCase) {
			return;
		}
		std::fprintf(stderr, "native_lifecycle_case=%s result=STOPPED\n",
					 activeCase);
		std::fflush(stderr);
		reported.push_back(QString::fromLatin1(activeCase));
		failures.push_back(activeCase);
		if (std::strcmp(activeCase, "chat-participants-prerequisites") == 0) {
			prerequisitesPassed = false;
		}
		activeCase = nullptr;
	}

	void StartCase(const char *name) {
		if (activeCase && (std::strcmp(activeCase, name) == 0)) {
			return;
		}
		StopActiveCase();
		activeCase = name;
		std::fprintf(stderr, "native_lifecycle_case=%s status=START\n", name);
		std::fflush(stderr);
	}

	void Report(const char *name, bool passed) {
		if (activeCase && (std::strcmp(activeCase, name) != 0)) {
			StopActiveCase();
		}
		if (activeCase) {
			activeCase = nullptr;
		}
		std::fprintf(stderr, "native_lifecycle_case=%s result=%s\n", name,
					 passed ? "PASS" : "FAIL");
		reported.push_back(QString::fromLatin1(name));
		if (!passed) {
			failures.push_back(name);
		}
		if (std::strcmp(name, "chat-participants-prerequisites") == 0) {
			prerequisitesPassed = passed;
		}
	}

	void Skip(const char *name, const char *reason) {
		if (activeCase && (std::strcmp(activeCase, name) == 0)) {
			activeCase = nullptr;
		}
		std::fprintf(stderr, "native_lifecycle_case=%s result=SKIP reason=%s\n",
					 name, reason);
		reported.push_back(QString::fromLatin1(name));
	}

	void SkipUnreported(const char *reason) {
		static constexpr const char *kCases[] = {
			"username-input",
			"teagram-icon-persistence",
			"account-lifecycle-prerequisites",
			"legacy-cleanup-symlink",
			"cache-callback-timeout",
			"account-authorization-lifecycle",
			"chat-participants-prerequisites",
			"cache-concurrent-deletion",
			"calls-instance-start",
			"stock-incoming-call-answer",
			"pinned-incoming-call-answer",
			"stock-blocked-call-policy",
			"pinned-blocked-call-policy",
			"conference-link-call-preservation",
			"call-links-and-settings",
			"call-session-fixtures",
			"admin-window-fixtures",
			"settings-search",
			"session-window-lifecycle",
			"translation-controls",
			"manage-controls",
			"boost-entry-paths",
			"passport-link-refusal",
			"mini-app-open",
			"settings-deep-link",
			"messages-privacy",
			"paid-link",
			"boost-story-menus",
			"chatlist-invite-check",
			"authenticated-cache-file-symlinks",
			"post-open-cache",
			"post-open-media-cache",
			"pinned-chat-participants",
			"folder-crud",
			"bio-editor-target",
			"admin-session-identities",
			"stock-basic-group-creator",
			"pinned-basic-group-creator",
			"stock-full-list-admin",
			"stock-full-list-demotion",
			"custom-full-list-admin",
			"stock-live-admin-promotion",
			"stock-live-admin-demotion",
			"custom-live-admin-promotion",
			"custom-live-admin-demotion",
			"pinned-migration-fixture",
			"post-close-window-lifecycle",
			"chat-migration",
			"protected-cache-cleanup",
		};
		for (const auto name : kCases) {
			if (!reported.contains(QString::fromLatin1(name))) {
				Skip(name, reason);
			}
		}
	}

	[[nodiscard]] bool Passed() const { return failures.empty(); }

	void ReportSummary() {
		if (summaryReported) {
			return;
		}
		summaryReported = true;
		std::fprintf(stderr, "native_lifecycle_cases_failed=");
		if (failures.empty()) {
			std::fprintf(stderr, "none\n");
			return;
		}
		for (auto i = 0; i != int(failures.size()); ++i) {
			std::fprintf(stderr, "%s%s", (i == 0) ? "" : ",", failures[i]);
		}
		std::fprintf(stderr, "\n");
	}

	~NativeLifecycleResults() {
		StopActiveCase();
		if (!asyncPending && !finalized) {
			if (!prerequisitesPassed) {
				if (!reported.contains(QString::fromLatin1(
						"chat-participants-prerequisites"))) {
					Report("chat-participants-prerequisites", false);
				}
				SkipUnreported("dependency-chat-participants-prerequisites");
			} else {
				SkipUnreported("dependency-native-lifecycle-incomplete");
			}
		}
		ReportSummary();
	}
};

void RunAuthenticatedCacheSymlinkCases(
	Main::Domain &domain, const ProtectedCacheFixtures &fixtures,
	not_null<Main::Account *> pinned, const QString &pinnedCachePath,
	const QString &pinnedMediaCachePath,
	std::shared_ptr<NativeLifecycleResults> nativeLifecycleResults) {
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		nativeLifecycleResults->StartCase("authenticated-cache-file-symlinks");
		pinned->session().data().cache().sync();
		pinned->session().data().cacheBigFile().sync();
		const auto cacheFileRefused = !Core::MacProtectedPath::CheckCachePath(
			pinnedCachePath, "Tests::ProtectedCache::file");
		const auto mediaCacheFileRefused
			= !Core::MacProtectedPath::CheckCachePath(
				pinnedMediaCachePath, "Tests::ProtectedCache::media-file");
		const auto cacheFilesPassed = cacheFileRefused && mediaCacheFileRefused;
		nativeLifecycleResults->Report("authenticated-cache-file-symlinks",
									   cacheFilesPassed);
		if (cacheFilesPassed) {
			std::fprintf(stderr,
						 "Authenticated cache regression passed: root, "
						 "directory, and file "
						 "symlinks refused for cache and media_cache.\n");
		}
	} else {
		nativeLifecycleResults->Skip("authenticated-cache-file-symlinks",
									 "protected-path-integration-disabled");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		const auto selfId = UserId(1);
		auto postOpenCachePassed = false;
		auto postOpenMediaCachePassed = false;
		for (const auto mediaCache : {false, true}) {
			const auto caseName
				= mediaCache ? "post-open-media-cache" : "post-open-cache";
			nativeLifecycleResults->StartCase(caseName);
			const auto guarded = domain.add(MTP::Environment::Production);
			guarded->mtp().stopForServerEnrollment();
			guarded->setSessionUserId(selfId);
			if (!guarded->createSession(
					RegressionUser(selfId, true, QString()),
					std::make_unique<Main::SessionSettings>())) {
				nativeLifecycleResults->Report(caseName, false);
				continue;
			}
			const auto cacheGetter
				= [guarded, mediaCache]() -> Storage::Cache::Database & {
				auto &data = guarded->session().data();
				return mediaCache ? data.cacheBigFile() : data.cache();
			};
			const auto clearCaches = [guarded] {
				auto &data = guarded->session().data();
				data.clearLocalStorage();
				data.cache().sync();
				data.cacheBigFile().sync();
			};
			const auto passed = RunPostOpenCacheSymlinkRegression(
				mediaCache ? guarded->local().cacheBigFilePath()
						   : guarded->local().cachePath(),
				mediaCache ? fixtures.openedMediaCache : fixtures.openedCache,
				cacheGetter, clearCaches, [guarded, mediaCache] {
					return RunRefusedCacheReaderRegressions(guarded,
															mediaCache);
				});
			nativeLifecycleResults->Report(caseName, passed);
			if (mediaCache) {
				postOpenMediaCachePassed = passed;
			} else {
				postOpenCachePassed = passed;
			}
		}
		if (postOpenCachePassed && postOpenMediaCachePassed) {
			std::fprintf(stderr,
						 "Authenticated cache post-open regression passed: "
						 "get, put, and "
						 "clear symlinks refused for cache and media_cache.\n");
		}
	} else {
		nativeLifecycleResults->Skip("post-open-cache",
									 "protected-path-integration-disabled");
		nativeLifecycleResults->Skip("post-open-media-cache",
									 "protected-path-integration-disabled");
	}
}

void RunProtectedCacheCleanupRegression(
	not_null<Main::Account *> stock, not_null<Main::Account *> pinned,
	const QString &stockCachePath, const QString &stockMediaCachePath,
	const QString &pinnedCachePath, const QString &pinnedMediaCachePath,
	std::shared_ptr<NativeLifecycleResults> nativeLifecycleResults) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		nativeLifecycleResults->Skip("protected-cache-cleanup",
									 "protected-path-integration-disabled");
		return;
	}
	nativeLifecycleResults->StartCase("protected-cache-cleanup");
	stock->session().data().clearLocalStorage();
	pinned->session().data().clearLocalStorage();
	stock->session().data().cache().sync();
	stock->session().data().cacheBigFile().sync();
	pinned->session().data().cache().sync();
	pinned->session().data().cacheBigFile().sync();
	const auto protectedCacheCleanupPassed
		= !Core::MacProtectedPath::CheckCachePath(
			  stockCachePath, "Tests::ProtectedCache::async-root")
		  && !Core::MacProtectedPath::CheckCachePath(
			  stockMediaCachePath, "Tests::ProtectedCache::async-media-root")
		  && !Core::MacProtectedPath::CheckCachePath(
			  pinnedCachePath, "Tests::ProtectedCache::async-file")
		  && !Core::MacProtectedPath::CheckCachePath(
			  pinnedMediaCachePath, "Tests::ProtectedCache::async-media-file");
	nativeLifecycleResults->Report("protected-cache-cleanup",
								   protectedCacheCleanupPassed);
	if (!protectedCacheCleanupPassed) {
		std::fprintf(stderr,
					 "Chat participants regression failed: asynchronous cache "
					 "cleanup accessed a protected target.\n");
	}
}

[[nodiscard]] bool
RunAiComposeApplyHandlerRegression(not_null<Window::Controller *> window,
								   not_null<Main::Account *> account,
								   bool supported, const char *stage) {
	const auto controller = window->sessionController();
	if (!controller || (&controller->session() != &account->session())
		|| (controller->session().aiComposeSupported() != supported)) {
		return false;
	}
	const auto chatId = ChatId(1051);
	const auto chat = controller->session().data().chat(chatId);
	const auto peer = not_null<PeerData *>(static_cast<PeerData *>(&*chat));
	if (!controller->session().user()->isLoaded()) {
		controller->session().api().processFullPeer(
			peer,
			RegressionChatFullReply(chatId, 2, u"+10000000001"_q, UserId(2)));
	}
	const auto history = controller->session().data().history(peer);
	controller->showPeerHistory(history);
	QCoreApplication::processEvents();
	const auto historyWidget = [&] {
		for (const auto child :
			 controller->content()->findChildren<QWidget *>()) {
			if (const auto result = dynamic_cast<HistoryWidget *>(child)) {
				return result;
			}
		}
		return static_cast<HistoryWidget *>(nullptr);
	}();
	if (!historyWidget) {
		return false;
	}
	const auto verify = [&](const char *handler, bool historyWidgetHandler,
							AiComposeApplyRegressionCounts before,
							int resolvesBefore) {
		const auto after = GetAiComposeApplyRegressionCounts();
		const auto expected = supported ? 1 : 0;
		const auto historyReads
			= after.historyWidgetDraftReads - before.historyWidgetDraftReads;
		const auto composeReads = after.composeControlsDraftReads
								  - before.composeControlsDraftReads;
		const auto dispatches = after.applyDispatches - before.applyDispatches;
		const auto resolves = account->session()
								  .data()
								  .aiComposeTones()
								  .resolveCallsForRegressionTest()
							  - resolvesBefore;
		const auto matches
			= historyWidgetHandler
				  ? (historyReads == expected && composeReads == 0)
				  : (historyReads == 0 && composeReads == expected);
		if (!matches || dispatches != expected || resolves != expected) {
			std::fprintf(stderr,
						 "AI apply lifecycle regression failed at %s / %s: "
						 "reads=%d/%d dispatches=%d resolves=%d expected=%d\n",
						 stage, handler, historyReads, composeReads, dispatches,
						 resolves, expected);
			return false;
		}
		return true;
	};

	auto tones = &account->session().data().aiComposeTones();
	auto readsBefore = GetAiComposeApplyRegressionCounts();
	auto resolvesBefore = tones->resolveCallsForRegressionTest();
	historyWidget->triggerAiApplyInPlaceForRegressionTest(u"lifecycle draft"_q);
	if (!verify("HistoryWidget", true, readsBefore, resolvesBefore)) {
		return false;
	}

	const auto repliesRootId = MsgId(1);
	const auto repliesRootFullId = FullMsgId(peer->id, repliesRootId);
	if (!controller->session().data().message(repliesRootFullId)) {
		const auto root = MTP_message(
			MTP_flags(MTPDmessage::Flag::f_from_id | MTPDmessage::Flag::f_out),
			MTP_int(int(repliesRootId.bare)),
			peerToMTP(controller->session().userPeerId()),
			MTPint(),	 // from_boosts_applied
			MTPstring(), // from_rank
			peerToMTP(peer->id),
			MTPPeer(), // saved_peer_id
			MTPMessageFwdHeader(),
			MTP_long(0), // via_bot_id
			MTPlong(),	 // via_business_bot_id
			MTPPeer(),	 // guestchat_via_from
			MTPMessageReplyHeader(),
			MTP_int(1), // date
			MTP_string(u"synthetic replies root"_q), MTP_messageMediaEmpty(),
			MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(),
			MTP_int(0), // views
			MTP_int(0), // forwards
			MTPMessageReplies(),
			MTP_int(0), // edit_date
			MTPstring(), MTPlong(), MTPMessageReactions(),
			MTPVector<MTPRestrictionReason>(),
			MTP_int(0), // ttl_period
			MTPint(),	// quick_reply_shortcut_id
			MTPlong(),	// effect
			MTPFactCheck(),
			MTPint(),  // report_delivery_until_date
			MTPlong(), // paid_message_stars
			MTPSuggestedPost(),
			MTPint(),	 // schedule_repeat_period
			MTPstring(), // summary_from_language
			MTPRichMessage());
		if (!controller->session().data().addNewMessage(
				root, MessageFlags(), NewMessageType::Existing)) {
			return false;
		}
	}
	controller->showSection(
		std::make_shared<HistoryView::ChatMemento>(HistoryView::ChatViewId{
			.history = history,
			.repliesRootId = repliesRootId,
		}),
		Window::SectionShow::Way::ClearStack);
	QCoreApplication::processEvents();
	const auto chatWidget = [&] {
		for (const auto child :
			 controller->content()->findChildren<QWidget *>()) {
			if (const auto result
				= dynamic_cast<HistoryView::ChatWidget *>(child)) {
				return result;
			}
		}
		return static_cast<HistoryView::ChatWidget *>(nullptr);
	}();
	if (!chatWidget) {
		return false;
	}
	readsBefore = GetAiComposeApplyRegressionCounts();
	resolvesBefore = tones->resolveCallsForRegressionTest();
	chatWidget->triggerAiApplyInPlaceForRegressionTest(u"lifecycle draft"_q);
	return verify("ComposeControls", false, readsBefore, resolvesBefore);
}

[[nodiscard]] bool RunAiComposeApplySwitchRegression(
	not_null<Window::Controller *> window, not_null<Main::Account *> stock,
	not_null<Main::Account *> custom, not_null<Main::Account *> blocked) {
	const auto previousSlug = Api::AiApplyBoundSlug();
	Api::SetAiApplyBoundSlug(u"lifecycle-regression-missing-tone"_q);
	const auto restoreSlug = gsl::finally([=] {
		Api::SetAiApplyBoundSlug(previousSlug);
		ResetAiComposeApplyRegressionCounts();
	});
	ResetAiComposeApplyRegressionCounts();
	const auto run = [&](not_null<Main::Account *> account, bool supported,
						 const char *stage) {
		return RunAiComposeApplyHandlerRegression(window, account, supported,
												  stage);
	};
	if (!run(stock, true, "stock")) {
		return false;
	}
	window->showAccount(custom);
	if (!run(custom, false, "custom")) {
		return false;
	}
	window->showAccount(blocked);
	if (!run(blocked, false, "blocked")) {
		return false;
	}
	window->showAccount(stock);
	return run(stock, true, "stock after switch back");
}

[[nodiscard]] int StartChatParticipantsRegression(
	Main::Domain &domain, const ProtectedCacheFixtures &fixtures,
	std::shared_ptr<NativeLifecycleResults> nativeLifecycleResults,
	Fn<void(int)> done) {
	const auto authenticatedCacheOnly = qEnvironmentVariableIsSet(
		"TDESKTOP_AUTH_LIFECYCLE_REGRESSION_CACHE_ONLY");
	const auto injectedFailCases
		= qEnvironmentVariable("TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE")
			  .split(',', Qt::SkipEmptyParts);
	const auto shouldInjectFailure = [&](const char *name) {
		return injectedFailCases.contains(QString::fromLatin1(name));
	};
	const auto recordChatCase = [nativeLifecycleResults](const char *name,
														 bool passed,
														 const char *reason) {
		nativeLifecycleResults->Report(name, passed);
		if (!passed) {
			std::fprintf(stderr, "Chat participants regression failed: %s\n",
						 reason);
		}
	};
	const auto failPrerequisite = [&](const char *reason) {
		nativeLifecycleResults->StartCase("chat-participants-prerequisites");
		nativeLifecycleResults->Report("chat-participants-prerequisites",
									   false);
		nativeLifecycleResults->SkipUnreported(
			"dependency-chat-participants-prerequisites");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return FailChatParticipantsRegression(reason);
	};
	ReportCallRegressionCheckpoint("chat participant regression begin");
	Api::Updates::setDeferredOnlineUpdatePhaseForRegressionTest(
		DeferredOnlineUpdatePhase::Setup);
	nativeLifecycleResults->StartCase("cache-concurrent-deletion");
	const auto cacheConcurrentDeletionPassed
		= (!Core::MacProtectedPath::IntegrationTestActive()
		   || RunCacheConcurrentDeletionRegression())
		  && !shouldInjectFailure("cache-concurrent-deletion");
	nativeLifecycleResults->Report("cache-concurrent-deletion",
								   cacheConcurrentDeletionPassed);
	nativeLifecycleResults->StartCase("chat-participants-prerequisites");
	// Stock, pinned, blank, boost and restored blocked accounts coexist here.
	constexpr auto requiredAccounts = 5;
	if (domain.accounts().size()
		> Main::Domain::kPremiumMaxAccounts - requiredAccounts) {
		return failPrerequisite(
			"not enough account slots for isolated lifecycle fixtures");
	}
	const auto selfId = UserId(1);
	const auto chatId = ChatId(1051);
	const auto stock = domain.add(MTP::Environment::Production);
	stock->mtp().stopForServerEnrollment();
	stock->setSessionUserId(selfId);
	const auto stockCachePath = stock->local().cachePath();
	const auto stockMediaCachePath = stock->local().cacheBigFilePath();
	const auto refusedDownload = FullMsgId(peerFromUser(selfId), MsgId(12345));
	if (!StoreRefusedDownloadHistory(stock, fixtures.cacheRoot + "/marker",
									 refusedDownload)) {
		return failPrerequisite(
			"could not persist the refused download-history fixture");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()
		&& (!QDir().mkpath(QFileInfo(stockCachePath).absolutePath())
			|| !QDir().mkpath(QFileInfo(stockMediaCachePath).absolutePath())
			|| !QFile::link(fixtures.cacheRoot, stockCachePath)
			|| !QFile::link(fixtures.mediaCacheRoot, stockMediaCachePath))) {
		return failPrerequisite(
			"could not create protected cache-root symlink fixtures");
	}
	if (!stock->createSession(RegressionUser(selfId, true, QString()),
							  std::make_unique<Main::SessionSettings>())) {
		return failPrerequisite("could not create the stock test session");
	}
	const auto stockWasPremium = stock->session().premium();
	const auto restoreStockPremium = gsl::finally([&] {
		Expects(SetRegressionSessionPremium(&stock->session(), selfId,
											stockWasPremium));
	});
	if (!SetRegressionSessionPremium(&stock->session(), selfId, true)) {
		return failPrerequisite(
			"could not enable Premium for stock Settings search checks");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		if (!RunRefusedDownloadHistoryRegression(&stock->session(),
												 refusedDownload)) {
			return failPrerequisite(
				"refused persisted download history was published");
		}
		std::fprintf(
			stderr, "Authenticated refused download history regression passed: "
					"entry without a live message remained unpublished.\n");
		stock->session().data().cache().sync();
		stock->session().data().cacheBigFile().sync();
		if (Core::MacProtectedPath::CheckCachePath(
				stockCachePath, "Tests::ProtectedCache::root")
			|| Core::MacProtectedPath::CheckCachePath(
				stockMediaCachePath, "Tests::ProtectedCache::media-root")) {
			return failPrerequisite(
				"authenticated session accessed a protected cache root");
		}
	}
	const auto stockChat = stock->session().data().chat(chatId);
	const auto stockPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*stockChat));
	if (!authenticatedCacheOnly) {
		stock->session().api().processFullPeer(
			stockPeer,
			RegressionStockChatFullReply(chatId, 1, QString(), UserId(2)));
		if (stock->session().user()->isLoaded()
			|| !stockChat->participants.empty()) {
			return failPrerequisite(
				"stock session accepted phone-free self in full chat info");
		}
		stock->session().api().processFullPeer(
			stockPeer, RegressionStockChatFullReply(
						   chatId, 2, u"+10000000001"_q, UserId(2)));
		if (!stock->session().user()->isLoaded()
			|| !HasExpectedParticipants(stockChat, UserId(2))) {
			return failPrerequisite(
				"stock session with phone did not load creator and members");
		}
	}

	const auto pinned = domain.add(MTP::Environment::Production);
	pinned->mtp().stopForServerEnrollment();
	if (!ConfigurePinnedServer(pinned, RegressionServerKey())) {
		return failPrerequisite("could not pin the custom test server");
	}
	pinned->setSessionUserId(selfId);
	const auto pinnedCachePath = pinned->local().cachePath();
	const auto pinnedMediaCachePath = pinned->local().cacheBigFilePath();
	if (Core::MacProtectedPath::IntegrationTestActive()
		&& (!QDir().mkpath(pinnedCachePath)
			|| !QDir().mkpath(pinnedMediaCachePath)
			|| !QFile::link(fixtures.cacheLeaf, pinnedCachePath + "/0")
			|| !QFile::link(fixtures.cacheLeaf + "/marker",
							pinnedCachePath + "/version")
			|| !QFile::link(fixtures.mediaCacheLeaf,
							pinnedMediaCachePath + "/0")
			|| !QFile::link(fixtures.mediaCacheLeaf + "/marker",
							pinnedMediaCachePath + "/version"))) {
		return failPrerequisite(
			"could not create protected cache-file symlink fixtures");
	}
	if (!pinned->createSession(RegressionUser(selfId, true, QString()),
							   std::make_unique<Main::SessionSettings>())) {
		return failPrerequisite("could not create the pinned test session");
	}
	if (stock->session().uniqueId() != pinned->session().uniqueId()) {
		return failPrerequisite(
			"stock and pinned sessions did not share the same user id");
	}
	const auto &stockTones = stock->session().data().aiComposeTones();
	const auto &pinnedTones = pinned->session().data().aiComposeTones();
	if (stockTones.refreshRequestsForRegressionTest() != 1
		|| !stockTones.periodicRefreshScheduledForRegressionTest()
		|| pinnedTones.refreshRequestsForRegressionTest() != 0
		|| pinnedTones.periodicRefreshScheduledForRegressionTest()) {
		return failPrerequisite(
			"tone refresh escaped the stock-session capability boundary");
	}
	if (authenticatedCacheOnly) {
		nativeLifecycleResults->Report("chat-participants-prerequisites", true);
		RunAuthenticatedCacheSymlinkCases(domain, fixtures, pinned,
										  pinnedCachePath, pinnedMediaCachePath,
										  nativeLifecycleResults);
		RunProtectedCacheCleanupRegression(
			stock, pinned, stockCachePath, stockMediaCachePath, pinnedCachePath,
			pinnedMediaCachePath, nativeLifecycleResults);
		nativeLifecycleResults->SkipUnreported("authenticated-cache-only");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		auto result = 0;
		if (!nativeLifecycleResults->Passed()) {
			result = FailChatParticipantsRegression(
				"authenticated cache regression failed");
		} else {
			std::fprintf(stderr,
						 "Authenticated cache regression cases passed.\n");
		}
		crl::on_main([=] { done(result); });
		return 0;
	}
	const auto capabilitiesMatch = [](const Main::Session &session,
									  bool supported) {
		return (session.callsSupported() == supported)
			   && (session.botAppsSupported() == supported)
			   && (session.paidFeaturesSupported() == supported)
			   && (session.storiesSupported() == supported)
			   && (session.exportSupported() == supported)
			   && (session.passportSupported() == supported)
			   && (session.aiComposeSupported() == supported)
			   && (Ui::AiComposeButtonAllowed(session.aiComposeSupported(),
											  false, true)
				   == supported)
			   && (session.serverTranslationSupported() == supported)
			   && (session.sharedFoldersSupported() == supported)
			   && (session.accountBioEditSupported() == supported)
			   && (session.accountUsernameEditSupported() == supported)
			   && (Main::details::noForwardsSupportedForPeer(session.user())
				   == supported)
			   && (Main::details::chatThemeEditingSupportedForPeer(
					   session.user())
				   == supported)
			   && (Main::details::scheduledMessagesSupportedForPeer(
					   session.user())
				   == supported);
	};
	const auto pinnedUserPeer = not_null<PeerData *>(
		static_cast<PeerData *>(&*pinned->session().user()));
	auto &app = Core::App();
	pinned->mtp().stopForServerEnrollment();
	const auto primary = app.activePrimaryWindow();
	if (!primary || !primary->isPrimary()) {
		return failPrerequisite(
			"primary window disappeared before online-update lifetime "
			"regression");
	}
	const auto fallbackAccount = not_null<Main::Account *>(&domain.active());
	if (fallbackAccount->sessionExists()) {
		return failPrerequisite(
			"initial logout fallback account unexpectedly had a session");
	}
	const auto fallbackAccountLifetime = base::make_weak(fallbackAccount.get());
	auto pinnedFallbackWindow = static_cast<Window::Controller *>(nullptr);
	auto fallbackAccountWindow = static_cast<Window::Controller *>(nullptr);
	auto fallbackWindowsOpen = true;
	const auto closeFallbackWindows = gsl::finally([&] {
		if (!fallbackWindowsOpen) {
			return;
		}
		if (const auto account = fallbackAccountLifetime.get();
			fallbackAccountWindow && account
			&& app.separateWindowFor(not_null<Main::Account *>(account))
				   == fallbackAccountWindow) {
			app.closeWindow(fallbackAccountWindow);
		}
		if (pinnedFallbackWindow
			&& app.separateWindowFor(pinned) == pinnedFallbackWindow) {
			app.closeWindow(pinnedFallbackWindow);
		}
	});
	primary->showAccount(stock);
	if (primary->maybeSession() != &stock->session()) {
		return failPrerequisite(
			"primary window did not switch to the stock session");
	}
	// Keep the initial sessionless account available as a stable logout
	// fallback. Activating stock schedules removeRedundantAccounts(), which
	// otherwise drops this account once the primary is rebound.
	if (app.separateWindowFor(fallbackAccount)) {
		return failPrerequisite(
			"initial logout fallback still owned the primary window");
	}
	fallbackAccountWindow = app.ensureSeparateWindowFor(fallbackAccount);
	primary->activate();
	if (!fallbackAccountWindow || fallbackAccountWindow == primary
		|| app.separateWindowFor(fallbackAccount) != fallbackAccountWindow
		|| app.activePrimaryWindow() != primary
		|| primary->maybeSession() != &stock->session()) {
		return failPrerequisite(
			"could not preserve the initial logout fallback window");
	}
	domain.activate(stock);
	if (!WaitForMainQueueBarrier()) {
		return failPrerequisite("main-thread queue did not drain after the "
								"initial session switch");
	}
	QCoreApplication::processEvents();
	if (!RunDeferredSwitchMutationRegression(
			*primary, *stock, *pinned, DeferredOnlineUpdateMutation::Remove,
			"remove-dispatch", "mutation-remove-deferred",
			SwitchUpdateClassification::MissingDeferred)) {
		return failPrerequisite(
			"switch fixture did not reject the removed deferred dispatch");
	}
	const auto pinnedBeforeRestoreRemoval
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(stock);
	QCoreApplication::processEvents();
	if (primary->maybeSession() != &stock->session()
		|| !WaitForDeferredSwitchUpdate(
			pinned->session(), pinnedBeforeRestoreRemoval.switchDeferred + 1)) {
		return failPrerequisite(
			"could not restore stock after the removed-dispatch mutation");
	}
	if (!RunDeferredSwitchMutationRegression(
			*primary, *stock, *pinned, DeferredOnlineUpdateMutation::Duplicate,
			"duplicate-dispatch", "mutation-duplicate-deferred",
			SwitchUpdateClassification::ExtraDeferred)) {
		return failPrerequisite(
			"switch fixture did not reject the duplicated deferred dispatch");
	}
	const auto pinnedBeforeRestoreDuplicate
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(stock);
	QCoreApplication::processEvents();
	if (primary->maybeSession() != &stock->session()
		|| !WaitForDeferredSwitchUpdate(
			pinned->session(),
			pinnedBeforeRestoreDuplicate.switchDeferred + 1)) {
		return failPrerequisite(
			"could not restore stock after the duplicated-dispatch mutation");
	}
	const auto stockToPinnedStockUpdates
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto stockToPinnedPinnedUpdates
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(pinned);
	const auto stockToPinnedInlineStock = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockToPinnedStockUpdates);
	const auto stockToPinnedInlinePinned = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		stockToPinnedPinnedUpdates);
	const auto stockToPinnedInlineMatches = ReportSwitchUpdateDeltas(
		"stock-to-pinned inline", stockToPinnedInlineStock, {0, 0},
		stockToPinnedInlinePinned, {1, 0});
	if (primary->maybeSession() != &pinned->session()
		|| !stockToPinnedInlineMatches || stockToPinnedInlineStock.total != 0
		|| stockToPinnedInlinePinned.total != 1) {
		return failPrerequisite(
			"stock-to-pinned switch did not update only the shown session "
			"inline");
	}
	QCoreApplication::processEvents();
	const auto stockToPinnedDeferredObserved = WaitForDeferredSwitchUpdate(
		stock->session(), stockToPinnedStockUpdates.switchDeferred + 1);
	const auto stockToPinnedDeferredStock = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockToPinnedStockUpdates);
	const auto stockToPinnedDeferredPinned = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		stockToPinnedPinnedUpdates);
	const auto stockToPinnedDeferredMatches = ReportSwitchUpdateDeltas(
		"stock-to-pinned deferred", stockToPinnedDeferredStock, {0, 1},
		stockToPinnedDeferredPinned, {1, 0});
	if (!WaitForMainQueueBarrier()
		|| !WaitForSessionSwitchUpdatesForTest(
			stock->session(),
			SessionSwitchUpdateCountForTest(stockToPinnedStockUpdates) + 1,
			pinned->session(),
			SessionSwitchUpdateCountForTest(stockToPinnedPinnedUpdates) + 1)
		|| !stockToPinnedDeferredObserved || !stockToPinnedDeferredMatches) {
		return failPrerequisite(
			"stock-to-pinned switch did not update the previous session once");
	}
	const auto pinnedToStockStockUpdates
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedToStockPinnedUpdates
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(stock);
	const auto pinnedToStockInlineStock = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedToStockStockUpdates);
	const auto pinnedToStockInlinePinned = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedToStockPinnedUpdates);
	const auto pinnedToStockInlineMatches = ReportSwitchUpdateDeltas(
		"pinned-to-stock inline", pinnedToStockInlineStock, {1, 0},
		pinnedToStockInlinePinned, {0, 0});
	if (primary->maybeSession() != &stock->session()
		|| !pinnedToStockInlineMatches || pinnedToStockInlineStock.total != 1
		|| pinnedToStockInlinePinned.total != 0) {
		return failPrerequisite(
			"pinned-to-stock switch did not update only the shown session "
			"inline");
	}
	QCoreApplication::processEvents();
	const auto pinnedToStockDeferredObserved = WaitForDeferredSwitchUpdate(
		pinned->session(), pinnedToStockPinnedUpdates.switchDeferred + 1);
	const auto pinnedToStockDeferredStock = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedToStockStockUpdates);
	const auto pinnedToStockDeferredPinned = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedToStockPinnedUpdates);
	const auto pinnedToStockDeferredMatches = ReportSwitchUpdateDeltas(
		"pinned-to-stock deferred", pinnedToStockDeferredStock, {1, 0},
		pinnedToStockDeferredPinned, {0, 1});
	if (!WaitForMainQueueBarrier()
		|| !WaitForSessionSwitchUpdatesForTest(
			stock->session(),
			SessionSwitchUpdateCountForTest(pinnedToStockStockUpdates) + 1,
			pinned->session(),
			SessionSwitchUpdateCountForTest(pinnedToStockPinnedUpdates) + 1)
		|| !pinnedToStockDeferredObserved || !pinnedToStockDeferredMatches) {
		return failPrerequisite(
			"pinned-to-stock switch did not update the previous session once");
	}
	const auto discarded = domain.add(MTP::Environment::Production);
	discarded->mtp().stopForServerEnrollment();
	discarded->setSessionUserId(selfId);
	// createSession() must persist the MTP config, so keep network access
	// stopped until the session exists, then block it before any switch update
	// can be dispatched.
	if (!discarded->createSession(RegressionUser(selfId, true, QString()),
								  std::make_unique<Main::SessionSettings>())) {
		return failPrerequisite(
			"could not create the previous-session teardown fixture");
	}
	discarded->mtp().dcOptions().constructBlocked();
	if (!discarded->mtp().dcOptions().blocked()
		|| discarded->mtp().isServerEnrollmentNetworkAllowed()) {
		return failPrerequisite(
			"previous-session teardown fixture was not blocked and paused");
	}
	if (discarded->session().uniqueId() != stock->session().uniqueId()) {
		return failPrerequisite(
			"previous-session teardown fixture did not share the stock user "
			"id");
	}
	if (discarded->session().aiComposeSupported()
		|| !RunAiComposeApplySwitchRegression(primary, stock, pinned, discarded)
		|| primary->maybeSession() != &stock->session()) {
		return failPrerequisite(
			"AI apply handlers read or dispatched draft text for an "
			"unsupported "
			"session, or stopped working after switching back to stock");
	}
	if (!WaitForMainQueueBarrier()) {
		return failPrerequisite(
			"main-thread queue did not drain after blocked-session setup");
	}
	const auto stockSetupDispatchesBeforeQueuedSwitches
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto pinnedSetupDispatchesBeforeQueuedSwitches
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto stockSetupPending = DeferredOnlineUpdateDispatchOutstanding(
		stockSetupDispatchesBeforeQueuedSwitches,
		DeferredOnlineUpdatePhase::Setup);
	const auto pinnedSetupPending = DeferredOnlineUpdateDispatchOutstanding(
		pinnedSetupDispatchesBeforeQueuedSwitches,
		DeferredOnlineUpdatePhase::Setup);
	if (stockSetupPending || pinnedSetupPending) {
		return failPrerequisite(
			"setup switch callbacks were pending after the main queue barrier");
	}
	const auto queuedDispatchesRemainBounded = [&](const char *observation) {
		const auto stockCurrent
			= stock->session()
				  .updates()
				  .deferredOnlineUpdateDispatchCountsForRegressionTest();
		const auto pinnedCurrent
			= pinned->session()
				  .updates()
				  .deferredOnlineUpdateDispatchCountsForRegressionTest();
		const auto stockDelta = DeferredOnlineUpdateDispatchDelta(
			stockCurrent, stockSetupDispatchesBeforeQueuedSwitches);
		const auto pinnedDelta = DeferredOnlineUpdateDispatchDelta(
			pinnedCurrent, pinnedSetupDispatchesBeforeQueuedSwitches);
		ReportDeferredOnlineUpdateDispatchDeltas(observation, stockDelta,
												 pinnedDelta);
		return (stockDelta.setupEnqueued == 1)
			   && (pinnedDelta.setupEnqueued == 1)
			   && (stockDelta.closeSwitchEnqueued == 0)
			   && (pinnedDelta.closeSwitchEnqueued == 0);
	};
	const auto stockBeforeQueuedSwitches
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedBeforeQueuedSwitches
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	const auto discardedBeforeQueuedSwitches
		= discarded->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(pinned);
	const auto stockQueuedFirst = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeQueuedSwitches);
	const auto pinnedQueuedFirst = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeQueuedSwitches);
	const auto discardedQueuedFirst = OnlineUpdateDelta(
		discarded->session().updates().onlineUpdateCountsForRegressionTest(),
		discardedBeforeQueuedSwitches);
	const auto queuedFirstMatches = ReportSwitchUpdateDeltas(
		"queued stock-to-pinned inline", stockQueuedFirst, {0, 0},
		pinnedQueuedFirst, {1, 0}, &discardedQueuedFirst, "discarded", {0, 0});
	if (primary->maybeSession() != &pinned->session() || !queuedFirstMatches
		|| stockQueuedFirst.total != 0 || pinnedQueuedFirst.total != 1
		|| discardedQueuedFirst.total != 0) {
		return failPrerequisite(
			"queued stock-to-pinned switch missed its inline session update");
	}
	primary->showAccount(discarded);
	const auto stockQueuedSecond = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeQueuedSwitches);
	const auto pinnedQueuedSecond = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeQueuedSwitches);
	const auto discardedQueuedSecond = OnlineUpdateDelta(
		discarded->session().updates().onlineUpdateCountsForRegressionTest(),
		discardedBeforeQueuedSwitches);
	const auto queuedSecondMatches = ReportSwitchUpdateDeltas(
		"queued pinned-to-teardown inline", stockQueuedSecond, {0, 0},
		pinnedQueuedSecond, {1, 0}, &discardedQueuedSecond, "discarded",
		{1, 0});
	if (primary->maybeSession() != &discarded->session() || !queuedSecondMatches
		|| stockQueuedSecond.total != 0 || pinnedQueuedSecond.total != 1
		|| discardedQueuedSecond.total != 1) {
		return failPrerequisite(
			"queued pinned-to-teardown switch missed its inline session "
			"update");
	}
	primary->showAccount(stock);
	const auto stockQueuedThird = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeQueuedSwitches);
	const auto pinnedQueuedThird = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeQueuedSwitches);
	const auto discardedQueuedThird = OnlineUpdateDelta(
		discarded->session().updates().onlineUpdateCountsForRegressionTest(),
		discardedBeforeQueuedSwitches);
	const auto queuedThirdMatches = ReportSwitchUpdateDeltas(
		"queued teardown-to-stock inline", stockQueuedThird, {1, 0},
		pinnedQueuedThird, {1, 0}, &discardedQueuedThird, "discarded", {1, 0});
	if (primary->maybeSession() != &stock->session() || !queuedThirdMatches
		|| stockQueuedThird.total != 1 || pinnedQueuedThird.total != 1
		|| discardedQueuedThird.total != 1) {
		return failPrerequisite(
			"queued teardown-to-stock switch missed its inline session update");
	}
	if (!queuedDispatchesRemainBounded("after queued switch fixture")) {
		return failPrerequisite(
			"queued switch fixture enqueued an extra live-session update");
	}
	if (fallbackAccountLifetime.get() != fallbackAccount.get()
		|| fallbackAccount->sessionExists()
		|| app.separateWindowFor(fallbackAccount) != fallbackAccountWindow) {
		return failPrerequisite(
			"preserved logout fallback account did not survive setup");
	}
	const auto pinnedFallbackWindowBefore = app.separateWindowFor(pinned);
	if (pinnedFallbackWindowBefore) {
		return failPrerequisite(
			"pinned account already had a primary window before isolation");
	}
	const auto pinnedWindowSetupBefore
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	const auto stockWindowSetupBefore
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto discardedWindowSetupBefore
		= discarded->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedWindowDispatchesBefore
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto stockWindowDispatchesBefore
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	pinnedFallbackWindow = app.ensureSeparateWindowFor(pinned);
	const auto pinnedFallbackWindowSetupInline = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedWindowSetupBefore);
	const auto stockFallbackWindowSetupInline = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockWindowSetupBefore);
	const auto discardedFallbackWindowSetupInline = OnlineUpdateDelta(
		discarded->session().updates().onlineUpdateCountsForRegressionTest(),
		discardedWindowSetupBefore);
	const auto pinnedFallbackWindowSetupInlineMatches
		= ReportSwitchUpdateDeltas("pinned fallback window setup inline",
								   stockFallbackWindowSetupInline, {0, 0},
								   pinnedFallbackWindowSetupInline, {1, 0},
								   &discardedFallbackWindowSetupInline,
								   "discarded", {0, 0});
	const auto pinnedWindowDispatchesAfter
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto stockWindowDispatchesAfter
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto pinnedFallbackWindowDispatchDelta
		= DeferredOnlineUpdateDispatchDelta(pinnedWindowDispatchesAfter,
											pinnedWindowDispatchesBefore);
	const auto stockFallbackWindowDispatchDelta
		= DeferredOnlineUpdateDispatchDelta(stockWindowDispatchesAfter,
											stockWindowDispatchesBefore);
	ReportDeferredOnlineUpdateDispatchDeltas("pinned fallback window setup",
											 stockFallbackWindowDispatchDelta,
											 pinnedFallbackWindowDispatchDelta);
	const auto pinnedFallbackWindowDispatchesMatch
		= (stockFallbackWindowDispatchDelta.setupEnqueued == 0)
		  && (stockFallbackWindowDispatchDelta.closeSwitchEnqueued == 0)
		  && (pinnedFallbackWindowDispatchDelta.setupEnqueued == 1)
		  && (pinnedFallbackWindowDispatchDelta.closeSwitchEnqueued == 0);
	if (!pinnedFallbackWindowSetupInlineMatches
		|| !pinnedFallbackWindowDispatchesMatch) {
		return failPrerequisite(
			"pinned fallback window setup did not match its separate update");
	}
	primary->activate();
	if (!pinnedFallbackWindow || !fallbackAccountWindow
		|| !pinnedFallbackWindow->isPrimary()
		|| app.separateWindowFor(pinned) != pinnedFallbackWindow
		|| !fallbackAccountWindow->isPrimary()
		|| app.separateWindowFor(fallbackAccount) != fallbackAccountWindow
		|| app.separateWindowFor(stock) != primary
		|| app.activePrimaryWindow() != primary
		|| primary->maybeSession() != &stock->session()) {
		return failPrerequisite(
			"could not stabilize logout fallback windows before teardown");
	}
	const auto stockAfterFallbackWindowSetup
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto pinnedAfterFallbackWindowSetup
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	ReportDeferredOnlineUpdateDispatchCounts(
		"before destroyed-session teardown", stockAfterFallbackWindowSetup,
		pinnedAfterFallbackWindowSetup);
	if (DeferredOnlineUpdateDispatchOutstanding(
			stockAfterFallbackWindowSetup, DeferredOnlineUpdatePhase::Setup)
			!= 1
		|| DeferredOnlineUpdateDispatchOutstanding(
			   pinnedAfterFallbackWindowSetup, DeferredOnlineUpdatePhase::Setup)
			   != 2) {
		return failPrerequisite("queued switch callbacks did not remain "
								"pending through window setup");
	}
	const auto teardownDispatchesRemainBounded = [&](const char *observation) {
		const auto stockCurrent
			= stock->session()
				  .updates()
				  .deferredOnlineUpdateDispatchCountsForRegressionTest();
		const auto pinnedCurrent
			= pinned->session()
				  .updates()
				  .deferredOnlineUpdateDispatchCountsForRegressionTest();
		const auto stockDelta = DeferredOnlineUpdateDispatchDelta(
			stockCurrent, stockAfterFallbackWindowSetup);
		const auto pinnedDelta = DeferredOnlineUpdateDispatchDelta(
			pinnedCurrent, pinnedAfterFallbackWindowSetup);
		ReportDeferredOnlineUpdateDispatchDeltas(observation, stockDelta,
												 pinnedDelta);
		return (stockDelta.setupEnqueued == 0)
			   && (pinnedDelta.setupEnqueued == 0)
			   && (stockDelta.closeSwitchEnqueued == 0)
			   && (pinnedDelta.closeSwitchEnqueued == 0);
	};
	auto closeSwitchPhase = ScopedDeferredOnlineUpdatePhase(
		DeferredOnlineUpdatePhase::CloseSwitch);
	const auto discardedAtTeardown = discardedQueuedThird;
	const auto discardedWindow = app.ensureSeparateWindowFor(Window::SeparateId(
		Window::SeparateType::Archive, &discarded->session()));
	if (!teardownDispatchesRemainBounded("after discarded window creation")) {
		return failPrerequisite(
			"discarded window creation enqueued an extra live-session update");
	}
	const auto discardedController
		= discardedWindow ? discardedWindow->sessionController() : nullptr;
	if (!discardedController || discardedWindow->isPrimary()) {
		return failPrerequisite(
			"could not create a non-primary controller for deferred "
			"call-link teardown");
	}
	const auto destroyedSessionChannelId = ChannelId(3051);
	PrepareRegressionChannel(
		discarded->session().data().channel(destroyedSessionChannelId));
	const auto discardedLifetime = base::make_weak(discarded.get());
	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("destroyed-session link begin");
	discardedController->showPeerByLink(Window::PeerByLinkInfo{
		.usernameOrId = destroyedSessionChannelId,
		.voicechatHash = u"destroyed-session"_q,
	});
	ReportCallRegressionCheckpoint("destroyed-session link dispatched");
	if (!teardownDispatchesRemainBounded(
			"after destroyed-session link dispatch")) {
		return failPrerequisite(
			"destroyed-session link dispatch enqueued an extra "
			"live-session update");
	}
	const auto discardedServerCacheBindingFiles
		= discarded->local().serverCacheBindingFilesForRegressionTest();
	discarded->forcedLogOut();
	if (!teardownDispatchesRemainBounded("after destroyed-session logout")) {
		return failPrerequisite(
			"destroyed-session logout enqueued an extra live-session update");
	}
	if (discarded->sessionExists() || &domain.active() != stock.get()
		|| app.activePrimaryWindow() != primary
		|| primary->maybeSession() != &stock->session()) {
		return failPrerequisite(
			"logout changed stock primary ownership or kept the teardown "
			"session alive");
	}
	ReportCallRegressionCheckpoint("destroyed-session logout complete");
	if (!WaitForMainQueueBarrier()) {
		return failPrerequisite(
			"main-thread queue did not drain after call-link session "
			"destruction");
	}
	if (!WaitForBindingFilesRemovedForRegression(
			discardedServerCacheBindingFiles)) {
		return failPrerequisite(
			"previous-session logout did not clear its cache binding");
	}
	QCoreApplication::processEvents();
	ReportCallRegressionCheckpoint("destroyed-session events processed");
	closeSwitchPhase.restore();
	if (!teardownDispatchesRemainBounded(
			"after destroyed-session event processing")) {
		return failPrerequisite(
			"destroyed-session event processing enqueued an extra "
			"live-session update");
	}
	if (!discardedLifetime.empty()) {
		return failPrerequisite(
			"previous-session account survived after its window closed");
	}
	const auto queuedStockDeferredObserved = WaitForDeferredSwitchUpdate(
		stock->session(), stockBeforeQueuedSwitches.switchDeferred + 1);
	const auto queuedPinnedDeferredObserved = WaitForDeferredSwitchUpdate(
		pinned->session(), pinnedBeforeQueuedSwitches.switchDeferred + 2);
	const auto stockQueuedDeferred = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeQueuedSwitches);
	auto pinnedQueuedDeferred = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeQueuedSwitches);
	pinnedQueuedDeferred.total
		-= pinnedFallbackWindowSetupInline.total
		   + pinnedFallbackWindowDispatchDelta.setupEnqueued;
	pinnedQueuedDeferred.other -= pinnedFallbackWindowSetupInline.other;
	pinnedQueuedDeferred.switchInline
		-= pinnedFallbackWindowSetupInline.switchInline;
	pinnedQueuedDeferred.switchDeferred
		-= pinnedFallbackWindowDispatchDelta.setupEnqueued;
	const auto queuedDeferredMatches = ReportSwitchUpdateDeltas(
		"queued switch teardown deferred", stockQueuedDeferred, {1, 1},
		pinnedQueuedDeferred, {1, 1}, &discardedAtTeardown,
		"discarded-at-teardown", {1, 0});
	const auto stockQueuedDispatches = DeferredOnlineUpdateDispatchDelta(
		stock->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		stockSetupDispatchesBeforeQueuedSwitches);
	const auto pinnedQueuedDispatches = DeferredOnlineUpdateDispatchDelta(
		pinned->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		pinnedSetupDispatchesBeforeQueuedSwitches);
	ReportDeferredOnlineUpdateDispatchDeltas(
		"queued switch teardown dispatches", stockQueuedDispatches,
		pinnedQueuedDispatches);
	const auto queuedDispatchCountsMatch
		= (stockQueuedDispatches.setupEnqueued == 1)
		  && (stockQueuedDispatches.setupExecuted == 1)
		  && (stockQueuedDispatches.closeSwitchEnqueued == 0)
		  && (stockQueuedDispatches.closeSwitchExecuted == 0)
		  && (pinnedQueuedDispatches.setupEnqueued == 2)
		  && (pinnedQueuedDispatches.setupExecuted == 2)
		  && (pinnedQueuedDispatches.closeSwitchEnqueued == 0)
		  && (pinnedQueuedDispatches.closeSwitchExecuted == 0);
	const auto destroyedSessionEvents = GetCallStartRegressionSnapshotForTest();
	if (!destroyedSessionEvents.navigationEvents.empty()
		|| !destroyedSessionEvents.openedPeers.empty()) {
		return failPrerequisite(
			"deferred call-link callback ran after session destruction");
	}
	if (!WaitForMainQueueBarrier()
		|| !WaitForSessionSwitchUpdatesForTest(
			stock->session(),
			SessionSwitchUpdateCountForTest(stockBeforeQueuedSwitches) + 2,
			pinned->session(),
			SessionSwitchUpdateCountForTest(pinnedBeforeQueuedSwitches) + 2
				+ pinnedFallbackWindowSetupInline.switchInline
				+ pinnedFallbackWindowDispatchDelta.setupEnqueued)
		|| !queuedStockDeferredObserved || !queuedPinnedDeferredObserved
		|| !queuedDeferredMatches || !queuedDispatchCountsMatch) {
		return failPrerequisite(
			"queued live and destroyed-session updates reached the wrong "
			"sessions");
	}
	app.closeWindow(fallbackAccountWindow);
	app.closeWindow(pinnedFallbackWindow);
	fallbackWindowsOpen = false;
	primary->activate();
	domain.removeRedundantAccounts();
	if (app.separateWindowFor(pinned) != nullptr
		|| !fallbackAccountLifetime.empty()
		|| app.activePrimaryWindow() != primary
		|| primary->maybeSession() != &stock->session()
		|| !WaitForMainQueueBarrier()) {
		return failPrerequisite(
			"could not restore the stock primary after logout fallback setup");
	}
	const auto stockDcOptionsBeforeDeferredLink
		= MTP::DcOptions(MTP::Environment::Production).serialize();
	stock->mtp().dcOptions().constructBlocked();
	const auto replacedController = primary->sessionController();
	if (!replacedController) {
		return failPrerequisite(
			"primary stock controller disappeared before deferred link test");
	}
	const auto replacedControllerWeak = base::make_weak(replacedController);
	const auto replacedSessionChannelId = ChannelId(3052);
	PrepareRegressionChannel(
		stock->session().data().channel(replacedSessionChannelId));
	ResetCallStartRegressionForTest();
	ReportCallRegressionCheckpoint("replaced-session link begin");
	replacedController->showPeerByLink(Window::PeerByLinkInfo{
		.usernameOrId = replacedSessionChannelId,
		.voicechatHash = u"replaced-session"_q,
	});
	ReportCallRegressionCheckpoint("replaced-session link dispatched");
	primary->showAccount(pinned);
	primary->showAccount(stock);
	stock->mtp().dcOptions().constructUnenrolled();
	if (!stock->mtp().dcOptions().constructFromSerialized(
			stockDcOptionsBeforeDeferredLink)) {
		return failPrerequisite(
			"could not restore stock DC options after deferred link test");
	}
	if (!WaitForMainQueueBarrier()) {
		return failPrerequisite(
			"main-thread queue did not drain after call-link session "
			"replacement");
	}
	QCoreApplication::processEvents();
	ReportCallRegressionCheckpoint("replaced-session events processed");
	if (replacedControllerWeak.get()
		|| primary->maybeSession() != &stock->session()
		|| !GetCallStartRegressionSnapshotForTest().navigationEvents.empty()
		|| !GetCallStartRegressionSnapshotForTest().openedPeers.empty()) {
		return failPrerequisite(
			"deferred call-link callback ran after session replacement");
	}
	auto pinnedWindow = app.ensureSeparateWindowFor(pinned);
	if (app.separateWindowFor(pinned) != pinnedWindow) {
		return failPrerequisite(
			"pinned window was not mapped before the primary closed");
	}
	if (!stock->sessionExists() || !pinned->sessionExists()) {
		return failPrerequisite(
			"live previous-session fixtures disappeared before primary close");
	}
	// The pinned account owns a window, so the pinned id is taken. Switching
	// the primary here must not rebind it: the pinned window is shown and the
	// primary stays on the stock account. Every account lookup must keep
	// returning a window bound to the account it was asked for, and every
	// window must stay registered under its own id.
	const auto stockBeforeCollision
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedBeforeCollision
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	primary->showAccount(pinned);
	const auto stockCollisionInline = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeCollision);
	const auto pinnedCollisionInline = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeCollision);
	const auto collisionInlineMatches = ReportSwitchUpdateDeltas(
		"window collision inline", stockCollisionInline, {0, 0},
		pinnedCollisionInline, {0, 0});
	const auto stockLookup = app.windowFor(stock);
	const auto pinnedLookup = app.windowFor(pinned);
	if (primary->maybeSession() != &stock->session()
		|| app.activePrimaryWindow() != pinnedWindow || stockLookup != primary
		|| pinnedLookup != pinnedWindow
		|| &stockLookup->account() != stock.get()
		|| &pinnedLookup->account() != pinned.get()
		|| app.separateWindowFor(stock) != primary
		|| app.separateWindowFor(pinned) != pinnedWindow
		|| app.separateWindowFor(primary->id()) != primary
		|| app.separateWindowFor(pinnedWindow->id()) != pinnedWindow
		|| !collisionInlineMatches || stockCollisionInline.total != 0
		|| pinnedCollisionInline.total != 0) {
		return failPrerequisite(
			"primary switch to an account owning a window left an account "
			"lookup on a window bound to another account");
	}
	QCoreApplication::processEvents();
	const auto stockCollisionDeferred = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeCollision);
	const auto pinnedCollisionDeferred = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeCollision);
	// clang-format off
	const auto collisionDeferredMatches = ReportSwitchUpdateDeltas(
		"window collision after event processing",
		stockCollisionDeferred,
		{0, 0},
		pinnedCollisionDeferred,
		{0, 0});
	// clang-format on
	if (!collisionDeferredMatches) {
		return failPrerequisite(
			"window collision produced a switch-caused online update");
	}
	// Free the pinned id, then run the queued-switch teardown on a switch that
	// is allowed: the previous-session update is queued for the stock session,
	// the originating window closes synchronously before the dispatch, and the
	// queued update must reach the stock session once, with no deferred update
	// of its own for the newly shown session.
	app.closeWindow(pinnedWindow);
	if (app.separateWindowFor(pinned) != nullptr
		|| app.separateWindowFor(stock) != primary
		|| app.activePrimaryWindow() != primary) {
		return failPrerequisite(
			"closed account window stayed mapped to the pinned account");
	}
	// A window for a brand-new account, which has no session, keeps one window
	// registered while the originating window closes. A new account cannot have
	// a window yet, so its id is free, and with no session behind it that
	// window cannot answer an online update: the stock count dispatched below
	// is the queued previous-session update and nothing else.
	const auto blank = domain.add(MTP::Environment::Production);
	blank->mtp().stopForServerEnrollment();
	const auto blankWindow = app.ensureSeparateWindowFor(blank);
	if (app.separateWindowFor(blank) != blankWindow
		|| blankWindow->sessionController() != nullptr) {
		// clang-format off
		std::fprintf(
			stderr,
			"Blank window fixture: mapped=%d controller=%p\n",
			app.separateWindowFor(blank) == blankWindow,
			static_cast<void *>(blankWindow->sessionController()));
		// clang-format on
		return failPrerequisite(
			"blank window fixture was not mapped before the close");
	}
	domain.activate(blank.get());
	if (&domain.active() != blank.get()) {
		return failPrerequisite(
			"blank account was not active before the primary close");
	}
	const auto stockSetupBeforeCompletion
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto pinnedSetupBeforeCompletion
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	// clang-format off
	ReportDeferredOnlineUpdateDispatchCounts(
		"setup before bounded completion",
		stockSetupBeforeCompletion,
		pinnedSetupBeforeCompletion);
	// clang-format on
	const auto stockSetupOnlineUpdatesBeforeCompletion
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedSetupOnlineUpdatesBeforeCompletion
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	const auto setupDispatchesSettled
		// clang-format off
		= WaitForDeferredOnlineUpdateDispatchesToSettle(
			stock->session(),
			pinned->session(),
			DeferredOnlineUpdatePhase::Setup);
	// clang-format on
	const auto stockCloseSwitchDispatchBaseline
		= stock->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	const auto pinnedCloseSwitchDispatchBaseline
		= pinned->session()
			  .updates()
			  .deferredOnlineUpdateDispatchCountsForRegressionTest();
	// clang-format off
	ReportDeferredOnlineUpdateDispatchCounts(
		"close baseline after bounded setup completion",
		stockCloseSwitchDispatchBaseline,
		pinnedCloseSwitchDispatchBaseline);
	// clang-format on
	const auto stockSetupOutstandingAtBaseline
		= DeferredOnlineUpdateDispatchOutstanding(
			stockCloseSwitchDispatchBaseline, DeferredOnlineUpdatePhase::Setup);
	const auto pinnedSetupOutstandingAtBaseline
		= DeferredOnlineUpdateDispatchOutstanding(
			pinnedCloseSwitchDispatchBaseline,
			DeferredOnlineUpdatePhase::Setup);
	// clang-format off
	std::fprintf(
		stderr,
		"Deferred online callback setup observation: settled=%d "
		"deadline_ms=%d outstanding={stock=%d pinned=%d}\n",
		setupDispatchesSettled,
		kDeferredSwitchDispatchDeadlineMs,
		stockSetupOutstandingAtBaseline,
		pinnedSetupOutstandingAtBaseline);
	// clang-format on
	if (!setupDispatchesSettled || stockSetupOutstandingAtBaseline != 0
		|| pinnedSetupOutstandingAtBaseline != 0) {
		const auto stockSetupOnlineUpdates = OnlineUpdateDelta(
			stock->session().updates().onlineUpdateCountsForRegressionTest(),
			stockSetupOnlineUpdatesBeforeCompletion);
		const auto pinnedSetupOnlineUpdates = OnlineUpdateDelta(
			pinned->session().updates().onlineUpdateCountsForRegressionTest(),
			pinnedSetupOnlineUpdatesBeforeCompletion);
		const auto stockSetupExpectedDeferred
			= stockCloseSwitchDispatchBaseline.setupEnqueued
			  - stockSetupBeforeCompletion.setupExecuted;
		const auto pinnedSetupExpectedDeferred
			= pinnedCloseSwitchDispatchBaseline.setupEnqueued
			  - pinnedSetupBeforeCompletion.setupExecuted;
		// clang-format off
		const auto setupOnlineUpdatesMatch = ReportSwitchUpdateDeltas(
			"setup completion before close baseline",
			stockSetupOnlineUpdates,
			{0, stockSetupExpectedDeferred},
			pinnedSetupOnlineUpdates,
			{0, pinnedSetupExpectedDeferred});
		// clang-format on
		if (!setupOnlineUpdatesMatch) {
			std::fprintf(stderr,
						 "Setup callback update deltas did not match the "
						 "deferred dispatch counts.\n");
		}
		return failPrerequisite(
			"setup deferred switch callbacks remained outstanding at close "
			"baseline");
	}
	const auto stockBeforeCloseSwitch
		= stock->session().updates().onlineUpdateCountsForRegressionTest();
	const auto pinnedBeforeCloseSwitch
		= pinned->session().updates().onlineUpdateCountsForRegressionTest();
	auto closeDispatchesSettled = false;
	auto closeSwitchEnqueuesMatch = false;
	auto stockAfterCloseSwitchDispatches = DeferredOnlineUpdateDispatchCounts();
	auto pinnedAfterCloseSwitchDispatches
		= DeferredOnlineUpdateDispatchCounts();
	auto phase = ScopedDeferredOnlineUpdatePhase(
		DeferredOnlineUpdatePhase::CloseSwitch);
	primary->showAccount(pinned);
	stockAfterCloseSwitchDispatches = DeferredOnlineUpdateDispatchDelta(
		stock->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		stockCloseSwitchDispatchBaseline);
	pinnedAfterCloseSwitchDispatches = DeferredOnlineUpdateDispatchDelta(
		pinned->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		pinnedCloseSwitchDispatchBaseline);
	// clang-format off
	ReportDeferredOnlineUpdateDispatchCounts(
		"after close switch",
		stockAfterCloseSwitchDispatches,
		pinnedAfterCloseSwitchDispatches);
	// clang-format on
	closeSwitchEnqueuesMatch
		= stockAfterCloseSwitchDispatches.setupEnqueued == 0
		  && stockAfterCloseSwitchDispatches.setupExecuted == 0
		  && stockAfterCloseSwitchDispatches.closeSwitchEnqueued == 1
		  && stockAfterCloseSwitchDispatches.closeSwitchExecuted == 0
		  && pinnedAfterCloseSwitchDispatches.setupEnqueued == 0
		  && pinnedAfterCloseSwitchDispatches.setupExecuted == 0
		  && pinnedAfterCloseSwitchDispatches.closeSwitchEnqueued == 0
		  && pinnedAfterCloseSwitchDispatches.closeSwitchExecuted == 0;
	const auto stockCloseSwitchInline = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeCloseSwitch);
	const auto pinnedCloseSwitchInline = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeCloseSwitch);
	// clang-format off
	const auto closeSwitchInlineMatches = ReportSwitchUpdateDeltas(
		"stock-to-pinned close-switch inline",
		stockCloseSwitchInline,
		{0, 0},
		pinnedCloseSwitchInline,
		{1, 0});
	// clang-format on
	if (primary->maybeSession() != &pinned->session()
		|| !closeSwitchInlineMatches || stockCloseSwitchInline.total != 0
		|| pinnedCloseSwitchInline.total != 1) {
		return failPrerequisite(
			"stock-to-pinned close switch missed its inline session update");
	}
	// The switch is registered under the pinned id, not the stock one it came
	// from, so no lookup can answer with a window of another account.
	if (app.separateWindowFor(pinned) != primary
		|| app.separateWindowFor(stock) != nullptr
		|| app.separateWindowFor(primary->id()) != primary
		|| &primary->account() != pinned.get()) {
		return failPrerequisite(
			"allowed primary switch kept the window keyed to its old account");
	}
	// The close is synchronous, before any dispatch of the queued update.
	app.closeWindow(primary);
	if (app.separateWindowFor(pinned) != nullptr
		|| app.separateWindowFor(stock) != nullptr
		|| app.separateWindowFor(blank) != blankWindow) {
		return failPrerequisite(
			"closed primary window remained mapped to an account");
	}
	// clang-format off
	closeDispatchesSettled = WaitForDeferredOnlineUpdateDispatchesToSettle(
		stock->session(),
		pinned->session(),
		DeferredOnlineUpdatePhase::CloseSwitch);
	// clang-format on
	phase.restore();
	if (&domain.active() != blank.get()) {
		return failPrerequisite(
			"primary close changed the active blank account");
	}
	const auto stockCloseSwitchDispatches = DeferredOnlineUpdateDispatchDelta(
		stock->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		stockCloseSwitchDispatchBaseline);
	const auto pinnedCloseSwitchDispatches = DeferredOnlineUpdateDispatchDelta(
		pinned->session()
			.updates()
			.deferredOnlineUpdateDispatchCountsForRegressionTest(),
		pinnedCloseSwitchDispatchBaseline);
	// clang-format off
	ReportDeferredOnlineUpdateDispatchCounts(
		"after primary close and bounded dispatch drain",
		stockCloseSwitchDispatches,
		pinnedCloseSwitchDispatches);
	// clang-format on
	const auto stockCloseCompletionDispatches
		= DeferredOnlineUpdateDispatchDelta(stockCloseSwitchDispatches,
											stockAfterCloseSwitchDispatches);
	const auto pinnedCloseCompletionDispatches
		= DeferredOnlineUpdateDispatchDelta(pinnedCloseSwitchDispatches,
											pinnedAfterCloseSwitchDispatches);
	// clang-format off
	ReportDeferredOnlineUpdateDispatchCounts(
		"close completion after close switch",
		stockCloseCompletionDispatches,
		pinnedCloseCompletionDispatches);
	// clang-format on
	const auto closeCompletionDispatchCountsMatch
		= stockCloseCompletionDispatches.setupEnqueued == 0
		  && stockCloseCompletionDispatches.setupExecuted == 0
		  && stockCloseCompletionDispatches.closeSwitchEnqueued == 0
		  && stockCloseCompletionDispatches.closeSwitchExecuted == 1
		  && pinnedCloseCompletionDispatches.setupEnqueued == 0
		  && pinnedCloseCompletionDispatches.setupExecuted == 0
		  && pinnedCloseCompletionDispatches.closeSwitchEnqueued == 0
		  && pinnedCloseCompletionDispatches.closeSwitchExecuted == 0;
	const auto closeDispatchCountsMatch
		= stockCloseSwitchDispatches.setupEnqueued == 0
		  && stockCloseSwitchDispatches.setupExecuted == 0
		  && stockCloseSwitchDispatches.closeSwitchEnqueued == 1
		  && stockCloseSwitchDispatches.closeSwitchExecuted == 1
		  && pinnedCloseSwitchDispatches.setupEnqueued == 0
		  && pinnedCloseSwitchDispatches.setupExecuted == 0
		  && pinnedCloseSwitchDispatches.closeSwitchEnqueued == 0
		  && pinnedCloseSwitchDispatches.closeSwitchExecuted == 0;
	// clang-format off
	std::fprintf(
		stderr,
		"Deferred callback attribution: close_phase=%s source=%s\n",
		closeDispatchCountsMatch ? "expected-counts" : "unexpected-counts",
		closeDispatchCountsMatch ? "accounted-for" : "unresolved");
	// clang-format on
	const auto stockCloseSwitchDeferred = OnlineUpdateDelta(
		stock->session().updates().onlineUpdateCountsForRegressionTest(),
		stockBeforeCloseSwitch);
	const auto pinnedCloseSwitchDeferred = OnlineUpdateDelta(
		pinned->session().updates().onlineUpdateCountsForRegressionTest(),
		pinnedBeforeCloseSwitch);
	// clang-format off
	const auto closeSwitchDeferredMatches = ReportSwitchUpdateDeltas(
		"stock-to-pinned close-switch deferred",
		stockCloseSwitchDeferred,
		{0, 1},
		pinnedCloseSwitchDeferred,
		{1, 0});
	// clang-format on
	if (!closeDispatchesSettled || !closeSwitchEnqueuesMatch
		|| !closeCompletionDispatchCountsMatch || !closeDispatchCountsMatch
		|| !closeSwitchDeferredMatches) {
		return failPrerequisite(
			"deferred primary-close update was not delivered once "
			"to the previous session with no pinned-session dispatch");
	}
	pinnedWindow = app.ensureSeparateWindowFor(pinned);
	if (app.separateWindowFor(pinned) != pinnedWindow
		|| &pinnedWindow->account() != pinned.get()) {
		return failPrerequisite(
			"pinned window was not remapped after the primary closed");
	}

	const auto stockWindow = app.ensureSeparateWindowFor(stock);
	Window::Controller *restoredBlockedWindow = nullptr;
	const auto closeWindows = gsl::finally([&] {
		if (stockWindow && app.separateWindowFor(stock) == stockWindow) {
			app.closeWindow(stockWindow);
		}
		if (pinnedWindow && app.separateWindowFor(pinned) == pinnedWindow) {
			app.closeWindow(pinnedWindow);
		}
		if (blankWindow && app.separateWindowFor(blank) == blankWindow) {
			app.closeWindow(blankWindow);
		}
		if (restoredBlockedWindow
			&& app.separateWindowFor(restoredBlockedWindow->id())
				   == restoredBlockedWindow) {
			app.closeWindow(restoredBlockedWindow);
		}
		domain.activate(stock);
	});
	const auto printCapabilities = [](const char *name,
									  const Main::Session &session) {
		std::fprintf(
			stderr, "%s capabilities=%d%d%d%d%d%d%d%d%d%d%d%d%d%d\n", name,
			session.callsSupported(), session.botAppsSupported(),
			session.paidFeaturesSupported(), session.storiesSupported(),
			session.exportSupported(), session.passportSupported(),
			session.aiComposeSupported(), session.serverTranslationSupported(),
			session.sharedFoldersSupported(), session.accountBioEditSupported(),
			session.accountUsernameEditSupported(),
			Main::details::noForwardsSupportedForPeer(session.user()),
			Main::details::chatThemeEditingSupportedForPeer(session.user()),
			Main::details::scheduledMessagesSupportedForPeer(session.user()));
	};
	const auto windowsMatch = [&](const char *stage) {
		const auto stockController = stockWindow->sessionController();
		const auto pinnedController = pinnedWindow->sessionController();
		const auto stockMapped = app.separateWindowFor(stock) == stockWindow;
		const auto pinnedMapped = app.separateWindowFor(pinned) == pinnedWindow;
		const auto stockBound = &stockWindow->account() == stock.get();
		const auto pinnedBound = &pinnedWindow->account() == pinned.get();
		const auto stockSessionMatches
			= stockController
			  && (&stockController->session() == &stock->session());
		const auto pinnedSessionMatches
			= pinnedController
			  && (&pinnedController->session() == &pinned->session());
		const auto stockCapabilitiesMatch
			= stockController
			  && capabilitiesMatch(stockController->session(), true);
		const auto pinnedCapabilitiesMatch
			= pinnedController
			  && capabilitiesMatch(pinnedController->session(), false);
		const auto stockSearchMatches = SettingsSearchMatchesExpected(
			&stock->session(), true, true, true);
		const auto pinnedSearchMatches = SettingsSearchMatchesExpected(
			&pinned->session(), false, false, false);
		const auto stockSearchStillMatches = SettingsSearchMatchesExpected(
			&stock->session(), true, true, true);
		const auto matches = (stockWindow != pinnedWindow) && stockMapped
							 && pinnedMapped && stockBound && pinnedBound
							 && stockSessionMatches && pinnedSessionMatches
							 && stockCapabilitiesMatch
							 && pinnedCapabilitiesMatch && stockSearchMatches
							 && pinnedSearchMatches && stockSearchStillMatches;
		if (!matches) {
			std::fprintf(stderr,
						 "Window/session regression mismatch at %s: "
						 "distinct=%d mapped=%d/%d account=%d/%d "
						 "controller=%d/%d session=%d/%d gates=%d/%d "
						 "search=%d/%d/%d active=%p\n",
						 stage, stockWindow != pinnedWindow, stockMapped,
						 pinnedMapped, stockBound, pinnedBound,
						 stockController != nullptr,
						 pinnedController != nullptr, stockSessionMatches,
						 pinnedSessionMatches, stockCapabilitiesMatch,
						 pinnedCapabilitiesMatch, stockSearchMatches,
						 pinnedSearchMatches, stockSearchStillMatches,
						 static_cast<const void *>(&domain.active()));
			printCapabilities("stock account", stock->session());
			printCapabilities("pinned account", pinned->session());
			if (stockController) {
				printCapabilities("stock window", stockController->session());
			}
			if (pinnedController) {
				printCapabilities("pinned window", pinnedController->session());
			}
		}
		return matches;
	};
	const auto customWebViewOpenIsRefused = [&] {
		const auto controller = pinnedWindow->sessionController();
		if (!controller) {
			return false;
		}
		auto &webView = pinned->session().attachWebView();
		const auto before = ReadWebViewOpenCounters(&pinned->session());
		const auto action = Api::SendAction(
			pinned->session().data().history(pinned->session().user()));
		const auto usernameOpened = webView.openByUsername(
			controller, action, u"regression_bot"_q, QString(), false);
		const auto directOpened = webView.open({
			.bot = pinned->session().user(),
			.context = { .controller = controller },
			.source = InlineBots::WebViewSourceGame{
				.title = u"Regression"_q,
			},
		});
		if (usernameOpened || directOpened) {
			webView.cancel();
			webView.closeAll();
			return false;
		}
		// The refusal is the toast and nothing else, at both switching orders:
		// no resolve, no app request, no instance, no activation.
		const auto refused = WebViewOpenDeltaMatches(
			"custom open at an activation order", before,
			ReadWebViewOpenCounters(&pinned->session()), 2, 0, 0, 0, 0);
		if (!refused) {
			std::fprintf(
				stderr,
				"Custom webview open at this activation order: username=%d "
				"direct=%d\n",
				usernameOpened, directOpened);
		}
		return refused;
	};
	const auto activateAndCheck = [&](bool customFirst) {
		const auto first = customFirst ? pinned : stock;
		const auto second = customFirst ? stock : pinned;
		ReportCallRegressionCheckpoint("account activation begin");
		domain.activate(first);
		ReportCallRegressionCheckpoint("first account activated");
		const auto firstWindow = app.separateWindowFor(first);
		const auto firstActive = &domain.active() == first.get();
		const auto firstWindowReady = firstWindow && firstActive;
		const auto firstCapabilities
			= capabilitiesMatch(first->session(), !customFirst);
		const auto firstWindows
			= firstWindowReady
			  && windowsMatch(customFirst ? "custom account first activation"
										  : "stock account first activation");
		const auto firstCallStart = firstWindowReady
									&& RunOutgoingStartSelectionRegression(
										first, firstWindow, !customFirst);
		const auto firstCustomWebViewRefused
			= firstWindowReady && customWebViewOpenIsRefused();
		const auto stockMenuController = stockWindow->sessionController();
		const auto pinnedMenuController = pinnedWindow->sessionController();
		const auto firstBoostMenusMatch
			= firstWindowReady && stockMenuController && pinnedMenuController
			  && RunBoostMenuVisibilityRegression(
				  stockMenuController, true,
				  customFirst ? "stock while custom is active"
							  : "stock while stock is active")
			  && RunBoostMenuVisibilityRegression(
				  pinnedMenuController, false,
				  customFirst ? "custom while custom is active"
							  : "custom while stock is active")
			  && RunStoryControlsVisibilityRegression(
				  stockMenuController, true,
				  customFirst ? "stock while custom is active"
							  : "stock while stock is active")
			  && RunStoryControlsVisibilityRegression(
				  pinnedMenuController, false,
				  customFirst ? "custom while custom is active"
							  : "custom while stock is active");
		if (!firstActive || !firstCapabilities || !firstWindows
			|| !firstCallStart || !firstCustomWebViewRefused
			|| !firstBoostMenusMatch) {
			std::fprintf(stderr,
						 "First activation mismatch: customFirst=%d active=%d "
						 "window=%d capabilities=%d windows=%d "
						 "call-start=%d "
						 "webview-refused=%d boost-menus=%d\n",
						 customFirst, firstActive, firstWindowReady,
						 firstCapabilities, firstWindows, firstCallStart,
						 firstCustomWebViewRefused, firstBoostMenusMatch);
			if (!firstCapabilities) {
				printCapabilities(customFirst ? "pinned first" : "stock first",
								  first->session());
			}
			return false;
		}
		if (!customFirst) {
			Export::Manager manager;
			Export::View::PanelController *stockExportPanel = nullptr;
			auto panelLifetime = rpl::lifetime();
			manager.currentView()
				| rpl::on_next(
					[&](Export::View::PanelController *view) {
						if (view && &view->session() == &stock->session()) {
							stockExportPanel = view;
						}
					},
					panelLifetime);
			manager.start(&stock->session());
			const auto stockExportInProgress
				= manager.inProgress(&stock->session());
			const auto stockExportPanelVisible
				= stockExportPanel && WaitForRegressionCondition([&] {
					  return stockExportPanel->panelVisibleForRegressionTest();
				  });
			if (!stockExportInProgress || !stockExportPanel
				|| !stockExportPanelVisible) {
				// clang-format off
				std::fprintf(
					stderr,
					"Stock export panel start mismatch: in_progress=%d "
					"panel=%d visible=%d\n",
					stockExportInProgress,
					stockExportPanel != nullptr,
					stockExportPanelVisible);
				// clang-format on
				return false;
			}
			stockExportPanel->hidePanelForRegressionTest();
			if (stockExportPanel->panelVisibleForRegressionTest()) {
				// clang-format off
				std::fprintf(
					stderr,
					"Stock export panel remained visible after hide.\n");
				// clang-format on
				return false;
			}
			const auto pinnedSessionController
				= pinnedWindow->sessionController();
			if (!pinnedSessionController) {
				// clang-format off
				std::fprintf(
					stderr,
					"Pinned window controller missing during export check.\n");
				// clang-format on
				return false;
			}
			const auto exportStarts = Export::ExportStartsForRegressionTest();
			const auto refuseCustomStart = [&](auto start) {
				const auto refusalCalls
					= pinnedSessionController
						  ->featureUnavailableOnServerToastCallsForRegressionTest();
				start();
				QCoreApplication::processEvents();
				return manager.inProgress(&stock->session())
					   && !manager.inProgress(&pinned->session())
					   && &domain.active() == first.get() && stockExportPanel
					   && !stockExportPanel->panelVisibleForRegressionTest()
					   && pinnedSessionController
								  ->featureUnavailableOnServerToastCallsForRegressionTest()
							  == refusalCalls + 1
					   && Export::ExportStartsForRegressionTest()
							  == exportStarts;
			};
			const auto peerRefused
				= refuseCustomStart([&] { manager.start(pinnedUserPeer); });
			const auto topicRefused = refuseCustomStart([&] {
				manager.startTopic(pinnedUserPeer, MsgId(1), QString());
			});
			const auto sessionRefused
				= refuseCustomStart([&] { manager.start(&pinned->session()); });
			if (!peerRefused || !topicRefused || !sessionRefused) {
				std::fprintf(
					stderr,
					"custom export start bypassed the stock panel refusal\n");
				return false;
			}
			const auto stockSessionController
				= stockWindow->sessionController();
			if (!stockSessionController) {
				// clang-format off
				std::fprintf(
					stderr,
					"Stock window controller missing during export check.\n");
				// clang-format on
				return false;
			}
			const auto stockRefusalCalls
				= stockSessionController
					  ->featureUnavailableOnServerToastCallsForRegressionTest();
			const auto activeBeforeNoWindowRefusal = &domain.active();
			app.closeWindow(pinnedWindow);
			const auto pinnedWindowMappedAfterClose
				= app.separateWindowFor(pinned) != nullptr;
			const auto pinnedSessionHasWindowsAfterClose
				= !pinned->session().windows().empty();
			const auto activeUnchangedAfterClose
				= &domain.active() == activeBeforeNoWindowRefusal;
			if (pinnedWindowMappedAfterClose
				|| pinnedSessionHasWindowsAfterClose
				|| !activeUnchangedAfterClose) {
				// clang-format off
				std::fprintf(
					stderr,
					"Pinned window close mismatch: mapped=%d has_windows=%d "
					"active_unchanged=%d\n",
					pinnedWindowMappedAfterClose,
					pinnedSessionHasWindowsAfterClose,
					activeUnchangedAfterClose);
				// clang-format on
				return false;
			}
			manager.start(&pinned->session(), MTP_inputPeerEmpty(),
						  stockSessionController);
			QCoreApplication::processEvents();
			const auto noWindowRefused
				= manager.inProgress(&stock->session())
				  && !manager.inProgress(&pinned->session())
				  && &domain.active() == activeBeforeNoWindowRefusal
				  && stockExportPanel
				  && !stockExportPanel->panelVisibleForRegressionTest()
				  && stockSessionController
							 ->featureUnavailableOnServerToastCallsForRegressionTest()
						 == stockRefusalCalls + 1
				  && Export::ExportStartsForRegressionTest() == exportStarts;
			pinnedWindow = app.ensureSeparateWindowFor(pinned);
			if (!noWindowRefused) {
				std::fprintf(
					stderr,
					"export refusal mishandled a session without a window\n");
				return false;
			}
		} else {
			Export::Manager manager;
			manager.start(pinnedUserPeer);
			const auto customExportInProgress = manager.inProgress();
			const auto activeUnchanged = &domain.active() == first.get();
			if (customExportInProgress || !activeUnchanged) {
				// clang-format off
				std::fprintf(
					stderr,
					"Custom export start mismatch: in_progress=%d "
					"active_unchanged=%d\n",
					customExportInProgress,
					activeUnchanged);
				// clang-format on
				return false;
			}
		}
		domain.activate(second);
		ReportCallRegressionCheckpoint("second account activated");
		const auto secondWindow = app.separateWindowFor(second);
		const auto secondActive = &domain.active() == second.get();
		const auto secondWindowReady = secondWindow && secondActive;
		const auto secondStockMenuController = stockWindow->sessionController();
		const auto secondPinnedMenuController
			= pinnedWindow->sessionController();
		const auto secondCapabilities
			= capabilitiesMatch(second->session(), customFirst);
		const auto secondWindows
			= secondWindowReady
			  && windowsMatch(customFirst ? "stock account second activation"
										  : "custom account second activation");
		const auto secondCallStart = secondWindowReady
									 && RunOutgoingStartSelectionRegression(
										 second, secondWindow, customFirst);
		const auto secondCustomWebViewRefused
			= secondWindowReady && customWebViewOpenIsRefused();
		const auto secondBoostMenusMatch
			= secondWindowReady && secondStockMenuController
			  && secondPinnedMenuController
			  && RunBoostMenuVisibilityRegression(
				  secondStockMenuController, true,
				  customFirst ? "stock while stock is active"
							  : "stock while custom is active")
			  && RunBoostMenuVisibilityRegression(
				  secondPinnedMenuController, false,
				  customFirst ? "custom while stock is active"
							  : "custom while custom is active")
			  && RunStoryControlsVisibilityRegression(
				  secondStockMenuController, true,
				  customFirst ? "stock while stock is active"
							  : "stock while custom is active")
			  && RunStoryControlsVisibilityRegression(
				  secondPinnedMenuController, false,
				  customFirst ? "custom while stock is active"
							  : "custom while custom is active");
		if (!secondActive || !secondCapabilities || !secondWindows
			|| !secondCallStart || !secondCustomWebViewRefused
			|| !secondBoostMenusMatch) {
			std::fprintf(stderr,
						 "Second activation mismatch: customFirst=%d active=%d "
						 "window=%d capabilities=%d windows=%d "
						 "call-start=%d "
						 "webview-refused=%d boost-menus=%d\n",
						 customFirst, secondActive, secondWindowReady,
						 secondCapabilities, secondWindows, secondCallStart,
						 secondCustomWebViewRefused, secondBoostMenusMatch);
			if (!secondCapabilities) {
				printCapabilities(customFirst ? "stock second"
											  : "pinned second",
								  second->session());
			}
			return false;
		}
		const auto stockUiController = stockWindow->sessionController();
		const auto customUiController = pinnedWindow->sessionController();
		if (!stockUiController || !customUiController
			|| !RunChatFeatureMenusLifecycleRegression(
				stockUiController, customUiController,
				customFirst ? u"custom-first"_q : u"stock-first"_q)) {
			return false;
		}
		return true;
	};
	if (!stockWindow || !pinnedWindow) {
		std::fprintf(
			stderr, "Separate window construction failed: stock=%p pinned=%p\n",
			static_cast<const void *>(stockWindow),
			static_cast<const void *>(pinnedWindow));
		return failPrerequisite(
			"session feature capabilities crossed account or window "
			"boundaries");
	}
	pinned->session().data().processUsers(MTP_vector<MTPUser>({
		RegressionUser(UserId(2), false, u"2"_q),
	}));
	const auto stockUser = stock->session().data().userLoaded(UserId(2));
	if (!stockUser) {
		return failPrerequisite(
			"stock call-start regression user was not loaded");
	}
	auto &calls = app.calls();
	const auto stockCall
		= CallsInstanceRegressionAccess::InstallStockCall(&calls, stockUser);
	if (!stockCall) {
		return failPrerequisite(
			"could not install a live stock call for refusal regressions");
	}
	const auto removeStockCall = gsl::finally([&] {
		CallsInstanceRegressionAccess::RemoveStockCall(&calls, stockCall);
	});
	const auto stockDcOptions = stock->mtp().dcOptions().serialize();
	const auto pinnedDcOptions = pinned->mtp().dcOptions().serialize();
	auto pinnedController = pinnedWindow->sessionController();
	auto stockController = stockWindow->sessionController();
	if (!pinnedController || !stockController
		|| (&pinnedController->session() != &pinned->session())
		|| (&stockController->session() != &stock->session())) {
		return failPrerequisite(
			"test windows lost their owning sessions before folder smoke "
			"tests");
	}
	ReportCallRegressionCheckpoint("call start fixtures ready");
	nativeLifecycleResults->Report("chat-participants-prerequisites", true);
	nativeLifecycleResults->StartCase("call-session-fixtures");
	nativeLifecycleResults->Report("call-session-fixtures", true);
	nativeLifecycleResults->StartCase("calls-instance-start");
	const auto stockCallStartPassed
		= RunCallsInstanceStartRegression(stock, stockWindow, stockCall, true);
	const auto pinnedCallStartPassed = RunCallsInstanceStartRegression(
		pinned, pinnedWindow, stockCall, false);
	const auto callsInstanceStartPassed
		= stockCallStartPassed && pinnedCallStartPassed
		  && !shouldInjectFailure("calls-instance-start");
	nativeLifecycleResults->Report("calls-instance-start",
								   callsInstanceStartPassed);
	nativeLifecycleResults->StartCase("stock-incoming-call-answer");
	const auto stockIncomingCallAnswerPassed
		= RunIncomingCallAnswerRegression(stock, true)
		  && !shouldInjectFailure("stock-incoming-call-answer");
	nativeLifecycleResults->Report("stock-incoming-call-answer",
								   stockIncomingCallAnswerPassed);
	nativeLifecycleResults->StartCase("pinned-incoming-call-answer");
	const auto pinnedIncomingCallAnswerPassed
		= RunIncomingCallAnswerRegression(pinned, false)
		  && !shouldInjectFailure("pinned-incoming-call-answer");
	nativeLifecycleResults->Report("pinned-incoming-call-answer",
								   pinnedIncomingCallAnswerPassed);
	ReportCallRegressionCheckpoint("stock and pinned call starts complete");
	nativeLifecycleResults->StartCase("stock-blocked-call-policy");
	stock->mtp().dcOptions().constructBlocked();
	const auto stockBlockedCallStartPassed
		= !stock->local().hasStoredCustomServer()
		  && stock->mtp().dcOptions().blocked()
		  && RunCallsInstanceStartRegression(stock, stockWindow, stockCall,
											 false);
	const auto stockBlockedChannelLinkPassed
		= RunChannelCallLinkRegression(stock, stockWindow);
	ReportCallRegressionCheckpoint("blocked stock starts complete");
	stock->mtp().dcOptions().constructUnenrolled();
	const auto stockOptionsRestored
		= stock->mtp().dcOptions().constructFromSerialized(stockDcOptions);
	nativeLifecycleResults->Report(
		"stock-blocked-call-policy",
		stockBlockedCallStartPassed && stockBlockedChannelLinkPassed
			&& stockOptionsRestored
			&& !shouldInjectFailure("stock-blocked-call-policy"));
	if (!stockOptionsRestored) {
		nativeLifecycleResults->SkipUnreported(
			"dependency-stock-blocked-call-policy");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return FailChatParticipantsRegression(
			"could not restore stock DC options after blocked-call regression");
	}
	nativeLifecycleResults->StartCase("pinned-blocked-call-policy");
	pinned->mtp().dcOptions().constructBlocked();
	const auto pinnedBlockedCallStartPassed
		= pinned->local().hasStoredCustomServer()
		  && pinned->mtp().dcOptions().blocked()
		  && !pinned->mtp().dcOptions().hasCustomServer()
		  && RunCallsInstanceStartRegression(pinned, pinnedWindow, stockCall,
											 false);
	const auto pinnedBlockedChannelLinkPassed
		= RunChannelCallLinkRegression(pinned, pinnedWindow);
	ReportCallRegressionCheckpoint("blocked pinned starts complete");
	pinned->mtp().dcOptions().constructUnenrolled();
	const auto pinnedOptionsRestored
		= pinned->mtp().dcOptions().constructFromSerialized(pinnedDcOptions);
	nativeLifecycleResults->Report(
		"pinned-blocked-call-policy",
		pinnedBlockedCallStartPassed && pinnedBlockedChannelLinkPassed
			&& pinnedOptionsRestored
			&& !shouldInjectFailure("pinned-blocked-call-policy"));
	if (!pinnedOptionsRestored) {
		nativeLifecycleResults->SkipUnreported(
			"dependency-pinned-blocked-call-policy");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return FailChatParticipantsRegression(
			"could not restore pinned DC options after blocked-call "
			"regression");
	}
	nativeLifecycleResults->StartCase("conference-link-call-preservation");
	const auto conferenceLinkCallPreserved
		= RunConferenceLinkPreservesStockCallRegression(pinned, pinnedWindow,
														stockCall)
		  && !shouldInjectFailure("conference-link-call-preservation");
	nativeLifecycleResults->Report("conference-link-call-preservation",
								   conferenceLinkCallPreserved);
	ReportCallRegressionCheckpoint("conference preservation complete");
	CallsInstanceRegressionAccess::RemoveStockCall(&calls, stockCall);
	nativeLifecycleResults->StartCase("call-links-and-settings");
	const auto callLinksAndSettingsPassed
		= RunCallLinkAndSettingsRegression(pinned, pinnedWindow);
	const auto settingsLayerClosed = !pinnedWindow->isLayerShown();
	nativeLifecycleResults->Report(
		"call-links-and-settings",
		callLinksAndSettingsPassed && settingsLayerClosed
			&& !shouldInjectFailure("call-links-and-settings"));
	ReportCallRegressionCheckpoint("call links and settings complete");
	nativeLifecycleResults->StartCase("settings-search");
	const auto settingsSearchMatches
		= SettingsSearchMatchesExpected(&stock->session(), true, true, true)
		  && SettingsSearchMatchesExpected(&pinned->session(), false, false,
										   false)
		  && SettingsSearchMatchesExpected(&stock->session(), true, true, true)
		  && !shouldInjectFailure("settings-search");
	nativeLifecycleResults->Report("settings-search", settingsSearchMatches);
	nativeLifecycleResults->StartCase("session-window-lifecycle");
	const auto initialWindowsMatch = windowsMatch("initial separate windows");
	const auto stockFirstActivationPassed = activateAndCheck(false);
	const auto pinnedFirstActivationPassed = activateAndCheck(true);
	const auto windowLifecyclePassed = initialWindowsMatch
									   && stockFirstActivationPassed
									   && pinnedFirstActivationPassed;
	nativeLifecycleResults->Report("session-window-lifecycle",
								   windowLifecyclePassed);
	if (!windowLifecyclePassed) {
		std::fprintf(stderr,
					 "Chat participants regression failed: session feature "
					 "capabilities crossed account or window boundaries\n");
	}
	pinnedController = pinnedWindow->sessionController();
	stockController = stockWindow->sessionController();
	nativeLifecycleResults->StartCase("admin-window-fixtures");
	const auto adminWindowFixturesPassed
		= pinnedController && stockController
		  && (&pinnedController->session() == &pinned->session())
		  && (&stockController->session() == &stock->session());
	nativeLifecycleResults->Report("admin-window-fixtures",
								   adminWindowFixturesPassed);
	if (!adminWindowFixturesPassed) {
		nativeLifecycleResults->SkipUnreported(
			"dependency-admin-window-fixtures");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return FailChatParticipantsRegression(
			"test windows lost their owning sessions after activation checks");
	}
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	nativeLifecycleResults->StartCase("translation-controls");
	const auto translationControlsPassed
		= !shouldInjectFailure("translation-controls")
		  && RunTranslationControlsLifecycleRegression(stockController,
													   pinnedController);
	nativeLifecycleResults->Report("translation-controls",
								   translationControlsPassed);
	stockController->hideLayer(anim::type::instant);
	pinnedController->hideLayer(anim::type::instant);
	nativeLifecycleResults->StartCase("pinned-peer-photo-removal-lifecycle");
	const auto pinnedPeerPhotoRemovalLifecyclePassed
		= !shouldInjectFailure("pinned-peer-photo-removal-lifecycle")
		  && RunPeerPhotoRemovalLifecycleRegression(pinnedController);
	nativeLifecycleResults->Report("pinned-peer-photo-removal-lifecycle",
								   pinnedPeerPhotoRemovalLifecyclePassed);
	nativeLifecycleResults->StartCase("stock-peer-photo-upload-lifecycle");
	const auto stockPeerPhotoUploadLifecyclePassed
		= !shouldInjectFailure("stock-peer-photo-upload-lifecycle")
		  && RunPeerPhotoUploadLifecycleRegression(stockController, false);
	nativeLifecycleResults->Report("stock-peer-photo-upload-lifecycle",
								   stockPeerPhotoUploadLifecyclePassed);
	nativeLifecycleResults->StartCase("pinned-peer-photo-upload-lifecycle");
	const auto pinnedPeerPhotoUploadLifecyclePassed
		= !shouldInjectFailure("pinned-peer-photo-upload-lifecycle")
		  && RunPeerPhotoUploadLifecycleRegression(pinnedController, true);
	nativeLifecycleResults->Report("pinned-peer-photo-upload-lifecycle",
								   pinnedPeerPhotoUploadLifecyclePassed);
	pinnedController->hideLayer(anim::type::instant);
	stockController->hideLayer(anim::type::instant);
	nativeLifecycleResults->StartCase("pinned-created-group-photo-lifecycle");
	const auto pinnedCreatedGroupPhotoPassed
		= !shouldInjectFailure("pinned-created-group-photo-lifecycle")
		  && RunCreatedGroupPhotoUploadRegression(pinnedController);
	nativeLifecycleResults->Report("pinned-created-group-photo-lifecycle",
								   pinnedCreatedGroupPhotoPassed);
	pinnedController->hideLayer(anim::type::instant);
	nativeLifecycleResults->StartCase("manage-controls");
	const auto manageControlsPassed = RunManageControlsLifecycleRegression(
		stockController, pinnedController);
	nativeLifecycleResults->Report("manage-controls", manageControlsPassed);
	stockController->hideLayer(anim::type::instant);
	pinnedController->hideLayer(anim::type::instant);
	nativeLifecycleResults->StartCase("boost-entry-paths");
	const auto activeWindowBeforeBoost = app.activeWindow();
	const auto boostBlocked = domain.add(MTP::Environment::Production);
	boostBlocked->mtp().stopForServerEnrollment();
	const auto boostWindow = app.ensureSeparateWindowFor(boostBlocked);
	boostBlocked->setSessionUserId(selfId);
	auto boostEntryPassed = false;
	if (boostWindow && app.separateWindowFor(boostBlocked) == boostWindow
		&& boostBlocked->createSession(
			RegressionUser(selfId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		boostBlocked->mtp().dcOptions().constructBlocked();
		const auto boostController
			= boostWindow->sessionController()
				  ? boostWindow->sessionController()
				  : boostBlocked->session().tryResolveWindow();
		if (boostController && boostBlocked->mtp().dcOptions().blocked()
			&& !boostBlocked->mtp().dcOptions().hasCustomServer()) {
			boostEntryPassed = RunBoostEntryPathRefusalRegression(
				app, stock, pinned, boostBlocked, stockController,
				pinnedController, boostController);
		} else {
			std::fprintf(stderr, "Boost entry regression: blocked fixture "
								 "could not be prepared.\n");
		}
		boostBlocked->forcedLogOut();
	} else {
		std::fprintf(
			stderr,
			"Boost entry regression: could not create the blocked fixture.\n");
		if (boostWindow && app.separateWindowFor(boostBlocked) == boostWindow) {
			app.closeWindow(boostWindow);
		}
	}
	if (activeWindowBeforeBoost) {
		activeWindowBeforeBoost->activate();
	}
	nativeLifecycleResults->Report(
		"boost-entry-paths",
		boostEntryPassed && !shouldInjectFailure("boost-entry-paths"));
#endif
	const auto activeWindowBeforeBlockedFixture = app.activeWindow();
	const auto restoredBlocked = domain.add(MTP::Environment::Production);
	restoredBlocked->mtp().stopForServerEnrollment();
	const auto restoredBlockedIsDistinct
		= (restoredBlocked.get() != stock.get())
		  && (restoredBlocked.get() != pinned.get())
		  && (restoredBlocked.get() != blank.get());
	const auto restoredBlockedHasNoSession = !restoredBlocked->sessionExists();
	const auto restoredBlockedHasNoStoredCustom
		= !restoredBlocked->local().hasStoredCustomServer();
	const auto restoredBlockedPinStateKnown
		= !restoredBlocked->local().customServerPinUnknown();
	const auto restoredBlockedInitialStateReady
		= restoredBlockedIsDistinct && restoredBlockedHasNoSession
		  && restoredBlockedHasNoStoredCustom && restoredBlockedPinStateKnown;
	auto restoredBlockedAuthorizationFailureWritten = false;
	if (restoredBlockedInitialStateReady) {
		restoredBlocked->mtp().dcOptions().constructBlocked();
		restoredBlockedAuthorizationFailureWritten
			= restoredBlocked->local().writeMtpAuthorizationFailure();
	}
	const auto restoredBlockedIsBlocked
		= restoredBlocked->mtp().config().blocked();
	const auto restoredBlockedHasNoCustomConfig
		= !restoredBlocked->mtp().dcOptions().hasCustomServer();
	const auto restoredBlockedAuthorizationFailed
		= restoredBlocked->local().mtpAuthorizationWriteFailed();
	const auto restoredBlockedNetworkAllowed
		= restoredBlocked->mtp().isServerEnrollmentNetworkAllowed();
	auto restoredBlockedFixtureReady
		= restoredBlockedInitialStateReady
		  && restoredBlockedAuthorizationFailureWritten
		  && restoredBlockedIsBlocked && restoredBlockedHasNoCustomConfig
		  && restoredBlockedHasNoStoredCustom && restoredBlockedPinStateKnown
		  && restoredBlockedAuthorizationFailed
		  && !restoredBlockedNetworkAllowed;
	if (!restoredBlockedFixtureReady) {
		std::fprintf(stderr,
					 "Settings deep link restored-blocked fixture checks: "
					 "distinct=%d marker_written=%d no_session=%d blocked=%d "
					 "custom=%d stored_custom=%d pin_unknown=%d "
					 "auth_failed=%d network_allowed=%d.\n",
					 restoredBlockedIsDistinct,
					 restoredBlockedAuthorizationFailureWritten,
					 restoredBlockedHasNoSession, restoredBlockedIsBlocked,
					 !restoredBlockedHasNoCustomConfig,
					 !restoredBlockedHasNoStoredCustom,
					 !restoredBlockedPinStateKnown,
					 restoredBlockedAuthorizationFailed,
					 restoredBlocked->mtp().isServerEnrollmentNetworkAllowed());
	}
	restoredBlockedWindow = app.ensureSeparateWindowFor(restoredBlocked);
	const auto restoredBlockedLifetime = base::make_weak(restoredBlocked.get());
	const auto restoredBlockedWindowReady
		= restoredBlockedWindow
		  && app.separateWindowFor(restoredBlocked) == restoredBlockedWindow
		  && (&restoredBlockedWindow->account() == restoredBlocked.get());
	restoredBlockedFixtureReady
		= restoredBlockedFixtureReady && restoredBlockedWindowReady;
	if (!restoredBlockedFixtureReady) {
		std::fprintf(stderr,
					 "Mini-app open regression: restored blocked fixture could "
					 "not be prepared: window=%d.\n",
					 restoredBlockedWindowReady);
	}
	auto blankController
		= PrepareBlockedWithoutPinMiniAppFixture(blank, selfId, blankWindow);
	const auto blankFixtureReady = (blankController != nullptr);
	const auto blankLifetime = base::make_weak(blank.get());
	const auto cleanupBlank = gsl::finally([blankLifetime] {
		if (const auto strong = blankLifetime.get()) {
			if (strong->sessionExists()) {
				strong->forcedLogOut();
			}
		}
	});
	if (activeWindowBeforeBlockedFixture) {
		activeWindowBeforeBlockedFixture->activate();
	}
	nativeLifecycleResults->StartCase("passport-link-refusal");
	const auto passportRegressionPassed
		= blankFixtureReady
		  && RunPassportLinkRefusalRegression(app, domain, stock, pinned, blank,
											  stockWindow, pinnedWindow,
											  blankWindow);
	nativeLifecycleResults->Report("passport-link-refusal",
								   passportRegressionPassed);
	nativeLifecycleResults->StartCase("mini-app-open");
	const auto miniAppRegressionPassed
		= restoredBlockedFixtureReady && blankFixtureReady
		  && RunMiniAppOpenRefusalRegression(
			  app, selfId, stock, pinned, blank,
			  not_null<Window::SessionController *>(blankController),
			  stockWindow, pinnedWindow);
	if (!miniAppRegressionPassed) {
		std::fprintf(stderr, "Chat participants regression failed: %s\n",
					 "mini-app open refusal was not observable on the owning "
					 "session");
	}
	const auto restoredBlockedLifetimeAlive
		= restoredBlockedLifetime.get() == restoredBlocked.get();
	const auto restoredBlockedWindowMapped
		= restoredBlockedLifetimeAlive
		  && app.separateWindowFor(restoredBlocked) == restoredBlockedWindow;
	if (!restoredBlockedLifetimeAlive || !restoredBlockedWindowMapped) {
		std::fprintf(
			stderr,
			"Mini-app open regression: restored blocked fixture lifetime "
			"failed: alive=%d window=%d.\n",
			restoredBlockedLifetimeAlive, restoredBlockedWindowMapped);
	}
	const auto miniAppPassed = miniAppRegressionPassed
							   && restoredBlockedLifetimeAlive
							   && restoredBlockedWindowMapped;
	nativeLifecycleResults->Report("mini-app-open", miniAppPassed);
	if (blankFixtureReady) {
		nativeLifecycleResults->StartCase("messages-privacy");
		const auto stockMessagesPrivacyPassed = RunMessagesPrivacyBoxRegression(
			stockController, selfId, true, "stock");
		const auto customMessagesPrivacyPassed
			= RunMessagesPrivacyBoxRegression(pinnedController, selfId, false,
											  "custom");
		const auto blockedMessagesPrivacyPassed
			= RunMessagesPrivacyBoxRegression(blankController, selfId, false,
											  "blocked");
		const auto restoredStockMessagesPrivacyPassed
			= RunMessagesPrivacyBoxRegression(stockController, selfId, true,
											  "stock after custom");
		nativeLifecycleResults->Report(
			"messages-privacy", stockMessagesPrivacyPassed
									&& customMessagesPrivacyPassed
									&& blockedMessagesPrivacyPassed
									&& restoredStockMessagesPrivacyPassed);

		nativeLifecycleResults->StartCase("paid-link");
		const auto paidLinkPassed = RunPaidLinkRefusalRegression(
			app, stockController, pinnedController, blankController);
		nativeLifecycleResults->Report("paid-link", paidLinkPassed);

		{
			nativeLifecycleResults->StartCase("boost-story-menus");
			auto &accountDomain = app.domain();
			const auto activeAccount = &accountDomain.active();
			const auto restoreActiveAccount
				= gsl::finally([&] { accountDomain.activate(activeAccount); });
			accountDomain.activate(stock);
			const auto stockBoostWhileBlockedAvailable
				= RunBoostMenuVisibilityRegression(
					stockController, true,
					"stock with blocked account available");
			const auto blockedBoostWhileStockActive
				= RunBoostMenuVisibilityRegression(
					blankController, false, "blocked while stock is active");
			const auto stockStoriesWhileBlockedAvailable
				= RunStoryControlsVisibilityRegression(
					stockController, true,
					"stock with blocked account available");
			const auto blockedStoriesWhileStockActive
				= RunStoryControlsVisibilityRegression(
					blankController, false, "blocked while stock is active");
			accountDomain.activate(blank);
			const auto blockedBoostWithStockAvailable
				= RunBoostMenuVisibilityRegression(
					blankController, false,
					"blocked with stock account available");
			const auto stockBoostWhileBlockedActive
				= RunBoostMenuVisibilityRegression(
					stockController, true, "stock while blocked is active");
			const auto blockedStoriesWithStockAvailable
				= RunStoryControlsVisibilityRegression(
					blankController, false,
					"blocked with stock account available");
			const auto stockStoriesWhileBlockedActive
				= RunStoryControlsVisibilityRegression(
					stockController, true, "stock while blocked is active");
			const auto boostStoryMenusPassed
				= stockBoostWhileBlockedAvailable
				  && blockedBoostWhileStockActive
				  && stockStoriesWhileBlockedAvailable
				  && blockedStoriesWhileStockActive
				  && blockedBoostWithStockAvailable
				  && stockBoostWhileBlockedActive
				  && blockedStoriesWithStockAvailable
				  && stockStoriesWhileBlockedActive;
			nativeLifecycleResults->Report("boost-story-menus",
										   boostStoryMenusPassed);
		}

		if (restoredBlockedFixtureReady) {
			nativeLifecycleResults->StartCase("settings-deep-link");
			const auto settingsDeepLinkPassed
				= RunSettingsDeepLinkRefusalRegression(
					app, stock, pinned, blank, restoredBlocked, stockController,
					pinnedController, blankController);
			const auto expiredSettingsTargetPassed
				= RunExpiredSettingsDeepLinkTargetRegression(
					app, blank, blankWindow, stockController);
			nativeLifecycleResults->Report("settings-deep-link",
										   settingsDeepLinkPassed
											   && expiredSettingsTargetPassed);
		} else {
			nativeLifecycleResults->Skip(
				"settings-deep-link", "restored-blocked-fixture-unavailable");
		}
	} else {
		for (const auto name : {
				 "settings-deep-link",
				 "messages-privacy",
				 "paid-link",
				 "boost-story-menus",
			 }) {
			nativeLifecycleResults->Skip(name,
										 "blocked-session-fixture-unavailable");
		}
	}
	if (restoredBlockedWindow
		&& app.separateWindowFor(restoredBlockedWindow->id())
			   == restoredBlockedWindow) {
		app.closeWindow(restoredBlockedWindow);
		restoredBlockedWindow = nullptr;
	}
	nativeLifecycleResults->StartCase("chatlist-invite-check");
	Api::CheckFilterInvite(pinnedController, u"regression-slug"_q);
	const auto inviteCheckRefused
		= !pinnedController->session()
			   .api()
			   .checkFilterInviteRequestPendingForRegressionTest();
	nativeLifecycleResults->Report("chatlist-invite-check", inviteCheckRefused);
	ReportCallRegressionCheckpoint("account activation regressions complete");
	RunAuthenticatedCacheSymlinkCases(domain, fixtures, pinned, pinnedCachePath,
									  pinnedMediaCachePath,
									  nativeLifecycleResults);
	nativeLifecycleResults->StartCase("pinned-chat-participants");
	const auto pinnedChat = pinned->session().data().chat(chatId);
	const auto pinnedPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*pinnedChat));
	pinned->session().api().processFullPeer(
		pinnedPeer, RegressionChatFullReply(chatId, 1, QString(), UserId(2)));
	const auto pinnedParticipantsPassed
		= pinned->session().user()->isLoaded()
		  && HasExpectedParticipants(pinnedChat, UserId(2));
	nativeLifecycleResults->Report("pinned-chat-participants",
								   pinnedParticipantsPassed);
	nativeLifecycleResults->StartCase("folder-crud");
	const auto stockFolderCrudPassed
		= Settings::RunFoldersCrudRegressionForTest(
			stockController, pinnedController,
			stock->session().data().history(stockPeer),
			Settings::FolderCrudRegressionSide::Stock);
	const auto pinnedFolderCrudPassed
		= Settings::RunFoldersCrudRegressionForTest(
			pinnedController, stockController,
			pinned->session().data().history(pinnedPeer),
			Settings::FolderCrudRegressionSide::Pinned);
	const auto folderCrudPassed = stockFolderCrudPassed
								  && pinnedFolderCrudPassed
								  && !shouldInjectFailure("folder-crud");
	nativeLifecycleResults->Report("folder-crud", folderCrudPassed);
	if (!folderCrudPassed) {
		std::fprintf(
			stderr,
			"Chat participants regression failed: ordinary folder "
			"create, rename, save, or remove did not stay in its owning "
			"session\n");
	}
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	nativeLifecycleResults->StartCase("bio-editor-target");
	const auto stockBioEditorTarget
		= Settings::InformationBioEditorTargetPresentForRegressionTest(
			stockController);
	const auto pinnedBioEditorTarget
		= Settings::InformationBioEditorTargetPresentForRegressionTest(
			pinnedController);
	const auto bioEditorTargetPassed
		= stockBioEditorTarget && !pinnedBioEditorTarget;
	recordChatCase("bio-editor-target", bioEditorTargetPassed,
				   "bio editor target did not follow account support");
#else
	nativeLifecycleResults->Skip("bio-editor-target",
								 "lifecycle-regression-disabled");
#endif
	nativeLifecycleResults->StartCase("admin-session-identities");
	const auto stockSelf = stock->session().user();
	const auto customSelf = pinned->session().user();
	const auto adminIdentityFailureInjected
		= shouldInjectFailure("admin-session-identities");
	const auto adminSessionIdentitiesMatch
		= stockSelf != customSelf && stockSelf->id == customSelf->id
		  && &stockSelf->session() == &stock->session()
		  && &customSelf->session() == &pinned->session()
		  && !adminIdentityFailureInjected;
	recordChatCase(
		"admin-session-identities", adminSessionIdentitiesMatch,
		adminIdentityFailureInjected
			? "injected admin-session identity failure"
			: "basic-group admin fixture identities are not distinct "
			  "session-owned objects with the same user id");
	if (!adminSessionIdentitiesMatch && !adminIdentityFailureInjected) {
		std::fprintf(stderr,
					 "basic-group admin fixture identities are not distinct "
					 "session-owned objects with the same user id\n");
	}
	const auto actionChatId = ChatId(1052);
	nativeLifecycleResults->StartCase("pinned-migration-fixture");
	const auto pinnedActionChatData
		= pinned->session().data().chat(actionChatId);
	ChatData *const pinnedActionChat = &*pinnedActionChatData;
	const auto pinnedActionPeer
		= not_null<PeerData *>(static_cast<PeerData *>(pinnedActionChat));
	pinned->session().api().processFullPeer(
		pinnedActionPeer,
		RegressionChatFullReply(actionChatId, 1, QString(), selfId));
	pinnedActionChat->setFlags(ChatDataFlag::Creator);
	const auto pinnedMigrationFixturePassed
		= pinnedActionChat->usesCustomServer()
		  && !pinnedActionChat->isDeactivated()
		  && !pinnedActionChat->migrateTo()
		  && !shouldInjectFailure("pinned-migration-fixture");
	recordChatCase(
		"pinned-migration-fixture", pinnedMigrationFixturePassed,
		"pinned migration fixture is not an active custom-server group");
	nativeLifecycleResults->StartCase("stock-basic-group-creator");
	const auto stockActionChat = stock->session().data().chat(actionChatId);
	const auto stockActionPeer
		= not_null<PeerData *>(static_cast<PeerData *>(&*stockActionChat));
	stock->session().api().processFullPeer(
		stockActionPeer, RegressionStockChatFullReply(
							 actionChatId, 1, u"+10000000001"_q, selfId));
	stockActionChat->setFlags(ChatDataFlag::Creator);
	const auto stockCreatorDefaultRightsAreUnchanged
		= !stockActionChat->usesCustomServer()
		  && stockActionChat->defaultAdminRights(stockSelf).flags
				 == ChatAdminRightsInfo(ChatAdminRight::Other
										| ChatAdminRight::ChangeInfo
										| ChatAdminRight::DeleteMessages
										| ChatAdminRight::BanUsers
										| ChatAdminRight::InviteByLinkOrAdd
										| ChatAdminRight::PinMessages
										| ChatAdminRight::ManageCall
										| ChatAdminRight::AddAdmins)
						.flags;
	const auto stockCreatorPassed
		= HasExpectedParticipants(stockActionChat, selfId)
		  && stockCreatorDefaultRightsAreUnchanged
		  && stockActionChat->canEditInformation()
		  && stockActionChat->canAddMembers() && stockActionChat->canAddAdmins()
		  && stockActionChat->canBanMembers();
	recordChatCase(
		"stock-basic-group-creator", stockCreatorPassed,
		"stock creator defaults or supported basic-group actions changed");
	if (pinnedMigrationFixturePassed) {
		nativeLifecycleResults->StartCase("pinned-basic-group-creator");
		const auto pinnedCreatorPassed
			= HasExpectedParticipants(pinnedActionChat, selfId)
			  && pinnedActionChat->canEditInformation()
			  && pinnedActionChat->canAddMembers()
			  && pinnedActionChat->canAddAdmins()
			  && pinnedActionChat->canBanMembers();
		recordChatCase(
			"pinned-basic-group-creator", pinnedCreatorPassed,
			"pinned creator lost a supported basic-group admin action");
	} else {
		nativeLifecycleResults->Skip("pinned-basic-group-creator",
									 "dependency-pinned-migration-fixture");
	}
	if (adminSessionIdentitiesMatch) {
		nativeLifecycleResults->StartCase("stock-full-list-admin");
		const auto memberActionChatId = ChatId(1053);
		const auto memberActionChat
			= stock->session().data().chat(memberActionChatId);
		const auto memberActionPeer
			= not_null<PeerData *>(static_cast<PeerData *>(&*memberActionChat));
		stock->session().api().processFullPeer(
			memberActionPeer,
			RegressionStockChatFullReply(memberActionChatId, 1,
										 u"+10000000001"_q, UserId(2)));

		const auto adminActionChatId = ChatId(1054);
		const auto adminActionChat
			= stock->session().data().chat(adminActionChatId);
		const auto adminActionPeer
			= not_null<PeerData *>(static_cast<PeerData *>(&*adminActionChat));
		stock->session().api().processFullPeer(
			adminActionPeer,
			RegressionStockChatFullReply(adminActionChatId, 1,
										 u"+10000000001"_q, UserId(2), true));
		const auto stockAdminDefaultRightsAreUnchanged
			= !adminActionChat->usesCustomServer()
			  && adminActionChat->defaultAdminRights(stockSelf).flags
					 == ChatAdminRightsInfo(ChatAdminRight::Other
											| ChatAdminRight::ChangeInfo
											| ChatAdminRight::DeleteMessages
											| ChatAdminRight::BanUsers
											| ChatAdminRight::InviteByLinkOrAdd
											| ChatAdminRight::PinMessages
											| ChatAdminRight::ManageCall)
							.flags;
		const auto fullListAdminRightsMatch = BasicGroupAdminRightsMatch(
			adminActionChat, "stock full-list promotion", true);
		const auto stockFullListAdminPassed
			= stockAdminDefaultRightsAreUnchanged
			  && HasExpectedParticipants(adminActionChat, UserId(2))
			  && adminActionChat->admins.contains(stockSelf)
			  && fullListAdminRightsMatch;
		recordChatCase(
			"stock-full-list-admin", stockFullListAdminPassed,
			"official basic-group admin defaults, moderation rights, or "
			"creator-only powers were incorrect");

		nativeLifecycleResults->StartCase("stock-full-list-demotion");
		const auto fullListDemotionChatId = ChatId(1057);
		const auto fullListDemotionChat
			= stock->session().data().chat(fullListDemotionChatId);
		const auto fullListDemotionPeer = not_null<PeerData *>(
			static_cast<PeerData *>(&*fullListDemotionChat));
		stock->session().api().processFullPeer(
			fullListDemotionPeer,
			RegressionStockChatFullReply(fullListDemotionChatId, 1,
										 u"+10000000001"_q, UserId(2), true));
		const auto fullListDemotionSeedRightsMatch = BasicGroupAdminRightsMatch(
			fullListDemotionChat, "stock full-list demotion seed", true);
		const auto fullListDemotionSeedPassed
			= fullListDemotionChat->admins.contains(stockSelf)
			  && fullListDemotionSeedRightsMatch;
		Data::ApplyChatUpdate(
			fullListDemotionChat,
			MTP_chatParticipants(
				MTP_long(fullListDemotionChatId.bare),
				MTP_vector<MTPChatParticipant>({
					MTP_chatParticipantCreator(
						MTP_flags(MTPDchatParticipantCreator::Flags()),
						MTP_long(UserId(2).bare), MTP_string(QString())),
					MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()),
										MTP_long(selfId.bare),
										MTP_long(UserId(2).bare), MTP_int(0),
										MTP_string(QString())),
					MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()),
										MTP_long(UserId(9).bare),
										MTP_long(selfId.bare), MTP_int(0),
										MTP_string(QString())),
				}),
				MTP_int(2)));
		const auto fullListDemotionRightsMatch = BasicGroupAdminRightsMatch(
			fullListDemotionChat, "stock full-list demotion", false);
		const auto fullListDemotionPassed
			= fullListDemotionSeedPassed
			  && HasExpectedParticipants(fullListDemotionChat, UserId(2))
			  && !fullListDemotionChat->admins.contains(stockSelf)
			  && fullListDemotionChat->adminRights() == ChatAdminRights()
			  && fullListDemotionRightsMatch;
		recordChatCase(
			"stock-full-list-demotion", fullListDemotionPassed,
			"normal-server full-list demotion retained admin rights or "
			"moderation controls");

		nativeLifecycleResults->StartCase("custom-full-list-admin");
		const auto customAdminActionChat
			= pinned->session().data().chat(adminActionChatId);
		const auto customAdminActionPeer = not_null<PeerData *>(
			static_cast<PeerData *>(&*customAdminActionChat));
		const auto customDisplayRights
			= customAdminActionChat->defaultAdminRights(customSelf).flags;
		const auto customDefaultRightsAreRoleOnly
			= customAdminActionChat->usesCustomServer()
			  && customDisplayRights
					 == ChatAdminRightsInfo(ChatAdminRight::Other).flags;
		customAdminActionChat->setAdminRights(
			OverbroadBasicGroupAdminRightsForRegression());
		const auto customFullListSeedRightsMatch = BasicGroupAdminRightsMatch(
			customAdminActionChat, "custom full-list promotion seed", true);
		const auto customFullListSeedPassed
			= customAdminActionChat->adminRights() != ChatAdminRights()
			  && customFullListSeedRightsMatch;
		pinned->session().api().processFullPeer(
			customAdminActionPeer,
			RegressionChatFullReply(adminActionChatId, 1, u"+10000000001"_q,
									UserId(2), true));
		const auto customAdminRightsMatch = BasicGroupAdminRightsMatch(
			customAdminActionChat, "custom full-list promotion", false);
		const auto customFullListAdminPassed
			= customDefaultRightsAreRoleOnly && customFullListSeedPassed
			  && HasExpectedParticipants(customAdminActionChat, UserId(2))
			  && customAdminActionChat->admins.contains(customSelf)
			  && !customAdminActionChat->admins.contains(stockSelf)
			  && customAdminActionChat->adminRights() == ChatAdminRights()
			  && customAdminRightsMatch;
		recordChatCase(
			"custom-full-list-admin", customFullListAdminPassed,
			"custom-server basic-group admin defaults or assigned rights "
			"included privileged powers");

		nativeLifecycleResults->StartCase("stock-live-admin-promotion");
		const auto liveAdminChatId = ChatId(1055);
		const auto liveAdminChat
			= stock->session().data().chat(liveAdminChatId);
		const auto liveAdminPeer
			= not_null<PeerData *>(static_cast<PeerData *>(&*liveAdminChat));
		stock->session().api().processFullPeer(
			liveAdminPeer,
			RegressionStockChatFullReply(liveAdminChatId, 1, u"+10000000001"_q,
										 UserId(2)));
		Data::ApplyChatUpdate(
			liveAdminChat,
			MTP_updateChatParticipantAdmin(MTP_long(liveAdminChatId.bare),
										   MTP_long(selfId.bare),
										   MTP_boolTrue(), MTP_int(2))
				.c_updateChatParticipantAdmin());
		const auto livePromotionRightsMatch = BasicGroupAdminRightsMatch(
			liveAdminChat, "stock incremental promotion", true);
		const auto stockLivePromotionPassed
			= liveAdminChat->admins.contains(stockSelf)
			  && livePromotionRightsMatch
			  && !shouldInjectFailure("stock-live-admin-promotion");
		recordChatCase(
			"stock-live-admin-promotion", stockLivePromotionPassed,
			"incremental official basic-group promotion lost admin rights "
			"or granted creator-only powers");
		if (stockLivePromotionPassed) {
			nativeLifecycleResults->StartCase("stock-live-admin-demotion");
			Data::ApplyChatUpdate(
				liveAdminChat,
				MTP_updateChatParticipantAdmin(MTP_long(liveAdminChatId.bare),
											   MTP_long(selfId.bare),
											   MTP_boolFalse(), MTP_int(3))
					.c_updateChatParticipantAdmin());
			const auto liveDemotionRightsMatch = BasicGroupAdminRightsMatch(
				liveAdminChat, "stock incremental demotion", false);
			const auto stockLiveDemotionPassed
				= !liveAdminChat->admins.contains(stockSelf)
				  && liveDemotionRightsMatch;
			recordChatCase(
				"stock-live-admin-demotion", stockLiveDemotionPassed,
				"incremental basic-group demotion did not clear admin display "
				"state");
		} else {
			nativeLifecycleResults->Skip(
				"stock-live-admin-demotion",
				"dependency-stock-live-admin-promotion");
		}

		nativeLifecycleResults->StartCase("custom-live-admin-promotion");
		const auto customLiveAdminChatId = ChatId(1056);
		const auto customLiveAdminChat
			= pinned->session().data().chat(customLiveAdminChatId);
		const auto customLiveAdminPeer = not_null<PeerData *>(
			static_cast<PeerData *>(&*customLiveAdminChat));
		pinned->session().api().processFullPeer(
			customLiveAdminPeer,
			RegressionChatFullReply(customLiveAdminChatId, 1, u"+10000000001"_q,
									UserId(2)));
		customLiveAdminChat->setAdminRights(
			OverbroadBasicGroupAdminRightsForRegression());
		const auto customLiveSeedRightsMatch = BasicGroupAdminRightsMatch(
			customLiveAdminChat, "custom incremental promotion seed", true);
		const auto customLiveSeedPassed
			= customLiveAdminChat->adminRights() != ChatAdminRights()
			  && customLiveSeedRightsMatch;
		Data::ApplyChatUpdate(
			customLiveAdminChat,
			MTP_updateChatParticipantAdmin(MTP_long(customLiveAdminChatId.bare),
										   MTP_long(selfId.bare),
										   MTP_boolTrue(), MTP_int(2))
				.c_updateChatParticipantAdmin());
		const auto customPromotionRightsMatch = BasicGroupAdminRightsMatch(
			customLiveAdminChat, "custom incremental promotion", false);
		const auto customLivePromotionPassed
			= customLiveSeedPassed
			  && customLiveAdminChat->admins.contains(customSelf)
			  && !customLiveAdminChat->admins.contains(stockSelf)
			  && customLiveAdminChat->adminRights() == ChatAdminRights()
			  && customPromotionRightsMatch
			  && !shouldInjectFailure("custom-live-admin-promotion");
		recordChatCase(
			"custom-live-admin-promotion", customLivePromotionPassed,
			"incremental custom-server promotion gained admin rights or "
			"creator-only powers");
		if (customLivePromotionPassed) {
			nativeLifecycleResults->StartCase("custom-live-admin-demotion");
			Data::ApplyChatUpdate(customLiveAdminChat,
								  MTP_updateChatParticipantAdmin(
									  MTP_long(customLiveAdminChatId.bare),
									  MTP_long(selfId.bare), MTP_boolFalse(),
									  MTP_int(3))
									  .c_updateChatParticipantAdmin());
			const auto customDemotionRightsMatch = BasicGroupAdminRightsMatch(
				customLiveAdminChat, "custom incremental demotion", false);
			const auto customLiveDemotionPassed
				= !customLiveAdminChat->admins.contains(customSelf)
				  && !customLiveAdminChat->admins.contains(stockSelf)
				  && customLiveAdminChat->adminRights() == ChatAdminRights()
				  && customDemotionRightsMatch;
			recordChatCase("custom-live-admin-demotion",
						   customLiveDemotionPassed,
						   "incremental custom-server demotion did not clear "
						   "admin display "
						   "state");
		} else {
			nativeLifecycleResults->Skip(
				"custom-live-admin-demotion",
				"dependency-custom-live-admin-promotion");
		}
	} else {
		for (const auto name : {
				 "stock-full-list-admin",
				 "stock-full-list-demotion",
				 "custom-full-list-admin",
				 "stock-live-admin-promotion",
				 "stock-live-admin-demotion",
				 "custom-live-admin-promotion",
				 "custom-live-admin-demotion",
			 }) {
			nativeLifecycleResults->Skip(name,
										 "dependency-admin-session-identities");
		}
	}
	nativeLifecycleResults->StartCase("post-close-window-lifecycle");
	const auto postCloseWindowPassed = windowsMatch("after primary close");
	recordChatCase("post-close-window-lifecycle", postCloseWindowPassed,
				   "primary close changed the separate session windows");

	struct MigrationResult {
		int done = 0;
		int failed = 0;
		QString error;
	};
	const auto migration = std::make_shared<MigrationResult>();
	ReportCallRegressionCheckpoint("migration callback begin");
	if (pinnedMigrationFixturePassed) {
		nativeLifecycleResults->StartCase("chat-migration");
		pinned->session().api().migrateChat(
			not_null<ChatData *>(pinnedActionChat),
			[migration](not_null<ChannelData *>) { ++migration->done; },
			[migration](const QString &error) {
				++migration->failed;
				migration->error = error;
			});
	} else {
		nativeLifecycleResults->Skip("chat-migration",
									 "dependency-pinned-migration-fixture");
	}
	nativeLifecycleResults->asyncPending = true;
	crl::on_main([=] {
		if (pinnedMigrationFixturePassed) {
			std::fprintf(
				stderr,
				"Pinned migration regression: done=%d failed=%d error=%s\n",
				migration->done, migration->failed,
				migration->error.toUtf8().constData());
			const auto migrationPassed
				= migration->done == 0 && migration->failed == 1
				  && migration->error == u"CLIENT_BAD_MIGRATION"_q;
			recordChatCase(
				"chat-migration", migrationPassed,
				"pinned basic-group migration was not rejected locally");
		}
		RunProtectedCacheCleanupRegression(
			stock, pinned, stockCachePath, stockMediaCachePath, pinnedCachePath,
			pinnedMediaCachePath, nativeLifecycleResults);
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		if (!nativeLifecycleResults->Passed()) {
			done(FailChatParticipantsRegression(
				"one or more named native lifecycle cases failed"));
			return;
		}
		std::fprintf(stderr, "Chat participants regression passed.\n");
		done(0);
	});
	ReportCallRegressionCheckpoint("migration callback scheduled");
	return 0;
}

[[nodiscard]] bool ReadOnlyUsernameInputRejectsWordDeletion() {
	const auto shortcuts
		= QKeySequence::keyBindings(QKeySequence::DeleteStartOfWord);
	if (shortcuts.isEmpty()) {
		return false;
	}
	Ui::UsernameInput input(nullptr, st::defaultInputField,
							rpl::single(u"@username"_q), u"operator"_q,
							QString());
	Ui::UsernameInput editable(nullptr, st::defaultInputField,
							   rpl::single(u"@username"_q), u"operator"_q,
							   QString());
	input.setReadOnly(true);
	Ui::EditPeer::UsernameEditorFlow flow;
	flow.inputChanged(u"operator"_q, true);
	auto requests = 0;
	auto checkStarted = false;
	QObject::connect(&input, &Ui::MaskedInputField::changed, &input, [&] {
		const auto name = input.getLastText();
		flow.inputChanged(name, Ui::EditPeer::IsValidPublicUsername(name));
		checkStarted = flow.checkAccount(
			name, [&](const QString &, auto, auto) { ++requests; }, [](bool) {},
			[](const QString &, auto, bool) {});
	});
	for (const auto &shortcut : shortcuts) {
		const auto key = shortcut[0];
		input.setSelection(6, 2);
		auto event
			= QKeyEvent(QEvent::KeyPress, key.key(), key.keyboardModifiers());
		QCoreApplication::sendEvent(&input, &event);
		if (input.text() != u"operator"_q
			|| input.getLastText() != u"operator"_q
			|| input.selectedText() != u"or"_q || requests != 0
			|| checkStarted) {
			std::fprintf(
				stderr,
				"Read-only username shortcut changed the handle or sent "
				"an availability request: handle=%s requests=%d.\n",
				input.text().toUtf8().constData(), requests);
			return false;
		}
		editable.setText(u"operator"_q);
		editable.setSelection(6, 2);
		auto editableEvent
			= QKeyEvent(QEvent::KeyPress, key.key(), key.keyboardModifiers());
		QCoreApplication::sendEvent(&editable, &editableEvent);
		if (editable.text() != u"operat"_q) {
			return false;
		}
	}
	std::fprintf(stderr, "Read-only username shortcut regression passed: "
						 "handle=operator requests=0.\n");
	return true;
}

[[nodiscard]] int StartAccountLifecycleRegression(Fn<void(int)> done) {
	auto nativeLifecycleResults = std::make_shared<NativeLifecycleResults>();
	const auto injectedFailCases
		= qEnvironmentVariable("TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE")
			  .split(',', Qt::SkipEmptyParts);
	const auto shouldInjectFailure = [&](const char *name) {
		return injectedFailCases.contains(QString::fromLatin1(name));
	};
	nativeLifecycleResults->StartCase("username-input");
	const auto usernameInputPassed = ReadOnlyUsernameInputRejectsWordDeletion()
									 && !shouldInjectFailure("username-input");
	nativeLifecycleResults->Report("username-input", usernameInputPassed);
	nativeLifecycleResults->StartCase("teagram-icon-persistence");
	const auto teagramIconPersistencePassed
		= TeagramIconChoicePersistsAcrossSettingsReload()
		  && !shouldInjectFailure("teagram-icon-persistence");
	nativeLifecycleResults->Report("teagram-icon-persistence",
								   teagramIconPersistencePassed);
	nativeLifecycleResults->StartCase("account-lifecycle-prerequisites");
	const auto failAccountPrerequisite = [&](const char *reason) {
		std::fprintf(stderr, "Account lifecycle fixture failed: %s\n", reason);
		nativeLifecycleResults->Report("account-lifecycle-prerequisites",
									   false);
		nativeLifecycleResults->SkipUnreported(
			"dependency-account-lifecycle-prerequisites");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return 1;
	};
	const auto failureVariable
		= QByteArray("TDESKTOP_FAIL_MTP_AUTHORIZATION_WRITE");
	const auto failWrites
		= gsl::finally([&] { qunsetenv(failureVariable.constData()); });
	ReportCallRegressionCheckpoint("account lifecycle setup begins");

	auto &domain = Core::App().domain();
	if (!domain.started()) {
		return failAccountPrerequisite("application domain did not start");
	}
	if (!Core::App().activePrimaryWindow()) {
		return failAccountPrerequisite(
			"application did not create the primary window fixture");
	}
	if (domain.accounts().size() != 1
		|| domain.accounts().front().account->sessionExists()) {
		return failAccountPrerequisite(
			"regression requires one fresh account in its isolated workdir");
	}
	auto fixtures = ProtectedCacheFixtures();
	if (!SetProtectedCacheFixturePaths(&fixtures)) {
		return failAccountPrerequisite(
			"could not prepare protected cache fixtures");
	}
	nativeLifecycleResults->Report("account-lifecycle-prerequisites", true);
	nativeLifecycleResults->StartCase("legacy-cleanup-symlink");
	const auto legacyCleanupPassed
		= RunLegacyCleanupSymlinkRegression(fixtures)
		  && !shouldInjectFailure("legacy-cleanup-symlink");
	nativeLifecycleResults->Report("legacy-cleanup-symlink",
								   legacyCleanupPassed);
	if (!legacyCleanupPassed) {
		std::fprintf(
			stderr,
			"Protected storage regression failed: legacy cleanup followed "
			"a protected account-directory symlink.\n");
	}

	// This is the production post-auth caller. It must keep the session
	// unpublished when the synchronous authorization write is refused.
	const auto account
		= not_null<Main::Account *>(domain.accounts().front().account.get());
	const auto cacheKey = Storage::Cache::Key{
		0x4d41494e31303632ULL,
		0x4341434845504159ULL,
	};
	const auto cachePayload = QByteArray("main-1062-cache-regression-payload");
	nativeLifecycleResults->StartCase("cache-callback-timeout");
	const auto cacheCallbackTimeoutPassed
		= CacheCallbackTimeoutIsSafe()
		  && !shouldInjectFailure("cache-callback-timeout");
	nativeLifecycleResults->Report("cache-callback-timeout",
								   cacheCallbackTimeoutPassed);
	nativeLifecycleResults->StartCase("account-authorization-lifecycle");
	const auto failAccountLifecycle = [&](const char *reason) {
		std::fprintf(stderr, "Account lifecycle regression failed: %s\n",
					 reason);
		nativeLifecycleResults->Report("account-authorization-lifecycle",
									   false);
		nativeLifecycleResults->SkipUnreported(
			"dependency-account-authorization-lifecycle");
		nativeLifecycleResults->finalized = true;
		nativeLifecycleResults->ReportSummary();
		return 1;
	};
	if (!IncompleteStoredSessionStartsPaused(domain)) {
		return failAccountLifecycle(
			"Account::start restored an incomplete stored session or did not "
			"keep its enrollment request paused");
	}
	if (!WriteCachePayload(account, cacheKey, cachePayload)) {
		return failAccountLifecycle("could not seed the cache payload");
	}
	if (!account->sessionExists()
		&& !account->mtp().dcOptions().hasCustomServer()
		&& (!account->mtp().dcOptions().unenrolled()
			|| !account->mtp().dcOptions().configEnumDcIds().empty())) {
		// A fresh account is allowed to show enrollment, but it must not
		// carry the built-in production table while it waits there.
		return failAccountLifecycle(
			"fresh account retained production endpoints before enrollment");
	}
	// Sender destruction must cancel a refused request before queued delivery.
	account->mtp().stopForServerEnrollment();
	auto cancelledFailures = 0;
	{
		auto sender = MTP::Sender(&account->mtp());
		const auto requestId
			= sender.request(MTPupdates_GetState())
				  .fail([&](const MTP::Error &) { ++cancelledFailures; })
				  .send();
		if (!sender.pending(requestId)) {
			return failAccountLifecycle(
				"stopped sender did not retain its request");
		}
	}
	QCoreApplication::processEvents();
	if (cancelledFailures) {
		return failAccountLifecycle(
			"destroyed sender delivered a cancelled request");
	}
	auto liveFailures = 0;
	{
		auto sender = MTP::Sender(&account->mtp());
		const auto requestId
			= sender.request(MTPupdates_GetState())
				  .fail([&](const MTP::Error &error) {
					  if (MTP::IsServerEnrollmentPausedError(error)) {
						  ++liveFailures;
					  }
				  })
				  .send();
		if (!sender.pending(requestId)) {
			return failAccountLifecycle(
				"paused sender did not retain its request");
		}
		QCoreApplication::processEvents();
		if (sender.pending(requestId) || liveFailures != 1) {
			return failAccountLifecycle(
				"paused request was not rejected locally");
		}
	}
	const auto originalKey = RegressionServerKey();
	const auto originalFingerprint = originalKey->fingerprint();
	if (!ConfigurePinnedServer(account, originalKey)) {
		return failAccountLifecycle("could not pin the test server");
	}
	account->setSessionUserId(UserId(4242));
	qputenv(failureVariable.constData(), "1");
	const auto published
		= account->createSession(UserId(4242), QByteArray(), 0,
								 std::make_unique<Main::SessionSettings>());
	if (published || account->sessionExists()
		|| account->willHaveSessionUniqueId(nullptr) == 0
		|| !account->local().mtpAuthorizationWriteFailed()) {
		return failAccountLifecycle(
			"post-auth write failure published a session or lost its marker");
	}
	qunsetenv(failureVariable.constData());

	// The failed post-auth write already persisted the block marker; leaving
	// it in place verifies startup restores that durable block.
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restart after the post-auth write failure");
	}

	const auto blocked = FindAuthorizationBlockedAccount(domain);
	if (!blocked || !blocked->mtp().config().blocked()) {
		return failAccountLifecycle(
			"post-auth failure did not restore a blocked account");
	}
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restart after blocked account teardown");
	}
	const auto stillBlocked = FindAuthorizationBlockedAccount(domain);
	if (!stillBlocked || !stillBlocked->mtp().config().blocked()) {
		return failAccountLifecycle(
			"blocked teardown cleared the durable authorization failure");
	}
	const auto restarted = Main::details::CommitServerForget(
		&stillBlocked->local(),
		Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = originalFingerprint,
			.userIdKnown = true,
			.userId = 4242,
		},
		[] {});
	if (!restarted || stillBlocked->local().hasStoredCustomServer()
		|| stillBlocked->local().mtpAuthorizationWriteFailed()) {
		return failAccountLifecycle(
			"Forget did not durably clear the blocked server state");
	}
	if (stillBlocked->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return failAccountLifecycle(
			"Forget left the persisted authorization snapshot before restart");
	}

	if (!RestartDomain(domain)) {
		return failAccountLifecycle("could not restart after Forget");
	}
	const auto forgotten
		= not_null<Main::Account *>(domain.accounts().front().account.get());
	if (!PersistedCachePayloadMatches(forgotten, cacheKey, cachePayload)
		|| !forgotten->mtp().dcOptions().unenrolled()
		|| forgotten->mtp().dcOptions().blocked()
		|| forgotten->local().hasStoredCustomServer()
		|| forgotten->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(forgotten)
		|| !forgotten->mtp().dcOptions().configEnumDcIds().empty()) {
		return failAccountLifecycle(
			"Forget did not restart unenrolled without auth data or endpoints");
	}
	auto pausedFailures = 0;
	{
		auto sender = MTP::Sender(&forgotten->mtp());
		const auto requestId
			= sender.request(MTPupdates_GetState())
				  .fail([&](const MTP::Error &error) {
					  if (MTP::IsServerEnrollmentPausedError(error)) {
						  ++pausedFailures;
					  }
				  })
				  .send();
		if (!sender.pending(requestId)) {
			return failAccountLifecycle(
				"unenrolled sender did not retain its request");
		}
		QCoreApplication::processEvents();
		if (sender.pending(requestId) || pausedFailures != 1) {
			return failAccountLifecycle(
				"unenrolled request was not rejected locally");
		}
	}

	const auto initialKey = RegressionAuthKey(0x11);
	forgotten->mtp().dcPersistentKeyChanged(2, initialKey);
	if (!ConfigurePinnedServer(forgotten, originalKey)) {
		return failAccountLifecycle(
			"could not configure the matching server after Forget");
	}
	forgotten->setSessionUserId(UserId(4242));
	if (!forgotten->createSession(RegressionUser(UserId(4242), true, QString()),
								  std::make_unique<Main::SessionSettings>())
		|| !forgotten->sessionExists()
		|| forgotten->serverCacheBindingMismatchPending()
		|| forgotten->local().checkServerCacheBinding(originalFingerprint, 4242)
			   != Storage::ServerCacheBindingStatus::Match
		|| !CachePayloadMatches(forgotten->session().data().cache(), cacheKey,
								cachePayload)) {
		return failAccountLifecycle("matching server and user identity did not "
									"reuse the retained cache payload");
	}
	forgotten->mtp().resume();
	const auto finalKey = RegressionAuthKey(0x22);
	const auto finalKeyId = finalKey->keyId();
	forgotten->mtp().dcPersistentKeyChanged(2, finalKey);
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restart after active account teardown");
	}
	auto active
		= not_null<Main::Account *>(domain.accounts().front().account.get());
	if (!active->sessionExists()) {
		return failAccountLifecycle(
			"active teardown regression fixture did not restore its self user");
	}
	if (!HasAuthKey(active, finalKeyId)
		|| !active->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return failAccountLifecycle(
			"active teardown did not persist its final authorization snapshot");
	}

	const auto teardownFailureKey = RegressionAuthKey(0x33);
	active->mtp().dcPersistentKeyChanged(2, teardownFailureKey);
	qputenv(failureVariable.constData(), "1");
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restart after teardown-only authorization failure");
	}
	qunsetenv(failureVariable.constData());
	auto failedTeardown = FindAuthorizationBlockedAccount(domain);
	if (!failedTeardown
		|| !failedTeardown->mtp().config().blocked()
		|| !failedTeardown->local().mtpAuthorizationWriteFailed()) {
		return failAccountLifecycle(
			"active teardown-only failure did not block the next startup");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	if (!failedTeardown->local().writeMtpAuthorizationFailure()) {
		return failAccountLifecycle(
			"could not rewrite the retained authorization failure marker");
	}
	const auto markerRewriteAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (markerRewriteAttempts.authorizationSnapshot != 0
		|| markerRewriteAttempts.authorizationFailureMarker != 1
		|| markerRewriteAttempts.customServerBlockMarker != 0) {
		return failAccountLifecycle("authorization failure marker observer "
									"missed an identical-value rewrite");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	auto blockedTeardownAttempts = LifecycleWriteCountsForRegressionTest();
	if (!RestartDomain(domain, &blockedTeardownAttempts)) {
		return failAccountLifecycle("could not restart after blocked teardown");
	}
	if (blockedTeardownAttempts.authorizationSnapshot != 0
		|| blockedTeardownAttempts.authorizationFailureMarker != 0
		|| blockedTeardownAttempts.customServerBlockMarker != 0) {
		return failAccountLifecycle(
			"blocked account teardown attempted an authorization or marker "
			"write");
	}
	failedTeardown = FindAuthorizationBlockedAccount(domain);
	if (!failedTeardown || !failedTeardown->mtp().config().blocked()
		|| !failedTeardown->local().mtpAuthorizationWriteFailed()) {
		return failAccountLifecycle(
			"blocked teardown changed its durable authorization state");
	}
	if (!failedTeardown->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return failAccountLifecycle(
			"blocked teardown lost the prior authorization snapshot");
	}
	if (!Main::details::CommitServerForget(
			&failedTeardown->local(),
			Storage::ServerCacheBinding{
				.fingerprintKnown = true,
				.fingerprint = originalFingerprint,
				.userIdKnown = true,
				.userId = 4242,
			},
			[] {})) {
		return failAccountLifecycle(
			"could not Forget the blocked account after teardown");
	}
	if (failedTeardown->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return failAccountLifecycle(
			"Forget left the prior authorization snapshot before restart");
	}
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not return to enrollment after teardown block");
	}
	auto unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)
		|| unenrolled->local().checkServerCacheBinding(originalFingerprint,
													   4242)
			   != Storage::ServerCacheBindingStatus::Match) {
		return failAccountLifecycle(
			"Forget did not retain only the cache identity binding");
	}
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"paused teardown did not complete its restart");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)) {
		return failAccountLifecycle(
			"paused teardown wrote authorization data or a failure marker");
	}

	if (!CacheMismatchWaitsForDestructiveConfirmation(
			unenrolled,
			RegressionOtherServerKey(),
			UserId(4242),
			originalFingerprint,
			4242)) {
		return failAccountLifecycle(
			"changed server fingerprint bypassed cache confirmation");
	}
	if (!PersistedCachePayloadMatches(
			unenrolled,
			cacheKey,
			cachePayload)) {
		return failAccountLifecycle(
			"declined fingerprint change damaged the cached payload");
	}
	if (!Main::details::CommitServerForget(
			&unenrolled->local(),
			Storage::ServerCacheBinding{
				.fingerprintKnown = true,
				.fingerprint = originalFingerprint,
				.userIdKnown = true,
				.userId = 4242,
			},
			[] {})
		|| !RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not reset the declined fingerprint change");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!CacheMismatchWaitsForDestructiveConfirmation(
			unenrolled,
			originalKey,
			UserId(5252),
			originalFingerprint,
			4242)) {
		return failAccountLifecycle(
			"changed user id bypassed cache confirmation");
	}
	if (!PersistedCachePayloadMatches(
			unenrolled,
			cacheKey,
			cachePayload)) {
		return failAccountLifecycle(
			"declined user-id change damaged the cached payload");
	}
	if (!unenrolled->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::DestructiveConfirmation,
			true)
		|| !unenrolled->local().serverReenrollmentPending()
		|| !RestartDomain(domain)) {
		return failAccountLifecycle(
			"confirmed identity change did not schedule destructive cleanup");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)
		|| unenrolled->local().hasStoredCustomServer()
		|| unenrolled->local().customServerPinUnknown()
		|| unenrolled->local().checkServerCacheBinding(
			originalFingerprint,
			4242) != Storage::ServerCacheBindingStatus::None) {
		return failAccountLifecycle(
			"confirmed identity change did not discard the old cache binding");
	}

	unenrolled->local().writeCustomServerBlocked(false);
	if (!RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restart the synthetic blocked account");
	}
	auto blockedWithoutAuthorizationFailure = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!blockedWithoutAuthorizationFailure->mtp().config().blocked()
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)
		|| !blockedWithoutAuthorizationFailure->local().hasStoredCustomServer()
		|| blockedWithoutAuthorizationFailure->local().customServerPinUnknown()) {
		return failAccountLifecycle("blocked startup did not retain its "
									"existing marker and empty auth state");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	qputenv(failureVariable.constData(), "1");
	const auto authorizationAttemptFailed =
		!blockedWithoutAuthorizationFailure->local().writeMtpAuthorization();
	qunsetenv(failureVariable.constData());
	const auto authorizationAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (!authorizationAttemptFailed
		|| authorizationAttempts.authorizationSnapshot != 1
		|| authorizationAttempts.authorizationFailureMarker != 0
		|| authorizationAttempts.customServerBlockMarker != 0
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)) {
		return failAccountLifecycle(
			"authorization snapshot observer missed an isolated write attempt");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	blockedWithoutAuthorizationFailure->local().writeCustomServerBlocked(false);
	const auto markerAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (markerAttempts.authorizationSnapshot != 0
		|| markerAttempts.authorizationFailureMarker != 0
		|| markerAttempts.customServerBlockMarker != 1
		|| !blockedWithoutAuthorizationFailure->local().hasStoredCustomServer()) {
		return failAccountLifecycle(
			"block marker observer missed an identical-value rewrite");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	auto teardownAttempts = LifecycleWriteCountsForRegressionTest();
	if (!RestartDomain(domain, &teardownAttempts)) {
		return failAccountLifecycle(
			"blocked teardown did not complete its restart");
	}
	if (teardownAttempts.authorizationSnapshot != 0
		|| teardownAttempts.authorizationFailureMarker != 0
		|| teardownAttempts.customServerBlockMarker != 0) {
		return failAccountLifecycle("blocked account teardown attempted an "
									"authorization or marker write");
	}
	blockedWithoutAuthorizationFailure = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!blockedWithoutAuthorizationFailure->mtp().config().blocked()
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)) {
		return failAccountLifecycle(
			"blocked teardown wrote authorization data or a failure marker");
	}
	if (!Main::details::CommitServerForget(
			&blockedWithoutAuthorizationFailure->local(),
			kSyntheticStockCacheBindingForRegression, [] {})
		|| blockedWithoutAuthorizationFailure->local().hasStoredCustomServer()
		|| blockedWithoutAuthorizationFailure->local().customServerPinUnknown()
		|| !blockedWithoutAuthorizationFailure->local()
				.writeMtpAuthorizationFailure()
		|| !RestartDomain(domain)) {
		return failAccountLifecycle(
			"could not restore a blocked account without custom-pin metadata");
	}
	const auto restoredBlocked = FindAuthorizationBlockedAccount(domain);
	if (!restoredBlocked || !restoredBlocked->mtp().config().blocked()
		|| restoredBlocked->sessionExists()
		|| !restoredBlocked->local().mtpAuthorizationWriteFailed()
		|| restoredBlocked->local().hasStoredCustomServer()
		|| restoredBlocked->local().customServerPinUnknown()) {
		return failAccountLifecycle(
			"restored blocked account retained custom-pin metadata");
	}
	if (!PrepareSyntheticStockAccountForRegression(restoredBlocked)) {
		return failAccountLifecycle(
			"could not reset the synthetic account after blocked-state checks");
	}

	nativeLifecycleResults->Report("account-authorization-lifecycle", true);
	ReportCallRegressionCheckpoint("account lifecycle setup complete");
	return StartChatParticipantsRegression(
		domain, fixtures, std::move(nativeLifecycleResults), std::move(done));
}

} // namespace

void RunAccountLifecycleRegression(Fn<void(int)> done) {
	if (const auto result = StartAccountLifecycleRegression(done)) {
		done(result);
	}
}

} // namespace Tests
