/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_username_validation.h"

#include "base/basic_types.h"
#include "base/qt/qt_string_view.h" // base::StringViewMid
#include "data/data_input_policy.h"

#include <QtCore/QRegularExpression>

namespace Intro {
namespace details {

QString NormalizeUsernameInput(const QString &input) {
	auto result = input.trimmed();
	if (result.startsWith('@')) {
		result.remove(0, 1);
	}
	return result;
}

QString ToWireUsername(const QString &normalized) {
	return normalized.toLower();
}

bool IsValidUsername(
		const QString &normalized,
		const UsernamePolicy &policy) {
	if (normalized.isEmpty()) {
		return false;
	}
	const auto length = Data::details::UnicodeScalarCount(normalized);
	if ((policy.minLength > 0 && length < policy.minLength)
		|| (policy.maxLength > 0 && length > policy.maxLength)) {
		return false;
	} else if (policy.pattern.isEmpty()) {
		return true;
	}
	const auto pattern = QRegularExpression(policy.pattern);
	return !pattern.isValid() || pattern.match(normalized).hasMatch();
}

int FloodWaitSeconds(const QString &errorType) {
	auto from = errorType.size();
	while (from > 0 && errorType[from - 1].isDigit()) {
		--from;
	}
	return base::StringViewMid(errorType, from).toInt();
}

QString NormalizeSignupNameInput(const QString &input) {
	return input.trimmed();
}

SignupNameValidation ValidateSignupName(const QString &input, int maxLength) {
	const auto normalized = NormalizeSignupNameInput(input);
	if (normalized.isEmpty()) {
		return SignupNameValidation::Empty;
	} else if (!Data::details::IsWithinInputLengthPolicy(normalized, 0,
														 maxLength)) {
		return SignupNameValidation::TooLong;
	}
	return SignupNameValidation::Valid;
}

SignupPasswordValidation ValidateSignupPassword(
		const QString &password,
		const QString &repeat) {
	if (password.isEmpty()) {
		return SignupPasswordValidation::Empty;
	} else if (Data::details::UnicodeScalarCount(password) < 8) {
		return SignupPasswordValidation::TooShort;
	} else if (repeat.isEmpty()) {
		return SignupPasswordValidation::RepeatEmpty;
	} else if (password != repeat) {
		return SignupPasswordValidation::Mismatch;
	}
	return SignupPasswordValidation::Valid;
}

SignupPasswordUpdateFailure ClassifySignupPasswordUpdateFailure(
		const QString &errorType) {
	if (errorType.startsWith(u"FLOOD_WAIT_"_q)
		|| errorType.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)) {
		return SignupPasswordUpdateFailure::Flood;
	} else if (errorType == u"NEW_PASSWORD_BAD"_q
		|| errorType == u"NEW_SALT_INVALID"_q) {
		return SignupPasswordUpdateFailure::InvalidVerifier;
	}
	return SignupPasswordUpdateFailure::Other;
}

SigninPasswordValidation ValidateSigninPassword(const QString &input) {
	return input.isEmpty()
		? SigninPasswordValidation::Empty
		: SigninPasswordValidation::Valid;
}

SigninPasswordDataFailure ClassifySigninPasswordDataFailure(
		const QString &errorType) {
	return errorType.startsWith(u"FLOOD_WAIT_"_q)
		|| errorType.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)
		? SigninPasswordDataFailure::Flood
		: SigninPasswordDataFailure::Other;
}

SigninPasswordFailure ClassifySigninPasswordFailure(
		const QString &errorType) {
	if (ClassifySigninPasswordDataFailure(errorType)
		== SigninPasswordDataFailure::Flood) {
		return SigninPasswordFailure::Flood;
	} else if (errorType == u"PASSWORD_HASH_INVALID"_q
		|| errorType == u"SRP_PASSWORD_CHANGED"_q) {
		return SigninPasswordFailure::WrongPassword;
	} else if (errorType == u"PASSWORD_EMPTY"_q) {
		return SigninPasswordFailure::PasswordEmpty;
	} else if (errorType == u"AUTH_KEY_UNREGISTERED"_q) {
		return SigninPasswordFailure::AuthKeyUnregistered;
	} else if (errorType == u"SRP_ID_INVALID"_q) {
		return SigninPasswordFailure::SrpIdInvalid;
	}
	return SigninPasswordFailure::Other;
}

} // namespace details
} // namespace Intro
