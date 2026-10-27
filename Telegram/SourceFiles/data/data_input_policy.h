/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <limits>

namespace Data {
namespace details {

[[nodiscard]] inline int UnicodeScalarCount(const QString &text) {
	auto result = 0;
	for (auto i = 0; i != text.size(); ++i) {
		if (text[i].isHighSurrogate() && (i + 1) < text.size()
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
		++result;
	}
	return result;
}

[[nodiscard]] inline bool ExceedsInputLengthLimit(int length, int maxLength) {
	return maxLength > 0 && length > maxLength;
}

[[nodiscard]] inline bool
IsWithinInputLengthPolicy(const QString &text, int minLength, int maxLength) {
	const auto length = UnicodeScalarCount(text);
	return (minLength <= 0 || length >= minLength)
		   && !ExceedsInputLengthLimit(length, maxLength);
}

[[nodiscard]] inline int InputLengthLimitForSplitting(int maxLength) {
	return (maxLength > 0) ? maxLength : std::numeric_limits<int>::max();
}

} // namespace details
} // namespace Data
