/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_bio_save_failure.h"

TEST_CASE(AboutNotSupportedBioFailureRestoresWithoutWriteOrRetry) {
	CHECK(Api::ClassifyBioSaveFailure(u"ABOUT_NOT_SUPPORTED"_q)
		== Api::BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry);
}

TEST_CASE(OtherBioSaveFailuresKeepTheirExistingBehavior) {
	CHECK(Api::ClassifyBioSaveFailure(u"ABOUT_INVALID"_q)
		== Api::BioSaveFailureAction::KeepExistingBehavior);
}
