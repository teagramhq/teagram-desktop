/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/launcher.h"

TEST_CASE(DebugModeEnabledByDefault) {
	CHECK(Core::details::DebugModeEnabled(std::nullopt, false));
}

TEST_CASE(PersistedOffSettingDisablesDebugMode) {
	CHECK(!Core::details::DebugModeEnabled(QByteArray("0"), false));
}

TEST_CASE(PersistedOnSettingEnablesDebugMode) {
	CHECK(Core::details::DebugModeEnabled(QByteArray("1"), false));
}

TEST_CASE(CommandLineOptionForcesDebugMode) {
	CHECK(Core::details::DebugModeEnabled(QByteArray("0"), true));
}
