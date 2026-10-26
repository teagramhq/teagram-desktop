/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_signup_terms.h"

namespace Intro::details {

void RunSignupWithTerms(bool termsTextPresent, bool accepted,
						Fn<void(Fn<void()>, Fn<void()>)> showTerms,
						Fn<void()> sendSignup, Fn<void()> declined) {
	if (termsTextPresent && !accepted) {
		showTerms(std::move(sendSignup), std::move(declined));
	} else {
		sendSignup();
	}
}

} // namespace Intro::details
