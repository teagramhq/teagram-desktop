/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "storage/upload_limits.h"

TEST_CASE(ServerUploadLimitRejectsLargerFiles) {
	CHECK(Storage::details::ExceedsUploadMaxBytes(500'001, 500'000));
}

TEST_CASE(ServerUploadLimitAllowsSmallerFiles) {
	CHECK(!Storage::details::ExceedsUploadMaxBytes(499'999, 500'000));
}

TEST_CASE(ServerUploadLimitAllowsFilesWhenConfigIsAbsent) {
	CHECK(!Storage::details::ExceedsUploadMaxBytes(900'000'000, std::nullopt));
}

TEST_CASE(FilePartTooBigErrorIsRecognized) {
	CHECK(Storage::details::IsFilePartTooBigError(u"FILE_PART_TOO_BIG"_q));
}
