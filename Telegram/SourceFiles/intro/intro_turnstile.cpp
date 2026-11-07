/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_turnstile.h"

#include "base/basic_types.h"
#include "ui/webview_limits.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

namespace Intro::details {
namespace {

[[nodiscard]] bool IsValidSiteKey(QStringView siteKey) {
	if (siteKey.isEmpty() || siteKey.size() > 256) {
		return false;
	}
	for (const auto ch : siteKey) {
		const auto code = ch.unicode();
		if (!((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')
			  || (code >= '0' && code <= '9') || code == '_' || code == '-')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsTurnstileAction(QStringView action) {
	return (action == u"signin") || (action == u"signup");
}

[[nodiscard]] QUrl ParseUrl(QStringView value) {
	return QUrl(value.toString(), QUrl::StrictMode);
}

[[nodiscard]] bool IsHttpsHostOnDefaultPort(const QUrl &url, QStringView host) {
	return url.isValid()
		   && (url.scheme().compare(u"https"_q, Qt::CaseInsensitive) == 0)
		   && (url.host().compare(host, Qt::CaseInsensitive) == 0)
		   && (url.port(443) == 443) && url.userInfo().isEmpty()
		   && !url.authority().contains(u'@');
}

[[nodiscard]] bool IsAllowedLinuxSubframeUrl(QStringView value) {
	if (value == u"about:blank" || value == u"about:srcdoc") {
		return true;
	}
	return IsHttpsHostOnDefaultPort(ParseUrl(value),
									u"challenges.cloudflare.com");
}

[[nodiscard]] bool IsSameInitialUrl(QStringView candidate,
									QStringView expected) {
	const auto actualUrl = ParseUrl(candidate);
	const auto expectedUrl = ParseUrl(expected);
	return IsTurnstileMainFrameUrl(candidate)
		   && IsTurnstileMainFrameUrl(expected)
		   && (actualUrl.path() == expectedUrl.path())
		   && (actualUrl.query(QUrl::FullyEncoded)
			   == expectedUrl.query(QUrl::FullyEncoded))
		   && (actualUrl.fragment(QUrl::FullyEncoded)
			   == expectedUrl.fragment(QUrl::FullyEncoded));
}

[[nodiscard]] bool IsPrintableAscii(QStringView value) {
	if (value.isEmpty() || value.size() > 2048) {
		return false;
	}
	for (const auto ch : value) {
		const auto code = ch.unicode();
		if (code < 0x20 || code > 0x7E) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::optional<QJsonObject>
ParsePayloadObject(const QString &payload) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(payload.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	return document.object();
}

} // namespace

std::optional<TurnstileChallenge>
ParseTurnstileChallenge(QStringView errorType) {
	constexpr auto kPrefix = QStringView(u"RECAPTCHA_CHECK_");
	if (!errorType.startsWith(kPrefix)) {
		return std::nullopt;
	}
	const auto parameters = errorType.mid(kPrefix.size());
	const auto separator = parameters.indexOf(u"__");
	if (separator <= 0) {
		return std::nullopt;
	}
	const auto action = parameters.left(separator);
	const auto siteKey = parameters.mid(separator + 2);
	if (!IsTurnstileAction(action) || !IsValidSiteKey(siteKey)) {
		return std::nullopt;
	}
	return TurnstileChallenge{
		siteKey.toString(),
		action.toString(),
	};
}

bool IsTurnstileChallengeError(QStringView errorType) {
	return errorType.startsWith(QStringView(u"RECAPTCHA_CHECK_"));
}

QString TurnstileUrl(const TurnstileChallenge &challenge) {
	const auto siteKey
		= QString::fromLatin1(QUrl::toPercentEncoding(challenge.siteKey));
	const auto action
		= QString::fromLatin1(QUrl::toPercentEncoding(challenge.action));
	return u"https://web.teagram.io/turnstile.html#sitekey=%1&action=%2"_q.arg(
		siteKey, action);
}

bool IsTurnstileMainFrameUrl(QStringView value) {
	const auto url = ParseUrl(value);
	return IsHttpsHostOnDefaultPort(url, u"web.teagram.io")
		   && (url.path() == u"/turnstile.html"_q);
}

void TurnstileLinuxNavigationGuard::initialLoadCompleted() {
	if (!_finished) {
		_initialLoadCompleted = true;
	}
}

bool TurnstileLinuxNavigationGuard::mainFrameUrlUpdated(
	QStringView url, QStringView challengeUrl) {
	if (!_initialLoadCompleted || _finished
		|| IsSameInitialUrl(url, challengeUrl)) {
		return false;
	}
	_finished = true;
	return true;
}

void TurnstileLinuxNavigationGuard::finish() { _finished = true; }

void DeferTurnstileCallback(Fn<void()> callback) {
	QTimer::singleShot(0, QCoreApplication::instance(), std::move(callback));
}

TurnstileMessageResult ParseTurnstileMessage(std::string_view bytes,
											 QStringView pendingAction,
											 bool challengePending,
											 bool navigationDone,
											 QStringView mainFrameUrl) {
	const auto reject = [](TurnstileMessageRejection reason) {
		return TurnstileMessageResult{.rejection = reason};
	};
	if (bytes.size() > Ui::kMaxNativeMessageBytes) {
		return reject(TurnstileMessageRejection::PayloadTooLarge);
	} else if (!challengePending) {
		return reject(TurnstileMessageRejection::NoChallengePending);
	} else if (!navigationDone) {
		return reject(TurnstileMessageRejection::NavigationNotComplete);
	} else if (!IsTurnstileMainFrameUrl(mainFrameUrl)) {
		return reject(TurnstileMessageRejection::InvalidMainFrameUrl);
	}

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(
		QByteArray(bytes.data(), int(bytes.size())), &error);
	if (error.error != QJsonParseError::NoError) {
		return reject(TurnstileMessageRejection::InvalidJson);
	} else if (!document.isArray()) {
		return reject(TurnstileMessageRejection::InvalidEnvelope);
	}
	const auto envelope = document.array();
	if (envelope.size() != 2 || !envelope[0].isString()
		|| !envelope[1].isString()) {
		return reject(TurnstileMessageRejection::InvalidEnvelope);
	}
	const auto eventName = envelope[0].toString();
	const auto payload = envelope[1].toString();
	if (eventName == u"turnstile_error"_q) {
		return {
			.message = TurnstileMessage{
				.type = TurnstileMessageType::Error,
			},
		};
	} else if (eventName != u"turnstile_token"_q) {
		return reject(TurnstileMessageRejection::UnknownEvent);
	}

	const auto object = ParsePayloadObject(payload);
	if (!object || !object->value(u"token"_q).isString()
		|| !object->value(u"action"_q).isString()) {
		return reject(TurnstileMessageRejection::InvalidPayload);
	}
	const auto token = object->value(u"token"_q).toString();
	if (object->value(u"action"_q).toString() != pendingAction.toString()) {
		return reject(TurnstileMessageRejection::ActionMismatch);
	} else if (!IsPrintableAscii(token)) {
		return reject(TurnstileMessageRejection::InvalidToken);
	}
	return {
		.message = TurnstileMessage{
			.type = TurnstileMessageType::Token,
			.token = token,
		},
	};
}

TurnstileNavigationDecision
CheckTurnstileNavigation(QStringView url, QStringView initialUrl,
						 bool initialNavigationPending, bool newWindow,
						 bool allowLinuxSubframes) {
	if (newWindow) {
		return {
			.rejection = TurnstileNavigationRejection::NewWindow,
		};
	} else if (initialNavigationPending && IsSameInitialUrl(url, initialUrl)) {
		return {
			.allowed = true,
			.initialNavigation = true,
			.rejection = TurnstileNavigationRejection::None,
		};
	} else if (allowLinuxSubframes && IsAllowedLinuxSubframeUrl(url)) {
		return {
			.allowed = true,
			.rejection = TurnstileNavigationRejection::None,
		};
	}
	return {
		.rejection = TurnstileNavigationRejection::UnexpectedNavigation,
	};
}

QString TurnstileMessageRejectionText(TurnstileMessageRejection rejection) {
	switch (rejection) {
	case TurnstileMessageRejection::None:
		return u"none"_q;
	case TurnstileMessageRejection::PayloadTooLarge:
		return u"payload too large"_q;
	case TurnstileMessageRejection::NoChallengePending:
		return u"no challenge pending"_q;
	case TurnstileMessageRejection::NavigationNotComplete:
		return u"navigation incomplete"_q;
	case TurnstileMessageRejection::InvalidMainFrameUrl:
		return u"invalid main-frame URL"_q;
	case TurnstileMessageRejection::InvalidJson:
		return u"invalid JSON"_q;
	case TurnstileMessageRejection::InvalidEnvelope:
		return u"invalid envelope"_q;
	case TurnstileMessageRejection::UnknownEvent:
		return u"unknown event"_q;
	case TurnstileMessageRejection::InvalidPayload:
		return u"invalid event payload"_q;
	case TurnstileMessageRejection::ActionMismatch:
		return u"action mismatch"_q;
	case TurnstileMessageRejection::InvalidToken:
		return u"invalid token"_q;
	}
	Unexpected("Unknown Turnstile message rejection.");
}

QString
TurnstileNavigationRejectionText(TurnstileNavigationRejection rejection) {
	switch (rejection) {
	case TurnstileNavigationRejection::None:
		return u"none"_q;
	case TurnstileNavigationRejection::NewWindow:
		return u"new-window request"_q;
	case TurnstileNavigationRejection::UnexpectedNavigation:
		return u"unexpected navigation"_q;
	}
	Unexpected("Unknown Turnstile navigation rejection.");
}

} // namespace Intro::details
