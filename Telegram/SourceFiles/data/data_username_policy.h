/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_input_policy.h"

#include <QtCore/QRegularExpression>

namespace Data {
namespace details {

struct UsernamePolicy {
	int minLength = 0;
	int maxLength = 0;
	QString pattern;
};

template <typename AppConfig>
[[nodiscard]] UsernamePolicy
UsernamePolicyFromAppConfig(const AppConfig &appConfig) {
	return {
		.minLength = appConfig.get<int>(u"username_min_length"_q, 0),
		.maxLength = appConfig.get<int>(u"username_max_length"_q, 0),
		.pattern = appConfig.get<QString>(u"username_pattern"_q, QString()),
	};
}

[[nodiscard]] inline bool IsValidUsername(const QString &username,
										  const UsernamePolicy &policy) {
	if (username.isEmpty()) {
		return false;
	}
	const auto length = UnicodeScalarCount(username);
	if ((policy.minLength > 0 && length < policy.minLength)
		|| ExceedsInputLengthLimit(length, policy.maxLength)) {
		return false;
	} else if (policy.pattern.isEmpty()) {
		return true;
	}
	const auto pattern = QRegularExpression(policy.pattern);
	return !pattern.isValid() || pattern.match(username).hasMatch();
}

} // namespace details
} // namespace Data
