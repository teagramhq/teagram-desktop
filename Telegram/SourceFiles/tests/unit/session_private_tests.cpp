/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "mtproto/session_private.h"

#include <optional>

namespace {

using namespace MTP;
using namespace MTP::details;

[[nodiscard]] SerializedRequest
MakeUsernameCheckRequest(mtpRequestId requestId) {
	auto request = SerializedRequest::Serialize(MTPauth_SendCode(
		MTP_string(u"steward"_q), MTP_int(1), MTP_string(u"test-api-hash"_q),
		MTP_codeSettings(MTP_flags(0), MTPVector<MTPbytes>(), MTPstring(),
						 MTPBool())));
	request->requestId = requestId;
	return request;
}

TEST_CASE(QueuedUsernameCheckCompletesAfterIdleSameSaltReconnect) {
	auto state = ConnectedState;
	state
		= ConnectingState; // The idle connection closed with auth still valid.
	const auto wasConnected = (state == ConnectedState);
	CHECK_EQ(state, ConnectingState);
	auto queued
		= std::optional<SerializedRequest>(MakeUsernameCheckRequest(17));
	auto sent = std::optional<SerializedRequest>();
	CHECK(queued.has_value());
	CHECK(!sent.has_value());
	auto sessionSalt = uint64(42);
	auto resentPreviouslySentRequests = 0;
	auto resumedRequests = 0;
	auto untrackedPongIgnored = false;

	const auto reconnectReplyProcessed = ProcessAuthenticatedResponse(
		state, wasConnected, sessionSalt, uint64(42), false,
		[&] {
			if (state != ConnectingState) {
				return false;
			}
			state = ConnectedState;
			return true;
		},
		[&] { ++resentPreviouslySentRequests; },
		[&] {
			untrackedPongIgnored = true;
			// An authenticated pong for an expired request is ignored by the
			// message handler, but still proves that the session is live.
			return true;
		},
		[&] { return state; },
		[&] {
			++resumedRequests;
			CHECK_EQ(state, ConnectedState);
			CHECK(queued.has_value());
			sent = std::move(queued);
			queued.reset();
		});

	CHECK(reconnectReplyProcessed);
	CHECK_EQ(sessionSalt, uint64(42));
	CHECK_EQ(state, ConnectedState);
	CHECK_EQ(resentPreviouslySentRequests, 1);
	CHECK_EQ(resumedRequests, 1);
	CHECK(untrackedPongIgnored);
	CHECK(!queued.has_value());
	CHECK(sent.has_value());

	auto usernameCheckSucceeded = false;
	auto requestThatReceivedResponse = mtpRequestId(0);
	const auto reply = mtpBuffer{
		mtpPrime(0x5e002502), mtpPrime(0),			mtpPrime(0xc000bba2),
		mtpPrime(5),		  mtpPrime(0x73616804), mtpPrime(0x00000068),
	};
	const auto usernameReplyProcessed = ProcessAuthenticatedResponse(
		state, true, sessionSalt, uint64(42), false, [] { return false; },
		[] {},
		[&] {
			if (!sent) {
				return false;
			}
			requestThatReceivedResponse = (*sent)->requestId;
			CHECK_EQ(requestThatReceivedResponse, mtpRequestId(17));
			CHECK_EQ(
				uint32((*sent)->at(SerializedRequest::kMessageBodyPosition)),
				uint32(0xa677244f));

			auto result = MTPauth_SentCode();
			auto from = reply.constData();
			const auto read = result.read(from, from + reply.size());
			if (!read) {
				return false;
			}
			result.match(
				[&](const MTPDauth_sentCode &data) {
					usernameCheckSucceeded
						= (data.vphone_code_hash().v == u"hash"_q);
				},
				[&](const MTPDauth_sentCodeSuccess &) {},
				[&](const MTPDauth_sentCodePaymentRequired &) {});
			return usernameCheckSucceeded;
		},
		[&] { return state; }, [&] { ++resumedRequests; });

	CHECK(usernameReplyProcessed);
	CHECK_EQ(requestThatReceivedResponse, mtpRequestId(17));
	CHECK_EQ(resumedRequests, 1);
	CHECK(usernameCheckSucceeded);
}

TEST_CASE(BadTimeAuthenticatedResponseDoesNotCompleteReconnect) {
	auto state = ConnectingState;
	auto sessionSalt = uint64(42);
	auto markedConnected = false;
	auto resent = false;
	auto resumed = false;
	const auto processed = ProcessAuthenticatedResponse(
		state, false, sessionSalt, uint64(84), true,
		[&] {
			markedConnected = true;
			return true;
		},
		[&] { resent = true; }, [] { return true; }, [&] { return state; },
		[&] { resumed = true; });

	CHECK(processed);
	CHECK_EQ(sessionSalt, uint64(42));
	CHECK_EQ(state, ConnectingState);
	CHECK(!markedConnected);
	CHECK(!resent);
	CHECK(!resumed);
}

TEST_CASE(AuthenticatedResponseDoesNotRestartAlreadyConnectedSession) {
	auto state = ConnectedState;
	auto sessionSalt = uint64(42);
	auto markedConnected = false;
	auto resent = false;
	auto resumed = false;
	const auto processed = ProcessAuthenticatedResponse(
		state, true, sessionSalt, uint64(84), false,
		[&] {
			markedConnected = true;
			return true;
		},
		[&] { resent = true; }, [] { return true; }, [&] { return state; },
		[&] { resumed = true; });

	CHECK(processed);
	CHECK_EQ(sessionSalt, uint64(84));
	CHECK_EQ(state, ConnectedState);
	CHECK(!markedConnected);
	CHECK(!resent);
	CHECK(!resumed);
}

} // namespace
