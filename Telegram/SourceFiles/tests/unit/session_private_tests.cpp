/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "mtproto/session_private.h"

namespace {

using namespace MTP;
using namespace MTP::details;

TEST_CASE(AuthenticatedResponseCompletesReconnectWithoutSaltChange) {
	CHECK(ShouldMarkConnectedAfterAuthenticatedResponse(
		ConnectingState,
		false));
}

TEST_CASE(BadTimeAuthenticatedResponseDoesNotCompleteReconnect) {
	CHECK(!ShouldMarkConnectedAfterAuthenticatedResponse(
		ConnectingState,
		true));
}

TEST_CASE(AuthenticatedResponseDoesNotResetAlreadyConnectedSession) {
	CHECK(!ShouldMarkConnectedAfterAuthenticatedResponse(
		ConnectedState,
		false));
}

} // namespace
