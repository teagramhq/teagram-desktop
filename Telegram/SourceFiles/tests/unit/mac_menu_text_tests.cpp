/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/mac/global_menu_mac.h"
#include "tests/unit/unit_test.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTranslator>

#include <cstring>

namespace {

class MenuTranslation final : public QTranslator {
public:
	QString translate(
		const char *context,
		const char *sourceText,
		const char *,
		int) const override {
		if (std::strcmp(context, "QMenuBar") == 0
			&& std::strcmp(sourceText, "About %1") == 0) {
			return u"À propos de %1"_q;
		}
		return {};
	}
};

} // namespace

TEST_CASE(MacAboutMenuUsesLocalizedApplicationName) {
	const auto previousName = QCoreApplication::applicationName();
	auto translation = MenuTranslation();
	QCoreApplication::setApplicationName(u"Teagram"_q);
	CHECK(QCoreApplication::installTranslator(&translation));
	CHECK_EQ(Platform::MacAboutMenuText(), u"À propos de Teagram"_q);
	QCoreApplication::removeTranslator(&translation);
	CHECK_EQ(Platform::MacAboutMenuText(), u"About Teagram"_q);
	QCoreApplication::setApplicationName(previousName);
}
