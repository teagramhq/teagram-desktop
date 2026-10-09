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

[[nodiscard]] inline BioSaveFailureAction ClassifyBioSaveFailure(
		const QString &errorType) {
	return (errorType == u"ABOUT_NOT_SUPPORTED"_q)
		? BioSaveFailureAction::RestoreStoredValueWithoutWriteOrRetry
		: BioSaveFailureAction::KeepExistingBehavior;
}

} // namespace Api
