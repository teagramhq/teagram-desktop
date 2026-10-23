/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_message_reactions_batch.h"

#include <vector>

namespace {

struct AvailableReactionFixture {
	int documents = 4;
};

} // namespace

TEST_CASE(AvailableReactionsResponseWith73ItemsUsesOneItemPerTurn) {
	auto response = std::vector<AvailableReactionFixture>(73);
	auto position = 0;
	auto processed = 0;
	auto processedDocuments = 0;
	auto turns = 0;
	while (position < int(response.size())) {
		const auto before = position;
		auto documentsThisTurn = 0;
		const auto more = Data::details::ProcessAvailableReactionsBatch(
			response,
			position,
			[&](const AvailableReactionFixture &reaction) {
				++processed;
				documentsThisTurn = reaction.documents;
				processedDocuments += reaction.documents;
			});
		CHECK_EQ(position - before, 1);
		CHECK_EQ(more, (position < int(response.size())));
		CHECK_EQ(documentsThisTurn, 4);
		++turns;
	}
	CHECK_EQ(processed, 73);
	CHECK_EQ(processedDocuments, 292);
	CHECK_EQ(turns, 73);
}
