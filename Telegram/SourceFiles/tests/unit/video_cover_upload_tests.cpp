/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "base/flat_map.h"
#include "data/data_msg_id.h"
#include "storage/file_upload.h"
#include "storage/video_cover_upload.h"

TEST_CASE(FailedVideoCoverNotifiesParentAndClearsState) {
	const auto videoId = FullMsgId(PeerId(1), 1);
	const auto coverId = FullMsgId(PeerId(1), 2);
	auto waitingCover = base::flat_map<FullMsgId, Storage::UploadedMedia>();
	waitingCover.emplace(coverId, Storage::UploadedMedia{
									  .fullId = videoId,
								  });
	auto videoIdToCoverId = base::flat_map<FullMsgId, FullMsgId>();
	videoIdToCoverId.emplace(videoId, coverId);

	auto failedVideoId = FullMsgId();
	auto failedCount = 0;
	Storage::details::FailVideoCover(coverId, waitingCover, videoIdToCoverId,
									 [&](const Storage::UploadedMedia &video) {
										 failedVideoId = video.fullId;
										 ++failedCount;
									 });

	CHECK(failedCount == 1);
	CHECK(failedVideoId == videoId);
	CHECK(waitingCover.empty());
	CHECK(videoIdToCoverId.empty());
}
