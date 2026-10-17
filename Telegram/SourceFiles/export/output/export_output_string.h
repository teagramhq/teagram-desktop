/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>

namespace Export::Output::details::Json {

inline QByteArray SerializeString(const QByteArray &value) {
	const auto size = value.size();
	const auto begin = value.data();
	const auto end = begin + size;

	auto result = QByteArray();
	result.reserve(2 + size * 4);
	result.append('"');
	for (auto p = begin; p != end; ++p) {
		const auto ch = *p;
		const auto byte = static_cast<unsigned char>(ch);
		if (ch == '\n') {
			result.append("\\n", 2);
		} else if (ch == '\r') {
			result.append("\\r", 2);
		} else if (ch == '\t') {
			result.append("\\t", 2);
		} else if (ch == '"') {
			result.append("\\\"", 2);
		} else if (ch == '\\') {
			result.append("\\\\", 2);
		} else if (byte < 32) {
			result.append("\\x", 2).append('0' + (byte >> 4));
			const auto left = (byte & 0x0F);
			if (left >= 10) {
				result.append('A' + (left - 10));
			} else {
				result.append('0' + left);
			}
		} else if (byte == 0xE2 && (p + 2 < end)
				   && static_cast<unsigned char>(*(p + 1)) == 0x80) {
			if (static_cast<unsigned char>(*(p + 2)) == 0xA8) {
				result.append("\\u2028", 6);
			} else if (static_cast<unsigned char>(*(p + 2)) == 0xA9) {
				result.append("\\u2029", 6);
			} else {
				result.append(ch);
			}
		} else {
			result.append(ch);
		}
	}
	result.append('"');
	return result;
}

} // namespace Export::Output::details::Json

namespace Export::Output::details::Html {

inline QByteArray SerializeString(const QByteArray &value) {
	const auto size = value.size();
	const auto begin = value.data();
	const auto end = begin + size;

	auto result = QByteArray();
	result.reserve(size * 6);
	for (auto p = begin; p != end; ++p) {
		const auto ch = *p;
		const auto byte = static_cast<unsigned char>(ch);
		if (ch == '\n') {
			result.append("<br>", 4);
		} else if (ch == '"') {
			result.append("&quot;", 6);
		} else if (ch == '&') {
			result.append("&amp;", 5);
		} else if (ch == '\'') {
			result.append("&apos;", 6);
		} else if (ch == '<') {
			result.append("&lt;", 4);
		} else if (ch == '>') {
			result.append("&gt;", 4);
		} else if (byte < 32) {
			result.append("&#x", 3).append('0' + (byte >> 4));
			const auto left = (byte & 0x0F);
			if (left >= 10) {
				result.append('A' + (left - 10));
			} else {
				result.append('0' + left);
			}
			result.append(';');
		} else if (byte == 0xE2 && (p + 2 < end)
				   && static_cast<unsigned char>(*(p + 1)) == 0x80) {
			if (static_cast<unsigned char>(*(p + 2)) == 0xA8) {
				result.append("<br>", 4);
			} else if (static_cast<unsigned char>(*(p + 2)) == 0xA9) {
				result.append("<br>", 4);
			} else {
				result.append(ch);
			}
		} else {
			result.append(ch);
		}
	}
	return result;
}

} // namespace Export::Output::details::Html
