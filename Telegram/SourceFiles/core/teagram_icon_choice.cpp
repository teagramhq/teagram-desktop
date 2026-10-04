/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_icon_choice.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

namespace Core {

QImage RenderTeagramIconImage(TeagramIconChoice choice) {
	const auto resource = TeagramIconSvgResource(choice);
	auto renderer = QSvgRenderer(QString::fromLatin1(
		resource.data(),
		static_cast<qsizetype>(resource.size())));
	if (!renderer.isValid()) {
		return {};
	}
	auto result = QImage(
		QSize(1024, 1024),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		auto painter = QPainter(&result);
		renderer.render(&painter, QRectF(0, 0, 1024, 1024));
	}
	result.setDevicePixelRatio(2.);
	return result;
}

} // namespace Core
