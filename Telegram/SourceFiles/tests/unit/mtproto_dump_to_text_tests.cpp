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

QString DumpMessageBody(mtpBuffer &body, int32 truncatePrimes) {
	body.resize(body.size() - truncatePrimes);

	auto message = mtpBuffer();
	MTP_long(1).write(message);
	MTP_int(1).write(message);
	MTP_int(static_cast<int32>(body.size() * sizeof(mtpPrime))).write(message);
	message += body;

	auto from = message.constData();
	return MTP::details::DumpToText(from, from + message.size());
}

template <typename Value>
QString DumpMessage(const Value &value, int32 truncatePrimes = 0) {
	auto body = mtpBuffer();
	value.template write<mtpBuffer>(body);
	return DumpMessageBody(body, truncatePrimes);
}

template <typename Value>
QString DumpReceivedMessage(const Value &value, int32 truncatePrimes = 0) {
	auto body = mtpBuffer();
	MTP_int(static_cast<int32>(value.type())).write(body);
	value.template write<mtpBuffer>(body);
	return DumpMessageBody(body, truncatePrimes);
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

TEST_CASE(PasswordInputSecretsAreRedactedFromMtpDump) {
	const auto secret = QByteArray(256, char(0xAB));
	using Flag = MTPDaccount_passwordInputSettings::Flag;
	const auto settings = MTP_account_passwordInputSettings(
		MTP_flags(Flag::f_new_password_hash | Flag::f_hint
				  | Flag::f_new_secure_settings),
		MTP_passwordKdfAlgoUnknown(), MTP_bytes(secret),
		MTP_string(u"visible-hint"_q), MTP_string(),
		MTP_secureSecretSettings(
			MTP_securePasswordKdfAlgoSHA512(MTP_bytes(secret)),
			MTP_bytes(secret), MTP_long(123)));
	const auto dump = DumpMessage(MTPaccount_UpdatePasswordSettings(
		MTP_inputCheckPasswordEmpty(), settings));

	CHECK(dump.contains(u"new_password_hash: [REDACTED] [STRING]"_q));
	CHECK(dump.contains(u"new_secure_settings: [REDACTED] [OBJECT]"_q));
	CHECK(!dump.toUpper().contains(u"AB"_q));
	CHECK(!dump.contains(u"secure_algo:"_q));
	CHECK(!dump.contains(u"secure_secret:"_q));
	CHECK(!dump.contains(u"secure_secret_id:"_q));
	CHECK(dump.contains(u"hint: \"visible-hint\" [STRING]"_q));
}

TEST_CASE(ReceivedPasswordSettingsSecureSecretIsRedactedFromMtpDump) {
	const auto secret = QByteArray("received-secure-secret");
	const auto salt = QByteArray("received-secure-salt");
	constexpr auto secureSecretId = 42LL;
	using Flag = MTPDaccount_passwordSettings::Flag;
	const auto dump = DumpReceivedMessage(MTP_account_passwordSettings(
		MTP_flags(Flag::f_email | Flag::f_secure_settings),
		MTP_string(u"safe@example.test"_q),
		MTP_secureSecretSettings(
			MTP_securePasswordKdfAlgoPBKDF2HMACSHA512iter100000(
				MTP_bytes(salt)),
			MTP_bytes(secret), MTP_long(secureSecretId))));

	CHECK(dump.contains(u"secure_settings: [REDACTED] [OBJECT]"_q));
	CHECK(
		!dump.contains(QString::fromLatin1(secret.constData(), secret.size())));
	CHECK(!dump.contains(QString::fromLatin1(salt.constData(), salt.size())));
	CHECK(!dump.contains(QString::number(secureSecretId)));
	CHECK(!dump.contains(u"secure_algo:"_q));
	CHECK(!dump.contains(u"secure_secret:"_q));
	CHECK(!dump.contains(u"secure_secret_id:"_q));
	CHECK(dump.contains(u"email: \"safe@example.test\" [STRING]"_q));
}

TEST_CASE(RedactedPasswordSettingsDecodeErrorDoesNotExposeBytes) {
	const auto secret = QByteArray("decode-error-secret");
	const auto salt = QByteArray("decode-error-salt");
	constexpr auto secureSecretId = 43LL;
	using Flag = MTPDaccount_passwordSettings::Flag;
	const auto dump = DumpReceivedMessage(
		MTP_account_passwordSettings(
			MTP_flags(Flag::f_email | Flag::f_secure_settings),
			MTP_string(u"safe@example.test"_q),
			MTP_secureSecretSettings(
				MTP_securePasswordKdfAlgoPBKDF2HMACSHA512iter100000(
					MTP_bytes(salt)),
				MTP_bytes(secret), MTP_long(secureSecretId))),
		1);

	CHECK(dump.contains(
		u"secure_settings: [ERROR] (could not decode redacted object)"_q));
	CHECK(
		!dump.contains(QString::fromLatin1(secret.constData(), secret.size())));
	CHECK(!dump.contains(QString::fromLatin1(salt.constData(), salt.size())));
	CHECK(!dump.contains(QString::number(secureSecretId)));
}

} // namespace
