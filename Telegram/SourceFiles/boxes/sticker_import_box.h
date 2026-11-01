/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

[[nodiscard]] bool StickerImportEnabled(not_null<Main::Session*> session);
void ShowStickerImportBox(
	not_null<Window::SessionController*> controller);
