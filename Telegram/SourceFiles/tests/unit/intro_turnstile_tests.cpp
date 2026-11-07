/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_turnstile.h"
#include "intro/intro_turnstile_request.h"
#include "mtproto/details/mtproto_serialized_request.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace Intro::details;

constexpr auto kInitialUrl = QStringView(
	u"https://web.teagram.io/turnstile.html#sitekey=abc_123&action=signup");

[[nodiscard]] TurnstileMessageResult ParseMessage(
	std::string_view message, TurnstileAction action = TurnstileAction::SignIn,
	bool pending = true, bool navigationDone = true,
	QStringView mainFrameUrl = u"https://web.teagram.io/turnstile.html") {
	return ParseTurnstileMessage(message, action, pending, navigationDone,
								 mainFrameUrl);
}

[[nodiscard]] std::string TokenMessage(const std::string &token,
									   const std::string &action = "signin") {
	const auto payload
		= QJsonDocument(QJsonObject{
							{u"token"_q, QString::fromStdString(token)},
							{u"action"_q, QString::fromStdString(action)},
						})
			  .toJson(QJsonDocument::Compact);
	return QJsonDocument(
			   QJsonArray{
				   u"turnstile_token"_q,
				   QString::fromUtf8(payload.constData(), payload.size()),
			   })
		.toJson(QJsonDocument::Compact)
		.toStdString();
}

} // namespace

TEST_CASE(ParsesTurnstileChallengeFromServerError) {
	const auto signIn
		= ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signin__site_key-123"_q);
	CHECK(signIn.has_value());
	CHECK(signIn->action() == TurnstileAction::SignIn);
	CHECK_EQ(signIn->siteKey(), u"site_key-123"_q);

	const auto signUp
		= ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signup__demoKey"_q);
	CHECK(signUp.has_value());
	CHECK(signUp->action() == TurnstileAction::SignUp);
}

TEST_CASE(RejectsMalformedTurnstileChallengeErrors) {
	CHECK(IsTurnstileChallengeError(u"RECAPTCHA_CHECK_signin__key"));
	CHECK(!IsTurnstileChallengeError(u"PHONE_NUMBER_INVALID"));
	for (const auto &error : {
			 u"RECAPTCHA_CHECK_signin"_q,
			 u"RECAPTCHA_CHECK_signin__"_q,
			 u"RECAPTCHA_CHECK_admin__sitekey"_q,
			 u"RECAPTCHA_CHECK_signup__bad.key"_q,
			 u"RECAPTCHA_CHECK_signup__bad%key"_q,
			 u"PHONE_NUMBER_INVALID"_q,
		 }) {
		CHECK(!ParseTurnstileChallenge(error).has_value());
	}
	CHECK(!ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signup__"_q
								   + QString(257, 'x'))
			   .has_value());
	CHECK(ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signup__key__suffix"_q)
			  .has_value());
}

TEST_CASE(BuildsHardcodedTurnstileUrlWithFragmentParameters) {
	const auto challenge
		= *ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signup__key_-123"_q);
	CHECK_EQ(
		TurnstileUrl(challenge),
		u"https://web.teagram.io/turnstile.html#sitekey=key_-123&action=signup"_q);
}

TEST_CASE(AcceptsOnlyTheTurnstileMainFrameUrl) {
	CHECK(IsTurnstileMainFrameUrl(
		u"https://web.teagram.io/turnstile.html#sitekey=x&action=signin"_q));
	CHECK(IsTurnstileMainFrameUrl(
		u"https://WEB.TEAGRAM.IO:443/turnstile.html"_q));
	CHECK(!IsTurnstileMainFrameUrl(QString()));
	CHECK(!IsTurnstileMainFrameUrl(u"https://web.teagram.io/other.html"_q));
	CHECK(!IsTurnstileMainFrameUrl(u"https://evil.example/turnstile.html"_q));
	CHECK(!IsTurnstileMainFrameUrl(
		u"https://web.teagram.io:444/turnstile.html"_q));
	CHECK(!IsTurnstileMainFrameUrl(
		u"https://user@web.teagram.io/turnstile.html"_q));
	CHECK(!IsTurnstileMainFrameUrl(u"http://web.teagram.io/turnstile.html"_q));
}

TEST_CASE(AcceptsTokenOnlyForCompletedPendingChallenge) {
	const auto result = ParseMessage(TokenMessage("token 123~"));
	CHECK(std::holds_alternative<TurnstileMessage>(result));
	const auto &message = std::get<TurnstileMessage>(result);
	CHECK(message.type == TurnstileMessageType::Token);
	CHECK_EQ(message.token, u"token 123~"_q);

	CHECK(std::get<TurnstileMessageRejection>(ParseMessage(
			  TokenMessage("token"), TurnstileAction::SignIn, false))
		  == TurnstileMessageRejection::NoChallengePending);
	CHECK(std::get<TurnstileMessageRejection>(ParseMessage(
			  TokenMessage("token"), TurnstileAction::SignIn, true, false))
		  == TurnstileMessageRejection::NavigationNotComplete);
}

