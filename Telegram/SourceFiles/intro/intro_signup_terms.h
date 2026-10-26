/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/bytes.h"
#include "ui/text/text_entity.h"

#include <optional>

class MTPDhelp_termsOfService;

namespace Intro::details {

struct SignupTermsPayload {
	bytes::vector id;
	TextWithEntities text;
	std::optional<int> minAge;
	bool popup = false;

	[[nodiscard]] bool hasSameConsent(
		const SignupTermsPayload &other) const;
};

[[nodiscard]] SignupTermsPayload SignupTermsPayloadFromMTP(
	const MTPDhelp_termsOfService &data);

[[nodiscard]] bool HandleSignupTermsKey(
	int key,
	bool declineOnEscape,
	Fn<void()> accept,
	Fn<void()> decline);

class SignupTermsGate;

class SignupTermsPermission final {
public:
	SignupTermsPermission(SignupTermsPermission &&) = default;
	SignupTermsPermission &operator=(SignupTermsPermission &&) = default;
	SignupTermsPermission(const SignupTermsPermission &) = delete;
	SignupTermsPermission &operator=(const SignupTermsPermission &) = delete;

private:
	friend class SignupTermsGate;
	SignupTermsPermission() = default;
};

class SignupTermsGate final {
public:
	void submit(
		const SignupTermsPayload &terms,
		Fn<void(Fn<void()>, Fn<void()>)> showTerms,
		Fn<void(SignupTermsPermission)> sendSignup,
		Fn<void()> declined);

private:
	std::optional<SignupTermsPayload> _accepted;
};

} // namespace Intro::details
