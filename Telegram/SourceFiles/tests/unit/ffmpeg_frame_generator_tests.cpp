/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "ffmpeg/ffmpeg_frame_generator.h"

TEST_CASE(InvalidContainersLeaveFrameGeneratorInert) {
	const auto check = [](const QByteArray &bytes) {
		auto generator = FFmpeg::FrameGenerator(bytes);
		CHECK_EQ(generator.count(), 0);
		CHECK(generator.rate() == 0.);
		const auto next = generator.renderNext({}, QSize(16, 16));
		CHECK(next.image.isNull());
		const auto current = generator.renderCurrent({}, QSize(16, 16));
		CHECK(current.image.isNull());
		generator.jumpToStart();
	};
	check(QByteArray("not-a-webm-file!"));
	check(QByteArray());
}
