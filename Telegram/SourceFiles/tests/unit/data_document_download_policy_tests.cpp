/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_document_download_policy.h"

TEST_CASE(NegativeDocumentDownloadSizeIsRejected) {
	CHECK(!Data::details::IsValidDocumentDownloadSize(-1));
	CHECK(Data::details::IsValidDocumentDownloadSize(0));
	CHECK(Data::details::IsValidDocumentDownloadSize(1024));
}
