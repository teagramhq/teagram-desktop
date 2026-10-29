/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_signup_invite_validation.h"

namespace {

using Intro::details::NormalizeInviteSecret;

const auto kInviteSecret
	= u"3f9a6c0e5b2d41a7c8e9f0b1d2c3a4b5e6f708192a3b4c5d6e7f8091a2b3c4d5"_q;
const auto kUpperInviteSecret
	= u"3F9A6C0E5B2D41A7C8E9F0B1D2C3A4B5E6F708192A3B4C5D6E7F8091A2B3C4D5"_q;

TEST_CASE(InviteSecretAcceptsCodeAndLinkFragment) {
	CHECK_EQ(NormalizeInviteSecret(kInviteSecret), kInviteSecret);
	CHECK_EQ(NormalizeInviteSecret(kUpperInviteSecret), kInviteSecret);
	CHECK_EQ(
		NormalizeInviteSecret(
			u" https://links.example.test/invite# 3F9A6C0E5B2D41A7C8E9F0B1D2C3A4B5E6F708192A3B4C5D6E7F8091A2B3C4D5 \n"_q),
		kInviteSecret);
}

TEST_CASE(InviteSecretRejectsIncompleteOrMalformedInput) {
	CHECK(NormalizeInviteSecret(QString()).isEmpty());
	CHECK(NormalizeInviteSecret(kInviteSecret.left(63)).isEmpty());
	CHECK(NormalizeInviteSecret(kInviteSecret + u"g"_q).isEmpty());
	CHECK(NormalizeInviteSecret(
		u"https://links.example.test/invite"_q).isEmpty());
	CHECK(NormalizeInviteSecret(u"https://links.example.test/invite#"_q).isEmpty());
}

} // namespace
