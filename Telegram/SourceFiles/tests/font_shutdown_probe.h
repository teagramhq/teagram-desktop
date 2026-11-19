/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <pthread.h>

namespace Core::Tests {

__attribute__((visibility("default"))) void
ArmFontShutdownProbe(pthread_t worker, int skippedPopulations);
__attribute__((visibility("default"))) void ReleaseFontShutdownProbe();
[[nodiscard]] __attribute__((visibility("default"))) bool
IsFontShutdownProbeReleased();

} // namespace Core::Tests
