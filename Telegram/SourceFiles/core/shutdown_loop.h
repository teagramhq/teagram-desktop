/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

class QEventLoop;

namespace Core {

class ShutdownLoop final {
  public:
	void run(Fn<bool()> pending);
	void quit();

  private:
	QEventLoop *_loop = nullptr;
};

} // namespace Core
