/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace Dialogs::details {

template <typename Row, typename Key, typename Index>
[[nodiscard]] auto ReorderChangedRows(const std::vector<Row> &original,
									  std::vector<Row> pending, Key key,
									  Index index)
	-> std::pair<std::vector<Row>, int> {
	const auto needsMove
		= std::any_of(pending.begin(), pending.end(), [&](const auto &row) {
			  const auto i = index(row);
			  return (i > 0 && key(original[i - 1]) < key(row))
					 || (i + 1 < int(original.size())
						 && key(row) < key(original[i + 1]));
		  });
	if (!needsMove) {
		return {};
	}
	std::sort(
		pending.begin(), pending.end(),
		[&](const auto &a, const auto &b) { return index(a) < index(b); });
	auto adjusted = pending;
	std::stable_sort(
		adjusted.begin(), adjusted.end(),
		[&](const auto &a, const auto &b) { return key(a) > key(b); });
	auto rows = std::vector<Row>();
	rows.reserve(original.size());
	auto nextAdjusted = adjusted.begin();
	auto nextPending = pending.begin();
	auto rowIt = original.begin();
	while (rowIt != original.end()) {
		if (nextAdjusted == adjusted.end()) {
			for (; nextPending != pending.end(); ++nextPending) {
				const auto skip = original.begin() + index(*nextPending);
				rows.insert(rows.end(), rowIt, skip);
				rowIt = skip + 1;
			}
			break;
		}
		const auto row = *rowIt++;
		if (nextPending != pending.end() && row == *nextPending) {
			++nextPending;
			continue;
		}
		const auto rowKey = key(row);
		while (nextAdjusted != adjusted.end()
			   && (key(*nextAdjusted) > rowKey
				   || (key(*nextAdjusted) == rowKey
					   && index(*nextAdjusted) < index(row)))) {
			rows.push_back(*nextAdjusted++);
		}
		rows.push_back(row);
	}
	rows.insert(rows.end(), nextAdjusted, adjusted.end());
	const auto changedEnd = int(rows.size());
	rows.insert(rows.end(), rowIt, original.end());
	return {std::move(rows), changedEnd};
}

} // namespace Dialogs::details
