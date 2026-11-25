/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_changes.h"
#include "tests/data_update_routing_test_helpers.h"

#include <cstdint>
#include <vector>

namespace {

template <typename T> not_null<T *> OpaqueObject(std::uintptr_t value) {
	return reinterpret_cast<T *>(value);
}

} // namespace

TEST_CASE(PeerScopedUpdateRoutingReclaimsAndDeliversByPeer) {
	auto changes = Data::Changes();
	CHECK(Data::tests::PeerScopedUpdateRoutingRegressionPasses(
		changes, OpaqueObject<PeerData>(1), OpaqueObject<PeerData>(2)));
}

TEST_CASE(SharedMediaInvalidationRoutingReclaimsSubscriptions) {
	CHECK(Data::tests::SharedMediaInvalidationRoutingRegressionPasses());
}

TEST_CASE(DestroyedSublistUpdateIsTheFinalScopedDelivery) {
	auto changes = Data::Changes();
	const auto sublist = OpaqueObject<SavedSublist>(1);
	using Flag = Data::SublistUpdate::Flag;
	auto lifetime = rpl::lifetime();
	auto deliveries = std::vector<Data::SublistUpdate::Flags>();
	changes.sublistUpdates(sublist, Flag::CloudDraft | Flag::Destroyed)
		| rpl::on_next(
			[&](const Data::SublistUpdate &update) {
				deliveries.push_back(update.flags);
			},
			lifetime);

	changes.sublistUpdated(sublist, Flag::CloudDraft);
	changes.sublistUpdated(sublist, Flag::Destroyed);
	CHECK(deliveries.size() == 1U);
	CHECK(deliveries.front() == (Flag::CloudDraft | Flag::Destroyed));

	changes.sendNotifications();
	changes.sublistUpdated(sublist, Flag::CloudDraft);
	changes.sendNotifications();
	CHECK(deliveries.size() == 1U);
}
