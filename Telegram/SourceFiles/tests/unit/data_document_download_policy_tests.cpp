/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_document_download_policy.h"

TEST_CASE(DocumentDownloadPolicyRejectsNegativeSizes) {
	using Decision = Data::details::DocumentDownloadDecision;
	CHECK(Data::details::DecideDocumentDownload(-1, false, 1024)
		  == Decision::Fail);
}

TEST_CASE(DocumentDownloadPolicyKeepsCacheAndStreamPaths) {
	using Decision = Data::details::DocumentDownloadDecision;
	CHECK(Data::details::DecideDocumentDownload(1025, false, 1024)
		  == Decision::Cache);
	CHECK(Data::details::DecideDocumentDownload(1025, true, 1024)
		  == Decision::Stream);
	CHECK(Data::details::DecideDocumentDownload(1024, false, 1024)
		  == Decision::Stream);
}
