/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

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

template <typename Recovery>
[[nodiscard]] inline BioSaveFailureTransition ResolveBioSaveFailure(
		const QString &errorType,
		QString &requestedText,
		const Recovery &recover) {
	const auto action = ClassifyBioSaveFailure(errorType);
	if (action == BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry) {
		requestedText = QString();
		return { action, !recover() };
	}
	return { action };
}

inline void CancelBioSaveDebounce(int &generation) {
	generation = 0;
}

[[nodiscard]] inline bool BioSaveDebounceIsCurrent(
		int generation,
		int scheduled) {
	return (generation == scheduled);
}

[[nodiscard]] inline bool BioSaveNeedsCloseRetry(int generation) {
	return (generation > 0);
}

} // namespace Api
