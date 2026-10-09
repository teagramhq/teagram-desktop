/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_bio_save_failure.h"

namespace {

class ControlledBioApi final {
public:
	void save(Api::BioSaveRequestState &state, const QString &text) {
		++_requestCount;
		_generation = state.begin(text);
	}

	[[nodiscard]] auto fail(
			Api::BioSaveRequestState &state,
			Api::BioSaveEditorState &editor,
			const QString &errorType) {
		return state.failed(_generation, errorType, [&] {
			return editor.aboutNotSupported();
		});
	}

	[[nodiscard]] bool succeed(
			Api::BioSaveRequestState &state,
			Api::BioSaveEditorState &editor) {
		return state.succeeded(_generation, [&](const QString &text) {
			editor.setStoredBio(text);
		});
	}

	[[nodiscard]] int requestCount() const {
		return _requestCount;
	}

private:
	Api::BioSaveRequestState::Generation _generation = 0;
	int _requestCount = 0;
};

} // namespace

TEST_CASE(AboutNotSupportedBioFailureRestoresAndCancelsPendingSave) {
	const auto savedBio = u"stored bio"_q;
	auto editor = Api::BioSaveEditorState(savedBio);
	editor.setEditorBio(u"refused draft"_q);
	const auto scheduled = editor.scheduleDebounce();
	auto request = Api::BioSaveRequestState();
	auto api = ControlledBioApi();

	api.save(request, editor.editorBio());
	const auto transition = api.fail(
		request,
		editor,
		u"ABOUT_NOT_SUPPORTED"_q);

	CHECK(transition.has_value());
	CHECK(transition->action
		== Api::BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry);
	CHECK(!transition->showFallbackToast);
	CHECK(request.requestedText().isEmpty());
	CHECK_EQ(editor.storedBio(), savedBio);
	CHECK_EQ(editor.editorBio(), savedBio);
	CHECK(!editor.runDebounce(scheduled, [&] {
		api.save(request, editor.editorBio());
	}));
	CHECK(!editor.saveOnClose([&] {
		api.save(request, editor.editorBio());
	}));
	CHECK_EQ(api.requestCount(), 1);
}

TEST_CASE(OtherBioSaveFailuresKeepTheDraftAndPendingSave) {
	const auto savedBio = u"stored bio"_q;
	auto editor = Api::BioSaveEditorState(savedBio);
	editor.setEditorBio(u"draft"_q);
	const auto scheduled = editor.scheduleDebounce();
	auto request = Api::BioSaveRequestState();
	auto api = ControlledBioApi();

	api.save(request, editor.editorBio());
	const auto transition = api.fail(
		request,
		editor,
		u"ABOUT_INVALID"_q);

	CHECK(transition.has_value());
	CHECK(transition->action == Api::BioSaveFailureAction::KeepExistingBehavior);
	CHECK(!transition->showFallbackToast);
	CHECK_EQ(request.requestedText(), u"draft"_q);
	CHECK_EQ(editor.storedBio(), savedBio);
	CHECK_EQ(editor.editorBio(), u"draft"_q);
	CHECK(editor.debounceIsCurrent(scheduled));
	CHECK(editor.needsCloseRetry());
	CHECK_EQ(api.requestCount(), 1);
}

TEST_CASE(SuccessfulBioSaveUpdatesTheStoredValue) {
	auto editor = Api::BioSaveEditorState(u"stored bio"_q);
	editor.setEditorBio(u"accepted draft"_q);
	auto request = Api::BioSaveRequestState();
	auto api = ControlledBioApi();

	api.save(request, editor.editorBio());
	CHECK(api.succeed(request, editor));
	CHECK_EQ(editor.storedBio(), u"accepted draft"_q);
	CHECK(request.requestedText().isEmpty());
	CHECK_EQ(api.requestCount(), 1);
}
