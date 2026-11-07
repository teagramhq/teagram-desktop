/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringView>

#include <optional>
#include <string_view>

namespace Intro::details {

struct TurnstileChallenge {
	QString siteKey;
	QString action;
};

enum class TurnstileMessageType {
	Token,
	Error,
};

struct TurnstileMessage {
	TurnstileMessageType type = TurnstileMessageType::Error;
	QString token;
};

enum class TurnstileMessageRejection {
	None,
	PayloadTooLarge,
	NoChallengePending,
	NavigationNotComplete,
	InvalidMainFrameUrl,
	InvalidJson,
	InvalidEnvelope,
	UnknownEvent,
	InvalidPayload,
	ActionMismatch,
	InvalidToken,
};

struct TurnstileMessageResult {
	std::optional<TurnstileMessage> message;
	TurnstileMessageRejection rejection = TurnstileMessageRejection::None;
};

enum class TurnstileNavigationRejection {
	None,
	NewWindow,
	UnexpectedNavigation,
};

[[nodiscard]] QString
TurnstileMessageRejectionText(TurnstileMessageRejection rejection);
[[nodiscard]] QString
TurnstileNavigationRejectionText(TurnstileNavigationRejection rejection);

struct TurnstileNavigationDecision {
	bool allowed = false;
	bool initialNavigation = false;
	TurnstileNavigationRejection rejection
		= TurnstileNavigationRejection::UnexpectedNavigation;
};

class TurnstileLinuxNavigationGuard final {
  public:
	void initialLoadCompleted();
	[[nodiscard]] bool mainFrameUrlUpdated(QStringView url,
										   QStringView challengeUrl);
	void finish();

  private:
	bool _initialLoadCompleted = false;
	bool _finished = false;
};

[[nodiscard]] std::optional<TurnstileChallenge>
ParseTurnstileChallenge(QStringView errorType);
[[nodiscard]] bool IsTurnstileChallengeError(QStringView errorType);
[[nodiscard]] QString TurnstileUrl(const TurnstileChallenge &challenge);
[[nodiscard]] bool IsTurnstileMainFrameUrl(QStringView url);
[[nodiscard]] TurnstileMessageResult
ParseTurnstileMessage(std::string_view bytes, QStringView pendingAction,
					  bool challengePending, bool navigationDone,
					  QStringView mainFrameUrl);
[[nodiscard]] TurnstileNavigationDecision
CheckTurnstileNavigation(QStringView url, QStringView initialUrl,
						 bool initialNavigationPending, bool newWindow,
						 bool allowLinuxSubframes);

} // namespace Intro::details
