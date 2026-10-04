/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_icon_choice.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

using namespace Core;

TEST_CASE(EveryTeagramIconRendersInsideTheMacIconTemplate) {
	CHECK_EQ(kTeagramIconChoiceCount, 12);
	for (auto index = 0; index != kTeagramIconChoiceCount; ++index) {
		const auto resource = kTeagramIconSvgResources[index];
		const auto path = QString::fromLatin1(
			resource.data(),
			static_cast<qsizetype>(resource.size()));
		auto renderer = QSvgRenderer(path);
		CHECK(renderer.isValid());

		auto image = QImage(
			QSize(1024, 1024),
			QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			auto painter = QPainter(&image);
			renderer.render(&painter);
		}
		CHECK_EQ(image.pixelColor(512, 512).alpha(), 255);
		CHECK_EQ(image.pixelColor(99, 512).alpha(), 0);
		CHECK_EQ(image.pixelColor(101, 512).alpha(), 255);
	}
}
