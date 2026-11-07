/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_turnstile.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <string>
#include <string_view>

namespace {

using namespace Intro::details;

constexpr auto kInitialUrl = QStringView(
	u"https://web.teagram.io/turnstile.html#sitekey=abc_123&action=signup");

[[nodiscard]] TurnstileMessageResult
ParseMessage(std::string_view message, QStringView action = u"signin",
			 bool pending = true, bool navigationDone = true,
			 QStringView mainFrameUrl
			 = u"https://web.teagram.io/turnstile.html") {
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
	CHECK_EQ(signIn->action, u"signin"_q);
	CHECK_EQ(signIn->siteKey, u"site_key-123"_q);

	const auto signUp
		= ParseTurnstileChallenge(u"RECAPTCHA_CHECK_signup__demoKey"_q);
	CHECK(signUp.has_value());
	CHECK_EQ(signUp->action, u"signup"_q);
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
	const auto challenge = TurnstileChallenge{
		u"key_-123"_q,
		u"signup"_q,
	};
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
	CHECK(result.message.has_value());
	CHECK(result.rejection == TurnstileMessageRejection::None);
	CHECK(result.message->type == TurnstileMessageType::Token);
	CHECK_EQ(result.message->token, u"token 123~"_q);

	CHECK(ParseMessage(TokenMessage("token"), u"signin", false).rejection
		  == TurnstileMessageRejection::NoChallengePending);
	CHECK(ParseMessage(TokenMessage("token"), u"signin", true, false).rejection
		  == TurnstileMessageRejection::NavigationNotComplete);
}

TEST_CASE(RejectsBridgeEventsOutsideTheChallengeMainFrame) {
	for (const auto &url : {
			 u""_q,
			 u"https://evil.example/turnstile.html"_q,
			 u"https://web.teagram.io/other.html"_q,
		 }) {
		CHECK(ParseMessage(TokenMessage("token"), u"signin", true, true, url)
				  .rejection
			  == TurnstileMessageRejection::InvalidMainFrameUrl);
	}
}

TEST_CASE(RejectsTokenActionDifferentFromPendingChallenge) {
	const auto result = ParseMessage(TokenMessage("token", "signup"));
	CHECK(!result.message.has_value());
	CHECK(result.rejection == TurnstileMessageRejection::ActionMismatch);
}

TEST_CASE(RejectsOversizedOrMalformedTurnstileTokens) {
	const auto oversized = ParseMessage(TokenMessage(std::string(2049, 'x')));
	CHECK(oversized.rejection == TurnstileMessageRejection::InvalidToken);

	const auto empty = ParseMessage(TokenMessage(""));
	CHECK(empty.rejection == TurnstileMessageRejection::InvalidToken);

	const auto control = ParseMessage(TokenMessage("bad\x01token"));
	CHECK(control.rejection == TurnstileMessageRejection::InvalidToken);

	const auto unicode = ParseMessage(TokenMessage("token \xF0\x9F\x8C\x8D"));
	CHECK(unicode.rejection == TurnstileMessageRejection::InvalidToken);
}

TEST_CASE(RejectsOversizedAndMalformedBridgeMessages) {
	const auto oversized = ParseMessage(std::string(1024 * 1024 + 1, 'x'));
	CHECK(oversized.rejection == TurnstileMessageRejection::PayloadTooLarge);

	CHECK(ParseMessage("not-json").rejection
		  == TurnstileMessageRejection::InvalidJson);
	CHECK(ParseMessage(R"(["turnstile_token",{}])").rejection
		  == TurnstileMessageRejection::InvalidEnvelope);
	CHECK(ParseMessage(R"(["web_app_close","{}"])").rejection
		  == TurnstileMessageRejection::UnknownEvent);
}

TEST_CASE(AcceptsTurnstileErrorEventsWithoutDisplayingPageText) {
	const auto error
		= ParseMessage(R"(["turnstile_error","{\"code\":\"expired\"}"])");
	CHECK(error.message.has_value());
	CHECK(error.message->type == TurnstileMessageType::Error);
	CHECK(error.message->token.isEmpty());

	const auto unknownCode
		= ParseMessage(R"(["turnstile_error","{\"code\":\"raw-page-text\"}"])");
	CHECK(unknownCode.message.has_value());
	CHECK(unknownCode.message->type == TurnstileMessageType::Error);
}

TEST_CASE(RefusesSecondAndNewWindowNavigations) {
	const auto initial = kInitialUrl.toString();
	const auto first
		= CheckTurnstileNavigation(initial, initial, true, false, false);
	CHECK(first.allowed);
	CHECK(first.initialNavigation);

	const auto second
		= CheckTurnstileNavigation(initial, initial, false, false, false);
	CHECK(!second.allowed);
	CHECK(second.rejection
		  == TurnstileNavigationRejection::UnexpectedNavigation);

	const auto sameOriginPath = CheckTurnstileNavigation(
		u"https://web.teagram.io/other.html"_q, initial, false, false, false);
	CHECK(!sameOriginPath.allowed);

	const auto newWindow
		= CheckTurnstileNavigation(initial, initial, true, true, false);
	CHECK(!newWindow.allowed);
	CHECK(newWindow.rejection == TurnstileNavigationRejection::NewWindow);
}

TEST_CASE(AllowsOnlyTurnstileSubframeDestinationsOnLinux) {
	for (const auto &url : {
			 u"https://challenges.cloudflare.com/cdn-cgi/challenge"_q,
			 u"about:blank"_q,
			 u"about:srcdoc"_q,
		 }) {
		CHECK(CheckTurnstileNavigation(url, kInitialUrl, false, false, true)
				  .allowed);
	}
	CHECK(!CheckTurnstileNavigation(
			   u"https://challenges.cloudflare.com.evil.example/"_q,
			   kInitialUrl, false, false, true)
			   .allowed);
	CHECK(!CheckTurnstileNavigation(u"https://challenges.cloudflare.com/"_q,
									kInitialUrl, false, false, false)
			   .allowed);
}