TEST_CASE(RejectsBridgeEventsOutsideTheChallengeMainFrame) {
	for (const auto &url : {
			 u""_q,
			 u"https://evil.example/turnstile.html"_q,
			 u"https://web.teagram.io/other.html"_q,
		 }) {
		CHECK(std::get<TurnstileMessageRejection>(
				  ParseMessage(TokenMessage("token"), TurnstileAction::SignIn,
							   true, true, url))
			  == TurnstileMessageRejection::InvalidMainFrameUrl);
	}
}

TEST_CASE(RejectsTokenActionDifferentFromPendingChallenge) {
	const auto result = ParseMessage(TokenMessage("token", "signup"));
	CHECK(std::get<TurnstileMessageRejection>(result)
		  == TurnstileMessageRejection::ActionMismatch);
}

TEST_CASE(RejectsOversizedOrMalformedTurnstileTokens) {
	const auto oversized = ParseMessage(TokenMessage(std::string(2049, 'x')));
	CHECK(std::get<TurnstileMessageRejection>(oversized)
		  == TurnstileMessageRejection::InvalidToken);

	const auto empty = ParseMessage(TokenMessage(""));
	CHECK(std::get<TurnstileMessageRejection>(empty)
		  == TurnstileMessageRejection::InvalidToken);

	const auto control = ParseMessage(TokenMessage("bad\x01token"));
	CHECK(std::get<TurnstileMessageRejection>(control)
		  == TurnstileMessageRejection::InvalidToken);

	const auto unicode = ParseMessage(TokenMessage("token \xF0\x9F\x8C\x8D"));
	CHECK(std::get<TurnstileMessageRejection>(unicode)
		  == TurnstileMessageRejection::InvalidToken);
}

TEST_CASE(RejectsOversizedAndMalformedBridgeMessages) {
	const auto oversized = ParseMessage(std::string(1024 * 1024 + 1, 'x'));
	CHECK(std::get<TurnstileMessageRejection>(oversized)
		  == TurnstileMessageRejection::PayloadTooLarge);

	CHECK(std::get<TurnstileMessageRejection>(ParseMessage("not-json"))
		  == TurnstileMessageRejection::InvalidJson);
	CHECK(std::get<TurnstileMessageRejection>(
			  ParseMessage(R"(["turnstile_token",{}])"))
		  == TurnstileMessageRejection::InvalidEnvelope);
	CHECK(std::get<TurnstileMessageRejection>(
			  ParseMessage(R"(["web_app_close","{}"])"))
		  == TurnstileMessageRejection::UnknownEvent);
}

TEST_CASE(AcceptsTurnstileErrorEventsWithoutDisplayingPageText) {
	const auto error
		= ParseMessage(R"(["turnstile_error","{\"code\":\"expired\"}"])");
	CHECK(std::holds_alternative<TurnstileMessage>(error));
	const auto &message = std::get<TurnstileMessage>(error);
	CHECK(message.type == TurnstileMessageType::Error);
	CHECK(message.token.isEmpty());

	const auto unknownCode
		= ParseMessage(R"(["turnstile_error","{\"code\":\"raw-page-text\"}"])");
	CHECK(std::holds_alternative<TurnstileMessage>(unknownCode));
	CHECK(std::get<TurnstileMessage>(unknownCode).type
		  == TurnstileMessageType::Error);
}

TEST_CASE(RefusesSecondAndNewWindowNavigations) {
	const auto initial = kInitialUrl.toString();
	const auto first
		= CheckTurnstileNavigation(initial, initial, true, false, false);
	CHECK(!first.rejection.has_value());
	CHECK(first.initialNavigation);

	const auto second
		= CheckTurnstileNavigation(initial, initial, false, false, false);
	CHECK(second.rejection.has_value());
	CHECK(*second.rejection
		  == TurnstileNavigationRejection::UnexpectedNavigation);

	const auto sameOriginPath = CheckTurnstileNavigation(
		u"https://web.teagram.io/other.html"_q, initial, false, false, false);
	CHECK(sameOriginPath.rejection.has_value());

	const auto newWindow
		= CheckTurnstileNavigation(initial, initial, true, true, false);
	CHECK(newWindow.rejection.has_value());
	CHECK(*newWindow.rejection == TurnstileNavigationRejection::NewWindow);
}

