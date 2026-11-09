/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "ffmpeg/ffmpeg_frame_storage.h"

#include <QtGui/QImage>

TEST_CASE(FrameStorageHonorsMinimumDestinationPitch) {
	const auto size = QSize(16, 2);
	const auto ordinary = FFmpeg::CreateFrameStorage(size);
	CHECK(!ordinary.isNull());
	CHECK(ordinary.bytesPerLine() < 128);
	CHECK(!FFmpeg::GoodStorageForFrame(ordinary, size, 128));

	const auto expanded = FFmpeg::CreateFrameStorage(size, 128);
	CHECK(!expanded.isNull());
	CHECK_EQ(expanded.bytesPerLine(), 128);
	CHECK(FFmpeg::GoodStorageForFrame(expanded, size, 128));
}
