/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

namespace Api {

enum class BioSaveFailureAction {
	KeepExistingBehavior,
	RestoreStoredValueWithoutWriteOrRetry,
};

struct BioSaveFailureTransition {
	BioSaveFailureAction action = BioSaveFailureAction::KeepExistingBehavior;
	bool showFallbackToast = false;
};

[[nodiscard]] inline BioSaveFailureAction ClassifyBioSaveFailure(
		const QString &errorType) {
	return (errorType == u"ABOUT_NOT_SUPPORTED"_q)
		? BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry
		: BioSaveFailureAction::KeepExistingBehavior;
}

class BioSaveRequestState final {
public:
	using Generation = std::uint64_t;

	[[nodiscard]] Generation begin(const QString &text) {
		_requestedText = text;
		return ++_generation;
	}

	void cancel() {
		++_generation;
		_requestedText = QString();
	}

	[[nodiscard]] const QString &requestedText() const {
		return _requestedText;
	}

	[[nodiscard]] bool isCurrent(Generation generation) const {
		return (_generation == generation);
	}

	template <typename Apply>
	[[nodiscard]] bool succeeded(Generation generation, Apply apply) {
		if (!isCurrent(generation)) {
			return false;
		}
		apply(_requestedText);
		_requestedText = QString();
		return true;
	}

	template <typename Recovery>
	[[nodiscard]] std::optional<BioSaveFailureTransition> failed(
			Generation generation,
			const QString &errorType,
			Recovery recovery) {
		if (!isCurrent(generation)) {
			return std::nullopt;
		}
		const auto action = ClassifyBioSaveFailure(errorType);
		if (action == BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry) {
			_requestedText = QString();
			return BioSaveFailureTransition{ action, !recovery() };
		}
		return BioSaveFailureTransition{ action };
	}

private:
	Generation _generation = 0;
	QString _requestedText;

};

class BioSaveEditorState final {
public:
	explicit BioSaveEditorState(QString storedBio)
	: _storedBio(std::move(storedBio))
	, _editorBio(_storedBio) {
	}

	[[nodiscard]] const QString &storedBio() const {
		return _storedBio;
	}

	void setStoredBio(QString text) {
		_storedBio = std::move(text);
	}

	[[nodiscard]] const QString &editorBio() const {
		return _editorBio;
	}

	void setEditorBio(QString text) {
		_editorBio = std::move(text);
	}

	template <typename Field, typename Text>
	bool aboutNotSupported(Field &field, Text text) {
		cancelDebounce();
		_editorBio = _storedBio;
		using HistoryAction = typename Field::HistoryAction;
		field.setTextWithTags(
			std::move(text),
			HistoryAction::Clear);
		return true;
	}

	[[nodiscard]] int scheduleDebounce() {
		_debounceGeneration = std::abs(_debounceGeneration) + 1;
		return _debounceGeneration;
	}

	void markUnchanged() {
		if (_debounceGeneration > 0) {
			_debounceGeneration = -_debounceGeneration;
		}
	}

	[[nodiscard]] bool debounceIsCurrent(int scheduled) const {
		return (_debounceGeneration == scheduled);
	}

	template <typename Save>
	bool runDebounce(int scheduled, Save save) {
		if (!debounceIsCurrent(scheduled)) {
			return false;
		}
		save();
		cancelDebounce();
		return true;
	}

	void cancelDebounce() {
		_debounceGeneration = 0;
	}

	[[nodiscard]] bool needsCloseRetry() const {
		return (_debounceGeneration > 0);
	}

	template <typename Save>
	bool saveOnClose(Save save) const {
		if (!needsCloseRetry()) {
			return false;
		}
		save();
		return true;
	}

private:
	QString _storedBio;
	QString _editorBio;
	int _debounceGeneration = 0;

};

} // namespace Api
