/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/font_startup_regression.h"

#include "base/debug_log.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QFont>
#include <QtGui/QRawFont>
#include <QtGui/QTextLayout>

namespace {

QString FontFamilyForCharacter(QString text, uint character) {
	auto layout = QTextLayout(text, QFont(u"Open Sans"_q, 16));
	layout.beginLayout();
	auto line = layout.createLine();
	if (!line.isValid()) {
		return QString();
	}
	line.setLineWidth(1024);
	layout.endLayout();
	for (const auto &run : layout.glyphRuns()) {
		const auto &font = run.rawFont();
		if (font.supportsCharacter(character)) {
			return font.familyName();
		}
	}
	return QString();
}

} // namespace

int RunFontFallbackRegression() {
	auto timer = QElapsedTimer();
	timer.start();
	const auto emoji = FontFamilyForCharacter(u"😀"_q, 0x1F600);
	const auto arabic = FontFamilyForCharacter(u"ش"_q, 0x0634);
	const auto wallNs = timer.nsecsElapsed();
	const auto emojiFamily = emoji.startsWith(u"."_q)
		? emoji.mid(1)
		: emoji;
	const auto emojiFallback
		= (emojiFamily == u"Apple Color Emoji"_q)
		|| (emojiFamily == u"Apple Color Emoji UI"_q);
	const auto passed
		= emojiFallback && (arabic == u"Vazirmatn UI NL"_q);
	LOG(("Font fallback regression: emoji=%1 arabic=%2 wall_ns=%3 "
		 "result=%4")
			.arg(emoji, arabic)
			.arg(wallNs)
			.arg(passed ? u"PASS"_q : u"FAIL"_q));
	return passed ? 0 : 1;
}
