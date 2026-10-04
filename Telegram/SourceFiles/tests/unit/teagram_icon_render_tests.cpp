/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_icon_choice.h"

#include <array>
#include <string_view>

#include <QtGui/QColor>
#include <QtGui/QImage>

using namespace Core;

namespace {

struct TeagramIconPreferences {
	int savedChoice = -1;

	template <typename Type>
	Type readPref(std::string_view key, Type fallback = Type{}) const {
		return (key == kTeagramIconChoicePreference)
			? static_cast<Type>(savedChoice)
			: fallback;
	}
};

} // namespace

TEST_CASE(TeagramIconPickerOrdersMugsBeforeTs) {
	const auto expected = std::array{
		TeagramIconChoice::MugSignal,
		TeagramIconChoice::MugGreen,
		TeagramIconChoice::MugSky,
		TeagramIconChoice::MugCrimson,
		TeagramIconChoice::MugBrown,
		TeagramIconChoice::TPrimary,
		TeagramIconChoice::TNavy,
		TeagramIconChoice::TNight,
		TeagramIconChoice::TPaper,
		TeagramIconChoice::TCrimson,
		TeagramIconChoice::TBrown,
	};
	CHECK_EQ(static_cast<int>(kTeagramIconPickerOrder.size()), 11);
	for (auto index = 0; index != static_cast<int>(expected.size()); ++index) {
		CHECK_EQ(
			static_cast<int>(kTeagramIconPickerOrder[index]),
			static_cast<int>(expected[index]));
	}
}

TEST_CASE(PreviouslySavedMugTeaLoadsAsMugSignal) {
	auto settings = TeagramIconPreferences{ .savedChoice = 1 };
	CHECK_EQ(
		static_cast<int>(ReadTeagramIconChoice(settings)),
		static_cast<int>(TeagramIconChoice::MugSignal));
}

TEST_CASE(EveryTeagramIconRendersInsideTheMacIconTemplate) {
	CHECK_EQ(kTeagramIconChoiceCount, 12);
	for (auto index = 0; index != kTeagramIconChoiceCount; ++index) {
		const auto choice = static_cast<TeagramIconChoice>(index);
		const auto image = RenderTeagramIconImage(choice);
		CHECK(!image.isNull());
		CHECK(image.size() == QSize(1024, 1024));
		CHECK(image.devicePixelRatioF() == 2.);
		CHECK_EQ(image.pixelColor(512, 512).alpha(), 255);
		CHECK_EQ(image.pixelColor(99, 512).alpha(), 0);
		CHECK_EQ(image.pixelColor(101, 512).alpha(), 255);

		// The outer signal arc peaks at SVG (494, 202), mapping to pixel
		// (497, 262) in the macOS icon template.
		auto expectedArcColor = QColor();
		auto expectedTColor = QColor();
		switch (choice) {
		case TeagramIconChoice::MugSignal:
		case TeagramIconChoice::MugTea:
		case TeagramIconChoice::MugCrimson:
		case TeagramIconChoice::MugBrown:
			expectedArcColor = QColor(Qt::white);
			break;
		case TeagramIconChoice::MugGreen:
			expectedArcColor = QColor(31, 167, 116);
			break;
		case TeagramIconChoice::MugSky:
			expectedArcColor = QColor(46, 139, 192);
			break;
		case TeagramIconChoice::TPrimary:
		case TeagramIconChoice::TCrimson:
		case TeagramIconChoice::TBrown:
			expectedTColor = QColor(Qt::white);
			break;
		case TeagramIconChoice::TNavy:
			expectedTColor = QColor(125, 211, 252);
			break;
		case TeagramIconChoice::TNight:
			expectedTColor = QColor(52, 211, 153);
			break;
		case TeagramIconChoice::TPaper:
			expectedTColor = QColor(15, 122, 87);
			break;
		}
		if (expectedArcColor.isValid()) {
			for (const auto y : std::array{ 262, 310, 359 }) {
				CHECK(image.pixelColor(497, y) == expectedArcColor);
			}
		} else {
			// T variants leave the signal arc area as smooth tile gradient.
			const auto arcPixel = image.pixelColor(497, 262);
			const auto left = image.pixelColor(300, 262);
			const auto right = image.pixelColor(700, 262);
			const auto IsBetween = [](int value, int first, int second) {
				return (value >= qMin(first, second) - 2)
					&& (value <= qMax(first, second) + 2);
			};
			CHECK(IsBetween(arcPixel.red(), left.red(), right.red()));
			CHECK(IsBetween(arcPixel.green(), left.green(), right.green()));
			CHECK(IsBetween(arcPixel.blue(), left.blue(), right.blue()));
			CHECK(image.pixelColor(497, 360) == expectedTColor);
		}
	}
}
