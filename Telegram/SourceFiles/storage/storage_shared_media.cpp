/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_shared_media.h"

#include <rpl/map.h>

namespace Storage {

auto SharedMedia::enforceLists(Key key)
-> std::map<Key, SharedMedia::Lists>::iterator {
	auto result = _lists.find(key);
	if (result != _lists.end()) {
		return result;
	}
	result = _lists.emplace(key, Lists {}).first;
	for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
		auto &list = result->second[index];
		auto type = static_cast<SharedMediaType>(index);

		list.sliceUpdated(
		) | rpl::map([=](const SparseIdsSliceUpdate &update) {
			return SharedMediaSliceUpdate(
				key.peerId,
				key.topicRootId,
				key.monoforumPeerId,
				type,
				update);
		}) | rpl::start_to_stream(_sliceUpdated, _lifetime);
	}
	return result;
}

void SharedMedia::add(SharedMediaAddNew &&query) {
	const auto addByIt = [&](const auto i) {
		for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
			auto type = static_cast<SharedMediaType>(index);
			if (query.types.test(type)) {
				i->second[index].addNew(query.messageId);
			}
		}
	};
	addByIt(enforceLists({ query.peerId, MsgId(0) }));
	const auto topicIt = query.topicRootId
		? _lists.find({ query.peerId, query.topicRootId })
		: end(_lists);
	if (topicIt != end(_lists)) {
		addByIt(topicIt);
	}
	const auto monoforumPeerIt = query.monoforumPeerId
		? _lists.find({ query.peerId, MsgId(), query.monoforumPeerId })
		: end(_lists);
	if (monoforumPeerIt != end(_lists)) {
		addByIt(monoforumPeerIt);
	}
}

void SharedMedia::add(SharedMediaAddExisting &&query) {
	auto peerIt = enforceLists({
		query.peerId,
		query.topicRootId,
		query.monoforumPeerId,
	});
	for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
		auto type = static_cast<SharedMediaType>(index);
		if (query.types.test(type)) {
			peerIt->second[index].addExisting(
				query.messageId,
				query.noSkipRange);
		}
	}
}

void SharedMedia::add(SharedMediaAddSlice &&query) {
	Expects(IsValidSharedMediaType(query.type));

	auto peerIt = enforceLists({
		query.peerId,
		query.topicRootId,
		query.monoforumPeerId,
	});
	auto index = static_cast<int>(query.type);
	peerIt->second[index].addSlice(
		std::move(query.messageIds),
		query.noSkipRange,
		query.count);
}

void SharedMedia::remove(SharedMediaRemoveOne &&query) {
	auto peerIt = _lists.lower_bound({ query.peerId, MsgId(0) });
	while (peerIt != end(_lists) && peerIt->first.peerId == query.peerId) {
		for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
			auto type = static_cast<SharedMediaType>(index);
			if (query.types.test(type)) {
				peerIt->second[index].removeOne(query.messageId);
			}
		}
		++peerIt;
	}
	_oneRemoved.fire(std::move(query));
}

void SharedMedia::remove(SharedMediaRemoveAll &&query) {
	auto peerIt = _lists.lower_bound({
		query.peerId,
		query.topicRootId,
		query.monoforumPeerId,
	});
	while (peerIt != end(_lists)
		&& peerIt->first.peerId == query.peerId
		&& (!query.topicRootId
			|| peerIt->first.topicRootId == query.topicRootId)
		&& (!query.monoforumPeerId
			|| peerIt->first.monoforumPeerId == query.monoforumPeerId)) {
		for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
			auto type = static_cast<SharedMediaType>(index);
			if (query.types.test(type)) {
				peerIt->second[index].removeAll();
			}
		}
		++peerIt;
	}
	_allRemoved.fire(std::move(query));
}

void SharedMedia::invalidate(SharedMediaInvalidateBottom &&query) {
	auto peerIt = _lists.lower_bound({ query.peerId, MsgId(0) });
	while (peerIt != end(_lists) && peerIt->first.peerId == query.peerId) {
		for (auto index = 0; index != kSharedMediaTypeCount; ++index) {
			peerIt->second[index].invalidateBottom();
		}
		++peerIt;
	}
	sendBottomInvalidated(query);
}

void SharedMedia::unload(SharedMediaUnloadThread &&query) {
	_lists.erase({ query.peerId, query.topicRootId, query.monoforumPeerId });
}

