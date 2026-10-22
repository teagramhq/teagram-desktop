/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_authorization.h"
#include "mtproto/sender.h"

class ApiWrap;

namespace Api {

namespace details {

class AuthorizationsReloadState final {
  public:
	void setRequestId(mtpRequestId requestId) { _requestId = requestId; }

	[[nodiscard]] bool pending() const { return _requestId != 0; }

	void complete() { _requestId = 0; }

	void fail(const MTP::Error &error) {
		_requestId = 0;
		_failures.fire_copy(error);
	}

	[[nodiscard]] mtpRequestId takeRequestId() {
		const auto result = _requestId;
		_requestId = 0;
		return result;
	}

	[[nodiscard]] rpl::producer<MTP::Error> failures() const {
		return _failures.events();
	}

  private:
	mtpRequestId _requestId = 0;
	rpl::event_stream<MTP::Error> _failures;
};

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

	MTP::Sender _api;
	details::AuthorizationsReloadState _reloadState;

	List _list;
	rpl::event_stream<> _listChanges;

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
