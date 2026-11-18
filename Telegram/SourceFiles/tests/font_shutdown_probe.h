/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <pthread.h>

namespace Core::Tests {

void ArmFontShutdownProbe(pthread_t worker, int skippedPopulations);
void ReleaseFontShutdownProbe();
[[nodiscard]] bool IsFontShutdownProbeReleased();

} // namespace Core::Tests