rpl::producer<SharedMediaResult> SharedMedia::query(SharedMediaQuery &&query) const {
	Expects(IsValidSharedMediaType(query.key.type));

	auto peerIt = _lists.find({
		query.key.peerId,
		query.key.topicRootId,
		query.key.monoforumPeerId,
	});
	if (peerIt != _lists.end()) {
		auto index = static_cast<int>(query.key.type);
		return peerIt->second[index].query(SparseIdsListQuery(
			query.key.messageId,
			query.limitBefore,
			query.limitAfter));
	}
	return [](auto consumer) {
		consumer.put_done();
		return rpl::lifetime();
	};
}

SharedMediaResult SharedMedia::snapshot(const SharedMediaQuery &query) const {
	Expects(IsValidSharedMediaType(query.key.type));

	auto peerIt = _lists.find({
		query.key.peerId,
		query.key.topicRootId,
		query.key.monoforumPeerId,
	});
	if (peerIt != _lists.end()) {
		auto index = static_cast<int>(query.key.type);
		return peerIt->second[index].snapshot(SparseIdsListQuery(
			query.key.messageId,
			query.limitBefore,
			query.limitAfter));
	}
	return {};
}

bool SharedMedia::empty(const SharedMediaKey &key) const {
	Expects(IsValidSharedMediaType(key.type));

	auto peerIt = _lists.find({
		key.peerId,
		key.topicRootId,
		key.monoforumPeerId,
	});
	if (peerIt != _lists.end()) {
		auto index = static_cast<int>(key.type);
		return peerIt->second[index].empty();
	}
	return true;
}

rpl::producer<SharedMediaSliceUpdate> SharedMedia::sliceUpdated() const {
	return _sliceUpdated.events();
}

rpl::producer<SharedMediaRemoveOne> SharedMedia::oneRemoved() const {
	return _oneRemoved.events();
}

rpl::producer<SharedMediaRemoveAll> SharedMedia::allRemoved() const {
	return _allRemoved.events();
}

rpl::producer<SharedMediaInvalidateBottom>
SharedMedia::bottomInvalidated(PeerId peerId) const {
	const auto streams
		= std::weak_ptr<BottomInvalidatedStreams>(_bottomInvalidatedStreams);
	return rpl::make_producer<SharedMediaInvalidateBottom>(
		[streams, peerId](const auto &consumer) {
			const auto strong = streams.lock();
			if (!strong) {
				return rpl::lifetime();
			}
			const auto i = strong->find(peerId);
			auto scoped = (i != strong->end())
							  ? i->second
							  : std::make_shared<BottomInvalidatedStream>();
			if (i == strong->end()) {
				strong->emplace(peerId, scoped);
			}
			const auto id = ++scoped->nextSubscriptionId;
			auto stream = std::make_shared<
				rpl::event_stream<SharedMediaInvalidateBottom>>();
			scoped->subscriptions.emplace(id, stream);
			auto result = rpl::lifetime(
				[streams, peerId, weakStream = std::weak_ptr(scoped), id] {
					const auto strong = streams.lock();
					const auto scoped = weakStream.lock();
					if (!strong || !scoped) {
						return;
					}
					const auto i = strong->find(peerId);
					if ((i != strong->end()) && (i->second == scoped)) {
						scoped->subscriptions.erase(id);
						if (scoped->subscriptions.empty()) {
							strong->erase(i);
						}
					}
				});
			auto events = stream->events();
			result.add(std::move(events).start_existing(consumer));
			return result;
		});
}

int SharedMedia::bottomInvalidatedSubscriberCountForRegressionTest(
	PeerId peerId) const {
	const auto i = _bottomInvalidatedStreams->find(peerId);
	return (i == _bottomInvalidatedStreams->end())
			   ? 0
			   : int(i->second->subscriptions.size());
}

void SharedMedia::sendBottomInvalidated(
	const SharedMediaInvalidateBottom &update) const {
	const auto i = _bottomInvalidatedStreams->find(update.peerId);
	if (i == _bottomInvalidatedStreams->end()) {
		return;
	}
	const auto scoped = i->second;
	const auto lastSubscriptionId = scoped->nextSubscriptionId;
	auto subscription = scoped->subscriptions.begin();
	while ((subscription != scoped->subscriptions.end())
		   && (subscription->first <= lastSubscriptionId)) {
		const auto id = subscription->first;
		const auto stream = subscription->second;
		stream->fire_copy(update);
		// A callback may unsubscribe itself or another observer while sending.
		subscription = scoped->subscriptions.upper_bound(id);
	}
}

} // namespace Storage
