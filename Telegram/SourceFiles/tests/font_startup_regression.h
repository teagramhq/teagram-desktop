/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QtGlobal>

int RunFontFallbackRegression();

#ifdef Q_OS_MAC
#include <functional>
void RunEmojiCacheShutdownRegression(std::function<void(bool)> done);
void PauseEmojiCacheForShutdownRegression();
void EmojiCacheShutdownApplicationDestroying();
#endif // Q_OS_MAC
