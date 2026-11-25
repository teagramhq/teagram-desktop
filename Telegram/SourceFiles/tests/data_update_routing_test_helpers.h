/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_changes.h"
#include "storage/storage_shared_media.h"

#include <QtCore/QCoreApplication>

#include <vector>

namespace Data::tests {

[[nodiscard]] inline bool SharedMediaInvalidationRoutingRegressionPasses() {
	auto sharedMedia = Storage::SharedMedia();
	const auto firstPeer = PeerId(1'050'053);
	const auto secondPeer = PeerId(1'050'054);
	auto firstLifetime = rpl::lifetime();
	auto survivingLifetime = rpl::lifetime();
	auto secondLifetime = rpl::lifetime();
	auto midDispatchLifetime = rpl::lifetime();
	auto firstDeliveries = 0;
	auto survivingDeliveries = 0;
	auto secondDeliveries = 0;
	auto midDispatchDeliveries = 0;
	auto churnDeliveries = 0;
	auto installMidDispatchSubscriber = true;
	sharedMedia.bottomInvalidated(firstPeer)
		| rpl::on_next(
			[&](const Storage::SharedMediaInvalidateBottom &update) {
				if (update.peerId != firstPeer) {
					return;
				}
				++firstDeliveries;
				if (installMidDispatchSubscriber) {
					installMidDispatchSubscriber = false;
					sharedMedia.bottomInvalidated(firstPeer)
						| rpl::on_next(
							[&](const Storage::SharedMediaInvalidateBottom
									&value) {
								midDispatchDeliveries
									+= (value.peerId == firstPeer);
							},
							midDispatchLifetime);
				}
			},
			firstLifetime);
	sharedMedia.bottomInvalidated(firstPeer)
		| rpl::on_next(
			[&](const Storage::SharedMediaInvalidateBottom &update) {
				survivingDeliveries += (update.peerId == firstPeer);
			},
			survivingLifetime);
	sharedMedia.bottomInvalidated(secondPeer)
		| rpl::on_next(
			[&](const Storage::SharedMediaInvalidateBottom &update) {
				secondDeliveries += (update.peerId == secondPeer);
			},
			secondLifetime);
	const auto firstBaseline
		= sharedMedia.bottomInvalidatedSubscriberCountForRegressionTest(
			firstPeer);
	for (auto i = 0; i != 64; ++i) {
		auto churnLifetime = rpl::lifetime();
		sharedMedia.bottomInvalidated(firstPeer)
			| rpl::on_next(
				[&](const Storage::SharedMediaInvalidateBottom &update) {
					churnDeliveries += (update.peerId == firstPeer);
				},
				churnLifetime);
		churnLifetime.destroy();
	}
	const auto churnReclaimed
		= (sharedMedia.bottomInvalidatedSubscriberCountForRegressionTest(
			   firstPeer)
		   == firstBaseline);
	sharedMedia.invalidate(Storage::SharedMediaInvalidateBottom(firstPeer));
	const auto firstDispatchMatches
		= (firstDeliveries == 1) && (survivingDeliveries == 1)
		  && (secondDeliveries == 0) && (midDispatchDeliveries == 0)
		  && (churnDeliveries == 0);
	sharedMedia.invalidate(Storage::SharedMediaInvalidateBottom(firstPeer));
	const auto secondDispatchMatches
		= (firstDeliveries == 2) && (survivingDeliveries == 2)
		  && (secondDeliveries == 0) && (midDispatchDeliveries == 1)
		  && (churnDeliveries == 0);
	firstLifetime.destroy();
	const auto firstReclaimed
		= (sharedMedia.bottomInvalidatedSubscriberCountForRegressionTest(
			   firstPeer)
		   == firstBaseline);
	sharedMedia.invalidate(Storage::SharedMediaInvalidateBottom(firstPeer));
	const auto survivingSubscriberMatches
		= (firstDeliveries == 2) && (survivingDeliveries == 3)
		  && (secondDeliveries == 0) && (midDispatchDeliveries == 2)
		  && (churnDeliveries == 0);
	survivingLifetime.destroy();
	midDispatchLifetime.destroy();
	secondLifetime.destroy();
	return churnReclaimed && firstDispatchMatches && secondDispatchMatches
		   && firstReclaimed && survivingSubscriberMatches
		   && (sharedMedia.bottomInvalidatedSubscriberCountForRegressionTest(
				   firstPeer)
			   == 0)
		   && (sharedMedia.bottomInvalidatedSubscriberCountForRegressionTest(
				   secondPeer)
			   == 0);
}

[[nodiscard]] inline bool
PeerScopedUpdateRoutingRegressionPasses(Data::Changes &changes,
										not_null<PeerData *> first,
										not_null<PeerData *> second) {
	using Flag = Data::PeerUpdate::Flag;
	using Flags = Data::PeerUpdate::Flags;
	struct Delivery final {
		int subscriber = 0;
		PeerData *peer = nullptr;
		Flags flags;
	};
	auto deliveries = std::vector<Delivery>();
	auto expected = std::vector<Delivery>();
	const auto deliveriesMatch = [&] {
		if (deliveries.size() != expected.size()) {
			return false;
		}
		for (auto i = 0U; i != deliveries.size(); ++i) {
			if ((deliveries[i].subscriber != expected[i].subscriber)
				|| (deliveries[i].peer != expected[i].peer)
				|| (deliveries[i].flags != expected[i].flags)) {
				return false;
			}
		}
		return true;
	};
	const auto subscribe
		= [&](not_null<PeerData *> peer, int subscriber,
			  rpl::lifetime &lifetime, Flags flags = Flag::Name) {
			  changes.peerUpdates(peer, flags)
				  | rpl::on_next(
					  [&, subscriber](const Data::PeerUpdate &update) {
						  deliveries.push_back({
							  subscriber,
							  update.peer.get(),
							  update.flags,
						  });
					  },
					  lifetime);
		  };
	const auto send = [&](not_null<PeerData *> peer, Flags flags,
						  std::vector<int> subscribers) {
		changes.peerUpdated(peer, flags);
		changes.sendNotifications();
		for (const auto subscriber : subscribers) {
			expected.push_back({subscriber, peer.get(), flags});
		}
		return deliveriesMatch();
	};
	changes.sendNotifications();
	QCoreApplication::processEvents();
	changes.sendNotifications();
	const auto firstBaseline
		= changes.peerUpdateScopedSubscriptionCountForRegressionTest(first);
	const auto secondBaseline
		= changes.peerUpdateScopedSubscriptionCountForRegressionTest(second);
	auto firstLifetime = rpl::lifetime();
	auto secondLifetime = rpl::lifetime();
	auto globalLifetime = rpl::lifetime();
	auto survivingFirstLifetime = rpl::lifetime();
	auto midDispatchLifetime = rpl::lifetime();
	auto installMidDispatchSubscriber = true;
	subscribe(first, 1, firstLifetime);
	subscribe(second, 2, secondLifetime);
	changes.peerUpdates(Flag::Name)
		| rpl::on_next(
			[&](const Data::PeerUpdate &update) {
				if ((update.peer == first) || (update.peer == second)) {
					deliveries.push_back({3, update.peer.get(), update.flags});
				}
				if ((update.peer == first) && installMidDispatchSubscriber) {
					installMidDispatchSubscriber = false;
					changes.peerFlagsValue(first, Flag::Name)
						| rpl::on_next(
							[&](const Data::PeerUpdate &value) {
								deliveries.push_back({
									5,
									value.peer.get(),
									value.flags,
								});
							},
							midDispatchLifetime);
				}
			},
			globalLifetime);
	subscribe(first, 4, survivingFirstLifetime);
	auto countsMatch = [&] {
		return (changes.peerUpdateScopedSubscriptionCountForRegressionTest(
					first)
				== firstBaseline + 2)
			   && (changes.peerUpdateScopedSubscriptionCountForRegressionTest(
					   second)
				   == secondBaseline + 1);
	};
	auto routingPassed = countsMatch();
	routingPassed = send(first, Flag::Name, {3, 5, 1, 4}) && routingPassed;
	routingPassed
		= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
		   == firstBaseline + 3)
		  && routingPassed;
	routingPassed = send(first, Flag::Name, {3, 1, 4, 5}) && routingPassed;
	routingPassed = send(second, Flag::Name, {3, 2}) && routingPassed;
	routingPassed = send(first, Flag::About, {}) && routingPassed;
	firstLifetime.destroy();
	routingPassed
		= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
		   == firstBaseline + 2)
		  && routingPassed;
	for (auto i = 0; i != 64; ++i) {
		auto churnLifetime = rpl::lifetime();
		subscribe(first, 100 + i, churnLifetime);
		routingPassed
			= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
			   == firstBaseline + 3)
			  && routingPassed;
		churnLifetime.destroy();
		routingPassed
			= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
			   == firstBaseline + 2)
			  && routingPassed;
	}
	routingPassed = send(first, Flag::Name, {3, 4, 5}) && routingPassed;
	routingPassed = send(second, Flag::Name, {3, 2}) && routingPassed;
	routingPassed = send(first, Flag::About, {}) && routingPassed;
	survivingFirstLifetime.destroy();
	midDispatchLifetime.destroy();
	secondLifetime.destroy();
	routingPassed
		= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
		   == firstBaseline)
		  && (changes.peerUpdateScopedSubscriptionCountForRegressionTest(second)
			  == secondBaseline)
		  && routingPassed;
	auto reconnectedLifetime = rpl::lifetime();
	subscribe(first, 6, reconnectedLifetime);
	routingPassed
		= (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
		   == firstBaseline + 1)
		  && routingPassed;
	routingPassed = send(first, Flag::Name, {3, 6}) && routingPassed;
	reconnectedLifetime.destroy();
	globalLifetime.destroy();
	return routingPassed
		   && (changes.peerUpdateScopedSubscriptionCountForRegressionTest(first)
			   == firstBaseline)
		   && (changes.peerUpdateScopedSubscriptionCountForRegressionTest(
				   second)
			   == secondBaseline)
		   && deliveriesMatch();
}

} // namespace Data::tests
