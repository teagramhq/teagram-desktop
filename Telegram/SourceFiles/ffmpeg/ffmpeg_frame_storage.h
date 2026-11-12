/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QSize>

class QImage;

namespace FFmpeg {

inline constexpr auto kPixelBytesSize = 4;
// See https://github.com/telegramdesktop/tdesktop/issues/7225
inline constexpr auto kAlignImageBy = 64;

[[nodiscard]] bool GoodStorageForFrame(const QImage &storage, QSize size,
									   int minBytesPerLine = 0);
[[nodiscard]] QImage CreateFrameStorage(QSize size, int minBytesPerLine = 0);

} // namespace FFmpeg
