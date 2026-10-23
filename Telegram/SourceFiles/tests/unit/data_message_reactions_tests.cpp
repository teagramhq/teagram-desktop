/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_message_reactions.h"

#include <QtCore/QEventLoop>
#include <QtCore/QTimer>

#include <optional>
#include <vector>

namespace {

struct ParsedAvailableReaction {
	QString title;
	int documents = 0;
	bool active = false;
};

[[nodiscard]] MTPAvailableReaction MakeAvailableReaction(int index) {
	const auto document = [=](int offset) {
		return MTP_documentEmpty(MTP_long(index * 10 + offset));
	};
	const auto flags = 2 | (index % 2);
	return MTP_availableReaction(
		MTP_flags(flags), MTP_string(u"👍"_q),
		MTP_string(QString::number(index)), document(1), document(2),
		document(3), document(4), document(5), document(6), document(7));
}

} // namespace

TEST_CASE(AvailableReactionsResponseYieldsAndPublishesCompleteLists) {
	auto response = QVector<MTPAvailableReaction>();
	response.reserve(73);
	for (auto i = 0; i != 73; ++i) {
		response.push_back(MakeAvailableReaction(i));
	}

	auto active = std::vector<ParsedAvailableReaction>();
	auto available = std::vector<ParsedAvailableReaction>();
	auto hydratedDocuments = 0;
	auto cachedDocuments = 0;
	auto parsed = 0;
	auto completed = false;
	auto timedOut = false;
	auto markerRanBeforeCompletion = false;
	auto loop = QEventLoop();
	auto update = Data::details::IncrementalAvailableReactionsUpdate<
		MTPAvailableReaction, ParsedAvailableReaction>();

	update.start(
		std::move(response),
		[&](const MTPAvailableReaction &reaction)
			-> std::optional<ParsedAvailableReaction> {
			const auto &data = reaction.data();
			const auto documents
				= Data::details::HydrateAvailableReactionDocuments(
					data,
					[&](const MTPDocument &) { return ++hydratedDocuments; });
			++parsed;
			if (parsed == 1) {
				QTimer::singleShot(
					0, &loop, [&] { markerRanBeforeCompletion = !completed; });
			}
			return ParsedAvailableReaction{
				.title = qs(data.vtitle()),
				.documents = 2 + int(bool(documents.aroundAnimation))
							 + int(bool(documents.centerIcon)),
				.active = !data.is_inactive(),
			};
		},
		[&](const ParsedAvailableReaction &reaction) {
			cachedDocuments += reaction.active ? reaction.documents : 0;
		},
		[] { return false; },
		[&](std::vector<ParsedAvailableReaction> &&finishedActive,
			std::vector<ParsedAvailableReaction> &&finishedAvailable,
			crl::time) {
			active = std::move(finishedActive);
			available = std::move(finishedAvailable);
			completed = true;
			loop.quit();
		});

	QTimer::singleShot(5000, &loop, [&] {
		timedOut = true;
		loop.quit();
	});
	loop.exec();

	CHECK(!timedOut);
	CHECK(markerRanBeforeCompletion);
	CHECK(completed);
	CHECK_EQ(parsed, 73);
	CHECK_EQ(hydratedDocuments, 73 * 4);
	CHECK_EQ(cachedDocuments, 37 * 4);
	CHECK_EQ(int(available.size()), 73);
	CHECK_EQ(int(active.size()), 37);
	if (available.size() == 73) {
		for (auto i = 0; i != 73; ++i) {
			CHECK_EQ(available[i].title, QString::number(i));
			CHECK_EQ(available[i].documents, 4);
			CHECK_EQ(available[i].active, (i % 2 == 0));
		}
	}
	if (active.size() == 37) {
		for (auto i = 0; i != 37; ++i) {
			CHECK_EQ(active[i].title, QString::number(2 * i));
			CHECK(active[i].active);
			CHECK_EQ(active[i].documents, 4);
		}
	}
}
