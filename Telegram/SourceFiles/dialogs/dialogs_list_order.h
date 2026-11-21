/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

namespace Dialogs::details {

// The returned rows are the reordered prefix; the original tail is unchanged.
template <typename Row, typename Key, typename Index>
[[nodiscard]] auto ReorderChangedRows(const std::vector<Row> &original,
									  std::vector<Row> pending, Key key,
									  Index index)
	-> std::pair<std::vector<Row>, int> {
	const auto needsMove
		= std::any_of(pending.begin(), pending.end(), [&](const auto &row) {
			  const auto i = index(row);
			  const auto rowKey = key(row);
			  return (i > 0 && key(original[i - 1]) < rowKey)
					 || (i + 1 < int(original.size())
						 && rowKey < key(original[i + 1]));
		  });
	if (!needsMove) {
		return {};
	}
	std::sort(
		pending.begin(), pending.end(),
		[&](const auto &a, const auto &b) { return index(a) < index(b); });
	using SortKey = std::decay_t<decltype(key(std::declval<Row>()))>;
	struct AdjustedRow {
		Row row;
		SortKey key;
		int index;
	};
	auto adjusted = std::vector<AdjustedRow>();
	adjusted.reserve(pending.size());
	for (const auto &row : pending) {
		adjusted.push_back({row, key(row), index(row)});
	}
	std::sort(
		adjusted.begin(), adjusted.end(), [](const auto &a, const auto &b) {
			return (a.key == b.key) ? (a.index < b.index) : (a.key > b.key);
		});
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
			   && (nextAdjusted->key > rowKey
				   || (nextAdjusted->key == rowKey
					   && nextAdjusted->index < index(row)))) {
			rows.push_back(nextAdjusted->row);
			++nextAdjusted;
		}
		rows.push_back(row);
	}
	for (; nextAdjusted != adjusted.end(); ++nextAdjusted) {
		rows.push_back(nextAdjusted->row);
	}
	const auto changedEnd = int(rows.size());
	return {std::move(rows), changedEnd};
}

} // namespace Dialogs::details
