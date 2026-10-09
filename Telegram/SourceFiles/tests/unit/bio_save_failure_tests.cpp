/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_bio_save_failure.h"

TEST_CASE(AboutNotSupportedBioFailureRestoresAndCancelsPendingSave) {
	const auto storedBio = u"stored bio"_q;
	auto editorBio = u"refused draft"_q;
	auto requestedText = editorBio;
	auto debounceGeneration = 7;
	const auto scheduledGeneration = debounceGeneration;
	auto rpcCount = 1;
	auto toasts = 0;
	auto recovered = false;
	const auto recover = [&] {
		recovered = true;
		Api::CancelBioSaveDebounce(debounceGeneration);
		editorBio = storedBio;
		++toasts;
		return true;
	};

	const auto transition = Api::ResolveBioSaveFailure(
		u"ABOUT_NOT_SUPPORTED"_q,
		requestedText,
		recover);

	CHECK(transition.action
		== Api::BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry);
	CHECK(!transition.showFallbackToast);
	CHECK(requestedText.isEmpty());
	CHECK(recovered);
	CHECK_EQ(editorBio, storedBio);
	CHECK_EQ(storedBio, u"stored bio"_q);
	CHECK_EQ(toasts, 1);
	CHECK(!Api::BioSaveDebounceIsCurrent(
		debounceGeneration,
		scheduledGeneration));
	if (Api::BioSaveDebounceIsCurrent(
			debounceGeneration,
			scheduledGeneration)) {
		++rpcCount;
	}
	if (Api::BioSaveNeedsCloseRetry(debounceGeneration)) {
		++rpcCount;
	}
	CHECK_EQ(rpcCount, 1);
}

TEST_CASE(OtherBioSaveFailuresKeepTheDraftWithoutRecovery) {
	const auto storedBio = u"stored bio"_q;
	auto editorBio = u"draft"_q;
	auto requestedText = editorBio;
	auto recovered = false;
	const auto recover = [&] {
		recovered = true;
		return true;
	};

	const auto transition = Api::ResolveBioSaveFailure(
		u"ABOUT_INVALID"_q,
		requestedText,
		recover);

	CHECK(transition.action == Api::BioSaveFailureAction::KeepExistingBehavior);
	CHECK(!transition.showFallbackToast);
	CHECK_EQ(requestedText, editorBio);
	CHECK_EQ(editorBio, u"draft"_q);
	CHECK_EQ(storedBio, u"stored bio"_q);
	CHECK(!recovered);
}
