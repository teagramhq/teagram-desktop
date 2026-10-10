/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common.h"

namespace Window {
class SessionController;
} // namespace Window

class History;

namespace Settings {

[[nodiscard]] Type FoldersId();

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
[[nodiscard]] bool RunFoldersCrudRegressionForTest(
	not_null<Window::SessionController *> controller,
	not_null<Window::SessionController *> other, not_null<History *> history);
#endif

} // namespace Settings
