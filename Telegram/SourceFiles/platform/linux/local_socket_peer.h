/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <sys/types.h>

namespace Platform::Linux {

[[nodiscard]] inline bool IsLocalSocketPeerUidAllowed(uid_t peerUid,
													  uid_t effectiveUid) {
	return peerUid == effectiveUid;
}

} // namespace Platform::Linux
