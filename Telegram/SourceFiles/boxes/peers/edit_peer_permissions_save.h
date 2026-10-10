/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QtGlobal>

namespace Ui::EditPeer::Permissions {

struct SavePlan final {
	bool migrateChat = false;
	bool saveSlowmode = false;
	bool saveBoosts = false;
	bool savePrice = false;

	[[nodiscard]] int requestCount() const {
		return 1
			+ static_cast<int>(saveSlowmode)
			+ static_cast<int>(saveBoosts)
			+ static_cast<int>(savePrice);
	}
};

[[nodiscard]] inline SavePlan PlanSave(
	bool isChannel,
	bool paidFeaturesSupported,
	int slowmodeSeconds,
	int boostsUnrestrict,
	int starsPerMessage) {
	const auto savePaid = isChannel && paidFeaturesSupported;
	return {
		.migrateChat = !isChannel
			&& paidFeaturesSupported
			&& (slowmodeSeconds != 0
				|| boostsUnrestrict != 0
				|| starsPerMessage != 0),
		.saveSlowmode = isChannel,
		.saveBoosts = savePaid,
		.savePrice = savePaid,
	};
}

class SaveProgress final {
public:
	explicit SaveProgress(int requestCount) : _remaining(requestCount) {
		Q_ASSERT(requestCount > 0);
	}

	[[nodiscard]] bool complete(const QString &errorType = QString()) {
		if (!errorType.isEmpty()
			&& errorType != u"CHAT_NOT_MODIFIED"_q) {
			_failed = true;
			if (_error.isEmpty()) {
				_error = errorType;
			}
		}
		return advance();
	}

	// A request that a newer save of the same setting supersed is cancelled and
	// never answers. Counting it is what keeps Save from waiting on a request
	// that can never arrive. It fails the save, and it invents no error string:
	// the newer save is the one that wrote the value.
	[[nodiscard]] bool superseded() {
		_failed = true;
		return advance();
	}
	[[nodiscard]] bool finished() const {
		return !_remaining;
	}
	[[nodiscard]] bool succeeded() const {
		return finished() && !_failed;
	}
	[[nodiscard]] const QString &error() const {
		return _error;
	}

private:
	// Never run the count below zero: a completion counted twice would leave the
	// save waiting for a request that can no longer arrive. Both entry points
	// rely on this, a request answering after its supersession included.
	[[nodiscard]] bool advance() {
		if (_remaining <= 0) {
			return false;
		}
		return --_remaining == 0;
	}

	int _remaining = 0;
	bool _failed = false;
	QString _error;

};

} // namespace Ui::EditPeer::Permissions
