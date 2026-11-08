/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "mtproto/details/mtproto_dump_to_text.h"
#include "scheme.h"

namespace {

template <typename Value> QString DumpMessage(const Value &value) {
	auto body = mtpBuffer();
	value.template write<mtpBuffer>(body);

	auto message = mtpBuffer();
	MTP_long(1).write(message);
	MTP_int(1).write(message);
	MTP_int(static_cast<int32>(body.size() * sizeof(mtpPrime))).write(message);
	message += body;

	auto from = message.constData();
	return MTP::details::DumpToText(from, from + message.size());
}

TEST_CASE(InviteSignInSecretIsRedactedFromMtpDump) {
	const auto secret = u"invite-secret-0123456789abcdef"_q;
	const auto dump = DumpMessage(MTPauth_SignIn(
		MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
		MTP_string(u"+15551234567"_q), MTP_string(u"phone-code-hash"_q),
		MTP_string(secret), MTP_emailVerificationCode(MTP_string(QString()))));

	CHECK(!dump.contains(secret));
	CHECK(dump.contains(u"phone_code: [REDACTED] [STRING]"_q));
}

TEST_CASE(InviteLinkSecretIsRedactedFromMtpDump) {
	const auto secret
		= u"https://teagram.example/#invite-secret-0123456789abcdef"_q;
	const auto dump
		= DumpMessage(MTPteagram_UserInviteLink(MTP_teagram_userInviteLink(
			MTP_long(42), MTP_string(secret), MTP_int(1'800'000'000))));

	CHECK(!dump.contains(secret));
	CHECK(dump.contains(u"link: [REDACTED] [STRING]"_q));
}

} // namespace
