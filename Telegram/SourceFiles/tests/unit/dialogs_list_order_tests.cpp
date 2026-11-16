/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "dialogs/dialogs_list_order.h"

namespace {

struct DialogState {
	int id;
	int index;
	int date;
	int lastMessage;
	int unread;
};

} // namespace

TEST_CASE(DialogOrderAfterBatchTouchesManyDialogs) {
	constexpr auto kCount = 256;
	auto dialogs = std::vector<DialogState>();
	dialogs.reserve(kCount);
	for (auto i = 0; i != kCount; ++i) {
		dialogs.push_back({i, i, kCount - i, 1, 0});
	}
	auto rows = std::vector<DialogState *>();
	for (auto &dialog : dialogs) {
		rows.push_back(&dialog);
	}
	auto changed = std::vector<DialogState *>();
	for (auto i = 50; i != 146; ++i) {
		dialogs[i].date = 1000 + i;
		dialogs[i].lastMessage = 2000 + i;
		dialogs[i].unread = 1;
		changed.push_back(&dialogs[i]);
	}
	const auto [ordered, changedEnd] = Dialogs::details::ReorderChangedRows(
		rows, changed, [](const auto *row) { return row->date; },
		[](const auto *row) { return row->index; });
	CHECK(ordered.size() == rows.size());
	CHECK_EQ(changedEnd, 146);
	if (ordered.size() != rows.size()) {
		return;
	}
	for (auto i = 0; i != kCount; ++i) {
		const auto expected = (i < 96) ? (145 - i) : (i < 146) ? (i - 96) : i;
		CHECK_EQ(ordered[i]->id, expected);
		CHECK_EQ(ordered[i]->lastMessage,
				 (expected >= 50 && expected < 146) ? 2000 + expected : 1);
		CHECK_EQ(ordered[i]->unread,
				 (expected >= 50 && expected < 146) ? 1 : 0);
	}
}

TEST_CASE(DialogOrderKeepsEqualDatesStable) {
	auto dialogs = std::vector<DialogState>{
		{0, 0, 10, 1, 0}, {1, 1, 10, 1, 0}, {2, 2, 10, 1, 0},
		{3, 3, 20, 2, 1}, {4, 4, 20, 2, 1}, {5, 5, 5, 1, 0},
	};
	auto rows = std::vector<DialogState *>();
	for (auto &dialog : dialogs) {
		rows.push_back(&dialog);
	}
	const auto [ordered, changedEnd] = Dialogs::details::ReorderChangedRows(
		rows, std::vector<DialogState *>{rows[3], rows[4]},
		[](const auto *row) { return row->date; },
		[](const auto *row) { return row->index; });
	CHECK_EQ(changedEnd, 5);
	CHECK(ordered
		  == std::vector<DialogState *>(
			  {rows[3], rows[4], rows[0], rows[1], rows[2], rows[5]}));
}
