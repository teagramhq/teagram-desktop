/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Intro::details {

enum class SignupFailureMessage {
	RegistrationClosed,
	UsernameTaken,
	UsernameUnavailable,
	UsernameInvalid,
	NameInvalid,
	InvitationUnavailable,
	SessionCannotContinue,
	CodeTicketExpired,
	FloodWait,
	ServerError,
};

enum class SignupFailureDestination {
	Username,
	Name,
	Password,
};

enum class SignupFailureAction {
	ShowError,
	ReturnToInput,
	ReissueCode,
};

struct SignupFailureDecision {
	SignupFailureMessage message = SignupFailureMessage::ServerError;
	SignupFailureDestination destination = SignupFailureDestination::Password;
	SignupFailureAction action = SignupFailureAction::ShowError;
	bool backAvailable = false;
	int floodWaitSeconds = 0;
};

class SignupCodeReissueState final {
  public:
	[[nodiscard]] bool begin(bool retried) {
		if (_reissuedOnce && !retried) {
			return false;
		} else if (!retried) {
			_reissuedOnce = true;
		}
		return true;
	}

	[[nodiscard]] bool wasReissuedOnce() const { return _reissuedOnce; }

	[[nodiscard]] bool handleChallengeError(bool retried) {
		if (retried) {
			_reissuedOnce = false;
			return false;
		}
		return true;
	}

	void challengeAborted() { _reissuedOnce = false; }

  private:
	bool _reissuedOnce = false;
};

[[nodiscard]] SignupFailureDecision ClassifySignupFailure(
	const QString &errorType,
	int errorCode,
	bool floodError,
	bool reissuedOnce);

} // namespace Intro::details
