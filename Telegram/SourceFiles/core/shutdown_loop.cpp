/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/shutdown_loop.h"

#include <QtCore/QEventLoop>

namespace Core {

void ShutdownLoop::run(Fn<bool()> pending) {
	auto loop = QEventLoop();
	_loop = &loop;
	while (pending()) {
		loop.exec(QEventLoop::ExcludeUserInputEvents);
	}
	_loop = nullptr;
}

void ShutdownLoop::quit() {
	if (_loop) {
		_loop->quit();
	}
}

} // namespace Core
