/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/local_socket_peer.h"
#include "tests/unit/unit_test.h"

TEST_CASE(LinuxSingleInstanceRejectsPeerWithDifferentUid) {
	CHECK(!Platform::Linux::IsLocalSocketPeerUidAllowed(
		static_cast<uid_t>(1001), static_cast<uid_t>(1000)));
}

TEST_CASE(LinuxSingleInstanceAllowsPeerWithSameUid) {
	CHECK(Platform::Linux::IsLocalSocketPeerUidAllowed(
		static_cast<uid_t>(1000), static_cast<uid_t>(1000)));
}
