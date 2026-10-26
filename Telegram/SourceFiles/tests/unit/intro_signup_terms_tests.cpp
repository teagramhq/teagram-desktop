/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_signup_terms.h"

namespace Intro::details {
namespace {

TEST_CASE(SignupTermsSendOnlyAfterAcceptance) {
	auto prompted = false;
	auto signupRequests = 0;
	Fn<void()> accept;
	Fn<void()> decline;

	RunSignupWithTerms(
		true, false,
		[&](Fn<void()> acceptNow, Fn<void()> declineNow) {
			prompted = true;
			accept = std::move(acceptNow);
			decline = std::move(declineNow);
		},
		[&] { ++signupRequests; }, [] {});

	CHECK(prompted);
	CHECK_EQ(signupRequests, 0);
	accept();
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(SignupTermsDeclineDoesNotSendSignup) {
	auto prompted = false;
	auto declined = false;
	auto signupRequests = 0;
	Fn<void()> accept;
	Fn<void()> decline;

	RunSignupWithTerms(
		true, false,
		[&](Fn<void()> acceptNow, Fn<void()> declineNow) {
			prompted = true;
			accept = std::move(acceptNow);
			decline = std::move(declineNow);
		},
		[&] { ++signupRequests; }, [&] { declined = true; });

	CHECK(prompted);
	CHECK_EQ(signupRequests, 0);
	decline();
	CHECK(declined);
	CHECK_EQ(signupRequests, 0);
}

TEST_CASE(SignupTermsAlreadyAcceptedDoesNotPromptAgain) {
	auto prompted = false;
	auto signupRequests = 0;

	RunSignupWithTerms(
		true, true, [&](Fn<void()>, Fn<void()>) { prompted = true; },
		[&] { ++signupRequests; }, [] {});

	CHECK(!prompted);
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(SignupWithoutTermsSendsWithoutPrompt) {
	auto prompted = false;
	auto signupRequests = 0;

	RunSignupWithTerms(
		false, false, [&](Fn<void()>, Fn<void()>) { prompted = true; },
		[&] { ++signupRequests; }, [] {});

	CHECK(!prompted);
	CHECK_EQ(signupRequests, 1);
}

} // namespace
} // namespace Intro::details
