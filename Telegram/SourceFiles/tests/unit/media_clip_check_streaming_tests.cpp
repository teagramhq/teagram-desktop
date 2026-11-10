/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/file_location.h"
#include "media/clip/media_clip_check_streaming.h"

#include <QtCore/QByteArray>

TEST_CASE(CheckStreamingSupportRejectsInvalidAtomLengths) {
	const auto location = Core::FileLocation();
	const auto zeroLength = QByteArray::fromHex("0000000066726565");
	const auto largerThanBuffer = QByteArray::fromHex("0000001066726565");

	CHECK(!Media::Clip::CheckStreamingSupport(location, zeroLength));
	CHECK(!Media::Clip::CheckStreamingSupport(location, largerThanBuffer));
}
