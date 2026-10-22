/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_compose_with_ai.h"
#include "main/session_feature_support.h"
#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/mtproto_dc_options.h"

#include <array>
#include <memory>

namespace {

const char kTestPublicKey[] = "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n\
5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n\
62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n\
+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n\
t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n\
5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n\
-----END RSA PUBLIC KEY-----";

MTP::DcOptions MakeCustomServerOptions() {
	auto options = MTP::DcOptions(MTP::Environment::Production);
	CHECK(options.setCustomServer({
		.dcId = 2,
		.ip = "10.4.1.7",
		.port = 8443,
		.key = std::make_shared<MTP::details::RSAPublicKey>(
			bytes::make_span(kTestPublicKey, sizeof(kTestPublicKey) - 1))
	}));
	return options;
}

struct Session {
	int sessionId = 0;
	int userId = 0;
	const MTP::DcOptions *options = nullptr;

	[[nodiscard]] bool aiComposeSupported() const {
		return Main::details::aiComposeSupported(*options);
	}
};

struct ApplyCounts {
	int historyWidgetDraftReads = 0;
	int composeControlsDraftReads = 0;
	int toneResolutions = 0;
	int composeRequests = 0;
};

enum class Handler {
	HistoryWidget,
	ComposeControls,
};

struct Step {
	const Session *owner = nullptr;
	bool supported = false;
};

} // namespace

TEST_CASE(AiComposeApplySessionSwitchIsolation) {
	auto stockOptions = MTP::DcOptions(MTP::Environment::Production);
	stockOptions.constructFromBuiltIn();
	auto customOptions = MakeCustomServerOptions();
	auto blockedOptions = MTP::DcOptions(MTP::Environment::Production);
	blockedOptions.constructBlocked();

	const auto stock = Session{ 1, 42, &stockOptions };
	const auto custom = Session{ 2, 42, &customOptions };
	const auto blocked = Session{ 3, 42, &blockedOptions };
	CHECK(stock.aiComposeSupported());
	CHECK(!custom.aiComposeSupported());
	CHECK(!blocked.aiComposeSupported());
	CHECK_EQ(stock.userId, custom.userId);
	CHECK_EQ(stock.userId, blocked.userId);
	CHECK(stock.sessionId != custom.sessionId);
	CHECK(stock.sessionId != blocked.sessionId);
	CHECK(custom.sessionId != blocked.sessionId);
	CHECK(&stock != &custom);
	CHECK(&stock != &blocked);
	CHECK(&custom != &blocked);

	const auto orders = std::array<std::array<Step, 3>, 4>{
		std::array<Step, 3>{
			Step{ &stock, true }, Step{ &custom, false }, Step{ &stock, true } },
		std::array<Step, 3>{
			Step{ &custom, false }, Step{ &stock, true }, Step{ &custom, false } },
		std::array<Step, 3>{
			Step{ &stock, true }, Step{ &blocked, false }, Step{ &stock, true } },
		std::array<Step, 3>{
			Step{ &blocked, false }, Step{ &stock, true }, Step{ &blocked, false } },
	};
	constexpr auto handlers = std::array{
		Handler::HistoryWidget,
		Handler::ComposeControls,
	};
	auto counts = ApplyCounts();
	for (const auto &order : orders) {
		for (const auto &step : order) {
			CHECK_EQ(step.owner->aiComposeSupported(), step.supported);
			for (const auto handler : handlers) {
				const auto before = counts;
				Api::details::RunIfAiComposeSupported(
					*step.owner,
					[&](const Session &authorizedOwner) {
						CHECK(&authorizedOwner == step.owner);
						if (handler == Handler::HistoryWidget) {
							++counts.historyWidgetDraftReads;
						} else {
							++counts.composeControlsDraftReads;
						}
						++counts.toneResolutions;
						++counts.composeRequests;
					});
				const auto expected = step.supported ? 1 : 0;
				CHECK_EQ(
					counts.historyWidgetDraftReads
						- before.historyWidgetDraftReads,
					handler == Handler::HistoryWidget ? expected : 0);
				CHECK_EQ(
					counts.composeControlsDraftReads
						- before.composeControlsDraftReads,
					handler == Handler::ComposeControls ? expected : 0);
				CHECK_EQ(counts.toneResolutions - before.toneResolutions, expected);
				CHECK_EQ(counts.composeRequests - before.composeRequests, expected);
			}
		}
	}
}
