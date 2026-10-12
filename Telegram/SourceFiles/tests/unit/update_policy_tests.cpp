/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/teagram_update_state.h"
#include "core/update_policy.h"

#include <limits>
#include <string_view>
#include <utility>

using namespace Core;

struct TeagramHighWaterSettings {
	QByteArray encoded;

	template <typename Type, typename Other = Type>
	Type readPref(std::string_view, Other &&fallback = Type()) const {
		return encoded.isEmpty()
			? std::forward<Other>(fallback)
			: Type(encoded);
	}
	template <typename Type, typename Other>
	void writePref(std::string_view, Other &&value) {
		encoded = std::forward<Other>(value);
	}
};

TEST_CASE(EveryUpdaterEntryPointIsDenied) {
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Automatic));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Periodic));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Settings));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::CrashWindowRetry));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::CrashWindowGetApp));
	CHECK(!UpdateNetworkAllowed(UpdateEntryPoint::Manual));
}

TEST_CASE(ServerCannotChooseAnUpdateOrigin) {
	CHECK(!AcceptServerAutoupdatePrefix(QString()));
	CHECK(!AcceptServerAutoupdatePrefix(u"https://td.telegram.org"_q));
	CHECK(!AcceptServerAutoupdatePrefix(u"https://updates.attacker.test"_q));
}

TEST_CASE(TeagramUpdateEligibilityHonorsHighWaterMark) {
	using Channel = TeagramUpdateChannel;
	CHECK(!TeagramUpdateBuildIsEligible(
		101, 102, Channel::Dev, 102, Channel::Main));
	CHECK(!TeagramUpdateBuildIsEligible(
		102, 102, Channel::Main, 103, Channel::Dev));
	CHECK(TeagramUpdateBuildIsEligible(
		102, 102, Channel::Main, 103, Channel::Main));
}

TEST_CASE(TeagramUpdateHighWaterPersistsFullUnsignedBuild) {
	auto settings = TeagramHighWaterSettings();
	const auto maximum = std::numeric_limits<quint64>::max();
	WriteTeagramUpdateHighWater(settings, maximum);
	CHECK(ReadTeagramUpdateHighWater(settings) == maximum);

	settings.encoded = QByteArrayLiteral("01");
	CHECK(ReadTeagramUpdateHighWater(settings) == 0);
}
