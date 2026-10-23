/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "data/data_authorization.h"
#include "mtproto/sender.h"

#include <optional>

class ApiWrap;

namespace Api {

namespace details {

class AuthorizationsReloadState final {
  public:
	using Generation = uint64;
	static constexpr auto kTimeoutMs = 10'000;

	[[nodiscard]] Generation begin() {
		_pending = true;
		_requestId = 0;
		return ++_generation;
	}

	void setRequestId(Generation generation, mtpRequestId requestId) {
		if (isCurrent(generation)) {
			_requestId = requestId;
		}
	}

	[[nodiscard]] bool pending() const { return _pending; }

	[[nodiscard]] bool isCurrent(Generation generation) const {
		return _pending && (generation == _generation);
	}

	[[nodiscard]] bool complete(Generation generation) {
		if (!isCurrent(generation)) {
			return false;
		}
		_pending = false;
		_requestId = 0;
		return true;
	}

	void fail(Generation generation, const MTP::Error &error) {
		if (!complete(generation)) {
			return;
		}
		_lastFailureType = error.type();
		_failures.fire_copy(error);
	}

	[[nodiscard]] mtpRequestId timeout(Generation generation) {
		const auto requestId = _requestId;
		if (!isCurrent(generation)) {
			return 0;
		}
		fail(generation,
			 MTP::Error::Local(u"AUTHORIZATIONS_RELOAD_TIMEOUT"_q, {}));
		return requestId;
	}

	[[nodiscard]] bool succeed(Generation generation) {
		if (!complete(generation)) {
			return false;
		}
		_lastFailureType = QString();
		return true;
	}

	[[nodiscard]] mtpRequestId takeRequestId() {
		const auto result = _requestId;
		_requestId = 0;
		_pending = false;
		_lastFailureType = QString();
		++_generation;
		return result;
	}

	[[nodiscard]] const QString &lastFailureType() const {
		return _lastFailureType;
	}

	[[nodiscard]] rpl::producer<MTP::Error> failures() const {
		return _failures.events();
	}

  private:
	mtpRequestId _requestId = 0;
	Generation _generation = 0;
	bool _pending = false;
	QString _lastFailureType;
	rpl::event_stream<MTP::Error> _failures;
};

template <typename Timer, typename Callback>
void ScheduleAuthorizationsReloadTimeout(Timer &timer, Fn<void()> &action,
										 crl::time timeout,
										 Callback &&callback) {
	action = Fn<void()>(std::forward<Callback>(callback));
	timer.callOnce(timeout);
}

template <typename Request, typename ScheduleTimeout, typename CancelTimeout,
		  typename CancelRequest, typename Done>
void RunAuthorizationsReload(AuthorizationsReloadState &state,
							 crl::time timeout, Request &&request,
							 ScheduleTimeout &&scheduleTimeout,
							 CancelTimeout &&cancelTimeout,
							 CancelRequest &&cancelRequest, Done &&done) {
	if (state.pending()) {
		return;
	}
	const auto generation = state.begin();
	auto cancelOnDone = std::forward<CancelTimeout>(cancelTimeout);
	auto cancelOnFail = cancelOnDone;
	auto cancelOnTimeout = cancelOnDone;
	auto cancelRequestOnTimeout = std::forward<CancelRequest>(cancelRequest);
	auto doneHandler = std::forward<Done>(done);
	const auto requestId = std::forward<Request>(request)(
		[statePtr = &state, generation, cancel = std::move(cancelOnDone),
		 done = std::move(doneHandler)](const auto &result) mutable {
			if (!statePtr->succeed(generation)) {
				return;
			}
			cancel();
			done(result);
		},
		[statePtr = &state, generation,
		 cancel = std::move(cancelOnFail)](const MTP::Error &error) mutable {
			if (!statePtr->isCurrent(generation)) {
				return;
			}
			cancel();
			statePtr->fail(generation, error);
		});
	state.setRequestId(generation, requestId);
	if (state.isCurrent(generation)) {
		std::forward<ScheduleTimeout>(scheduleTimeout)(
			timeout,
			[statePtr = &state, generation, cancel = std::move(cancelOnTimeout),
			 cancelRequest = std::move(cancelRequestOnTimeout)]() mutable {
				const auto id = statePtr->timeout(generation);
				cancel();
				if (id) {
					cancelRequest(id);
				}
			});
	}
}

} // namespace details

class Authorizations final {
public:
	explicit Authorizations(not_null<ApiWrap*> api);

	struct Entry {
		uint64 hash = 0;

		bool incomplete = false;
		bool callsDisabled = false;
		int apiId = 0;
		TimeId activeTime = 0;
		QString name, active, info, ip, location, system, platform;
	};
	using List = std::vector<Entry>;

	void reload();
	void cancelCurrentRequest();
	void requestTerminate(
		Fn<void(const MTPBool &result)> &&done,
		Fn<void(const MTP::Error &error)> &&fail,
		std::optional<uint64> hash = std::nullopt);

	void apply(const MTPUpdate &update);

	[[nodiscard]] crl::time lastReceivedTime();
	[[nodiscard]] bool hasAuthoritativeList() const;
	[[nodiscard]] rpl::producer<MTP::Error> reloadFailures() const;
	[[nodiscard]] rpl::producer<> reloadSuccesses() const;
	[[nodiscard]] QString lastReloadFailureType() const;

	[[nodiscard]] List list() const;
	[[nodiscard]] rpl::producer<List> listValue() const;
	[[nodiscard]] int total() const;
	[[nodiscard]] rpl::producer<int> totalValue() const;

	[[nodiscard]] const std::vector<Data::UnreviewedAuth> &unreviewed();
	[[nodiscard]] rpl::producer<> unreviewedChanges() const;

	void review(const std::vector<uint64> &hashes, bool confirm);

	void updateTTL(int days);
	[[nodiscard]] rpl::producer<int> ttlDays() const;

	void toggleCallsDisabledHere(bool disabled) {
		toggleCallsDisabled(0, disabled);
	}
	void toggleCallsDisabled(uint64 hash, bool disabled);
	[[nodiscard]] bool callsDisabledHere() const;
	[[nodiscard]] rpl::producer<bool> callsDisabledHereValue() const;
	[[nodiscard]] rpl::producer<bool> callsDisabledHereChanges() const;

	[[nodiscard]] static QString ActiveDateString(TimeId active);

private:
	void refreshCallsDisabledHereFromCloud();
	void removeExpiredUnreviewed();
	void reloadTimedOut();

	MTP::Sender _api;
	details::AuthorizationsReloadState _reloadState;
	std::optional<base::Timer> _reloadTimeout;
	Fn<void()> _reloadTimeoutAction;

	List _list;
	rpl::event_stream<> _listChanges;
	rpl::event_stream<> _reloadSuccesses;

	Fn<int()> _autoconfirmPeriod;
	std::vector<Data::UnreviewedAuth> _unreviewed;
	rpl::event_stream<> _unreviewedChanges;
	Fn<void()> _saveUnreviewed;

	mtpRequestId _ttlRequestId = 0;
	rpl::variable<int> _ttlDays = 0;

	base::flat_map<uint64, mtpRequestId> _toggleCallsDisabledRequests;
	base::flat_map<uint64, mtpRequestId> _reviewRequests;
	rpl::variable<bool> _callsDisabledHere;

	crl::time _lastReceived = 0;
	bool _hasAuthoritativeList = false;
	rpl::lifetime _lifetime;

};

} // namespace Api
