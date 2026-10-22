/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QCoreApplication>

namespace Ui {
struct MarkdownEnabledState;
} // namespace Ui

namespace Platform {

[[nodiscard]] inline QString MacAboutMenuText() {
	return QCoreApplication::translate("QMenuBar", "About %1")
		.arg(QCoreApplication::applicationName());
}

void CreateGlobalMenu();
void DestroyGlobalMenu();
void RequestUpdateGlobalMenu();

[[nodiscard]] rpl::producer<Ui::MarkdownEnabledState> GlobalMenuMarkdownState();

} // namespace Platform
