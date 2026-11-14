/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_auth_key.h"

#include <functional>

namespace MTP {

struct UnitEndpointRefreshProofResult {
	bool requestWasPausedBeforeProof = false;
	bool proofAccepted = false;
	bool requestResumed = false;
	bool requestCompleted = false;
	bool refreshCommitted = false;
};

using UnitEndpointRefreshCommitForTest
	= std::function<bool(bool, const std::function<void()> &)>;

[[nodiscard]] UnitEndpointRefreshProofResult
UnitVerifyEndpointAndResumeRequestsForTest(
	const AuthKeyPtr &sessionKey, const AuthKeyPtr &responseKey,
	const UnitEndpointRefreshCommitForTest &commit);

} // namespace MTP
