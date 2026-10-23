/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"

#include <crl/crl.h>

#include <QtCore/QVector>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Data::details {

template <typename Items, typename Callback>
[[nodiscard]] bool ProcessAvailableReactionsBatch(const Items &items,
												  int &position,
												  Callback &&callback) {
	const auto count = int(items.size());
	if (position >= count) {
		return false;
	}
	callback(items[position++]);
	return position < count;
}

template <typename Item, typename Reaction>
class IncrementalAvailableReactionsUpdate final {
  public:
	using Parse = std::function<std::optional<Reaction>(const Item &)>;
	using Cache = std::function<void(const Reaction &)>;
	using AfterBatch = std::function<bool()>;
	using Finished = std::function<void(std::vector<Reaction> &&,
										std::vector<Reaction> &&, crl::time)>;

	IncrementalAvailableReactionsUpdate() : _timer([=] { processNext(); }) {}

	void start(QVector<Item> items, Parse parse, Cache cache,
			   AfterBatch afterBatch, Finished finished) {
		_items = std::move(items);
		_position = 0;
		_active.clear();
		_available.clear();
		_active.reserve(_items.size());
		_available.reserve(_items.size());
		_longestBatch = 0;
		_parse = std::move(parse);
		_cache = std::move(cache);
		_afterBatch = std::move(afterBatch);
		_finished = std::move(finished);
		_timer.callOnce(0);
	}

  private:
	void processNext() {
		const auto started = crl::now();
		const auto moreReactions = ProcessAvailableReactionsBatch(
			_items, _position, [=](const Item &item) {
				if (const auto parsed = _parse(item)) {
					_available.push_back(*parsed);
					if (parsed->active) {
						_active.push_back(*parsed);
						_cache(*parsed);
					}
				}
			});
		const auto moreCleanup = _afterBatch();
		_longestBatch = std::max(_longestBatch, crl::now() - started);
		if (moreReactions || moreCleanup) {
			_timer.callOnce(0);
			return;
		}
		auto finished = std::move(_finished);
		auto active = std::move(_active);
		auto available = std::move(_available);
		const auto longestBatch = _longestBatch;
		_items.clear();
		_parse = nullptr;
		_cache = nullptr;
		_afterBatch = nullptr;
		_finished = nullptr;
		_position = 0;
		_longestBatch = 0;
		finished(std::move(active), std::move(available), longestBatch);
	}

	base::Timer _timer;
	QVector<Item> _items;
	std::vector<Reaction> _active;
	std::vector<Reaction> _available;
	Parse _parse;
	Cache _cache;
	AfterBatch _afterBatch;
	Finished _finished;
	crl::time _longestBatch = 0;
	int _position = 0;
};

} // namespace Data::details
