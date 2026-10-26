/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "data/data_msg_id.h"

namespace Storage::details {

template <typename WaitingCoverMap, typename OnFailed>
void FailVideoCover(
		FullMsgId coverId,
		WaitingCoverMap &videoWaitingCover,
		base::flat_map<FullMsgId, FullMsgId> &videoIdToCoverId,
		OnFailed &&onFailed) {
	auto video = videoWaitingCover.take(coverId);
	if (video) {
		videoIdToCoverId.remove(video->fullId);
		onFailed(*video);
	}
}

} // namespace Storage::details
