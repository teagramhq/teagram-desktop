/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

namespace Intro::details {

struct TurnstileChallenge;

struct TurnstilePanelResult {
	Fn<void()> closeSilently;
	QString error;
};

[[nodiscard]] TurnstilePanelResult
OpenTurnstilePanel(TurnstileChallenge challenge, Fn<void(QString)> token,
				   Fn<void()> failed);

} // namespace Intro::details
