/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringView>

#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace Intro::details {

enum class TurnstileAction {
	SignIn,
	SignUp,
};

class TurnstileChallenge final {
  public:
	[[nodiscard]] const QString &siteKey() const { return _siteKey; }
	[[nodiscard]] TurnstileAction action() const { return _action; }

  private:
	friend struct TurnstileChallengeParser;

	TurnstileChallenge(QString siteKey, TurnstileAction action)
		: _siteKey(std::move(siteKey)), _action(action) {}

	QString _siteKey;
	TurnstileAction _action;
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

using TurnstileMessageResult
	= std::variant<TurnstileMessage, TurnstileMessageRejection>;

enum class TurnstileNavigationRejection {
	NewWindow,
	UnexpectedNavigation,
};

[[nodiscard]] QString
TurnstileMessageRejectionText(TurnstileMessageRejection rejection);
[[nodiscard]] QString
TurnstileNavigationRejectionText(TurnstileNavigationRejection rejection);

struct TurnstileNavigationDecision {
	bool initialNavigation = false;
	std::optional<TurnstileNavigationRejection> rejection;
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

void DeferTurnstileCallback(Fn<void()> beforeCallback, Fn<void()> callback);

[[nodiscard]] std::optional<TurnstileChallenge>
ParseTurnstileChallenge(QStringView errorType);
[[nodiscard]] bool IsTurnstileChallengeError(QStringView errorType);
[[nodiscard]] QString TurnstileUrl(const TurnstileChallenge &challenge);
[[nodiscard]] bool IsTurnstileMainFrameUrl(QStringView url);
[[nodiscard]] TurnstileMessageResult
ParseTurnstileMessage(std::string_view bytes, TurnstileAction pendingAction,
					  bool challengePending, bool navigationDone,
					  QStringView mainFrameUrl);
[[nodiscard]] TurnstileNavigationDecision
CheckTurnstileNavigation(QStringView url, QStringView initialUrl,
						 bool initialNavigationPending, bool newWindow,
						 bool allowLinuxSubframes);

} // namespace Intro::details
