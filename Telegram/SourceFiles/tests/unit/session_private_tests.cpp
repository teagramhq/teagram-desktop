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
	const auto transition
		= ProcessAuthenticatedResponse(ConnectingState, 42, 42, false);
	CHECK_EQ(transition.sessionSalt, uint64(42));
	CHECK(transition.shouldMarkConnected);
}

TEST_CASE(BadTimeAuthenticatedResponseDoesNotCompleteReconnect) {
	const auto transition
		= ProcessAuthenticatedResponse(ConnectingState, 42, 84, true);
	CHECK_EQ(transition.sessionSalt, uint64(42));
	CHECK(!transition.shouldMarkConnected);
}

TEST_CASE(AuthenticatedResponseDoesNotResetAlreadyConnectedSession) {
	const auto transition
		= ProcessAuthenticatedResponse(ConnectedState, 42, 84, false);
	CHECK_EQ(transition.sessionSalt, uint64(84));
	CHECK(!transition.shouldMarkConnected);
}

} // namespace