TEST_CASE(TurnstileNavigationDecisionDefaultsToReject) {
	const auto decision = TurnstileNavigationDecision{};
	CHECK(!decision.initialNavigation);
	CHECK(decision.rejection.has_value());
	CHECK(*decision.rejection
		  == TurnstileNavigationRejection::UnexpectedNavigation);
}

TEST_CASE(RefusesTurnstileReloadOnLinuxPolicyPath) {
	const auto initial = kInitialUrl.toString();
	const auto reload
		= CheckTurnstileNavigation(initial, initial, false, false, true);
	CHECK(reload.rejection.has_value());
	CHECK(*reload.rejection
		  == TurnstileNavigationRejection::UnexpectedNavigation);
}

TEST_CASE(AllowsOnlyTurnstileSubframeDestinationsOnLinux) {
	for (const auto &url : {
			 u"https://challenges.cloudflare.com/cdn-cgi/challenge"_q,
			 u"about:blank"_q,
			 u"about:srcdoc"_q,
		 }) {
		CHECK(!CheckTurnstileNavigation(url, kInitialUrl, false, false, true)
				   .rejection.has_value());
	}
	CHECK(CheckTurnstileNavigation(
			  u"https://challenges.cloudflare.com.evil.example/"_q, kInitialUrl,
			  false, false, true)
			  .rejection.has_value());
	CHECK(CheckTurnstileNavigation(u"https://challenges.cloudflare.com/"_q,
								   kInitialUrl, false, false, false)
			  .rejection.has_value());
}

TEST_CASE(TurnstileLinuxNavigationGuardAllowsOnlyTheChallengeMainFrameUrl) {
	auto guard = TurnstileLinuxNavigationGuard();
	CHECK(!guard.mainFrameUrlUpdated(u"about:blank"_q, kInitialUrl));

	guard.initialLoadCompleted();
	CHECK(!guard.mainFrameUrlUpdated(kInitialUrl, kInitialUrl));
	CHECK(!guard.mainFrameUrlUpdated(
		u"https://WEB.TEAGRAM.IO:443/turnstile.html#sitekey=abc_123&action=signup"_q,
		kInitialUrl));
}

TEST_CASE(TurnstileLinuxNavigationGuardEndsChallengeOnDifferentMainFrameUrl) {
	auto guard = TurnstileLinuxNavigationGuard();
	guard.initialLoadCompleted();

	CHECK(guard.mainFrameUrlUpdated(
		u"https://challenges.cloudflare.com/cdn-cgi/challenge"_q, kInitialUrl));
	CHECK(!guard.mainFrameUrlUpdated(kInitialUrl, kInitialUrl));
}

TEST_CASE(TurnstileLinuxNavigationGuardIgnoresEventsAfterFinish) {
	auto guard = TurnstileLinuxNavigationGuard();
	guard.initialLoadCompleted();
	guard.finish();

	CHECK(!guard.mainFrameUrlUpdated(u"about:blank"_q, kInitialUrl));
}

TEST_CASE(InvokesTurnstileCallbackWhenBoxIsGone) {
	auto owner = std::make_unique<QObject>();
	const auto weak = QPointer<QObject>(owner.get());
	auto closed = false;
	auto called = false;
	DeferTurnstileCallback(
		[&] {
			if (weak) {
				closed = true;
			}
		},
		[&] { called = true; });
	CHECK(!called);
	owner.reset();
	QCoreApplication::processEvents();
	CHECK(called);
	CHECK(!closed);
}

TEST_CASE(BuildsWrappedAuthRequestOnlyWhenTokenIsPresent) {
	const auto wrapped
		= BuildAuthRequest(MTPaccount_GetPassword(), u"challenge-token"_q);
	using Wrapped = MTPInvokeWithReCaptcha<MTPaccount_GetPassword>;
	CHECK(std::holds_alternative<Wrapped>(wrapped));
	const auto serialized = MTP::details::SerializedRequest::Serialize(
		std::get<Wrapped>(wrapped));
	const auto &buffer = *serialized;
	const auto bodyOffset
		= MTP::details::SerializedRequest::kMessageBodyPosition;
	const auto bytes = QByteArray(
		reinterpret_cast<const char *>(buffer.constData() + bodyOffset),
		int((buffer.size() - bodyOffset) * sizeof(mtpPrime)));
	CHECK_EQ(bytes.left(4), QByteArray("\x94\x0f\xbb\xad", 4));
	CHECK(bytes.contains("challenge-token"));

	const auto plain = BuildAuthRequest(MTPaccount_GetPassword(), QString());
	CHECK(std::holds_alternative<MTPaccount_GetPassword>(plain));
}
