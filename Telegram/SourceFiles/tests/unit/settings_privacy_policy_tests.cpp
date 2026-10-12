/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "api/api_user_privacy.h"
#include "settings/settings_privacy_policy.h"

namespace {

using Key = Api::UserPrivacy::Key;
using Settings::details::IsPrivacyExceptionPeerAllowed;
using Settings::details::IsPrivacyKeySupported;
using Settings::details::PrivacyExceptionPeerType;

TEST_CASE(OnlyLastSeenPrivacyKeyIsSupported) {
	CHECK(IsPrivacyKeySupported(Key::LastSeen));
	CHECK(!IsPrivacyKeySupported(Key::PhoneNumber));
	CHECK(!IsPrivacyKeySupported(Key::ProfilePhoto));
	CHECK(!IsPrivacyKeySupported(Key::Forwards));
	CHECK(!IsPrivacyKeySupported(Key::Calls));
	CHECK(!IsPrivacyKeySupported(Key::CallsPeer2Peer));
	CHECK(!IsPrivacyKeySupported(Key::Voices));
	CHECK(!IsPrivacyKeySupported(Key::Birthday));
	CHECK(!IsPrivacyKeySupported(Key::GiftsAutoSave));
	CHECK(!IsPrivacyKeySupported(Key::About));
	CHECK(!IsPrivacyKeySupported(Key::SavedMusic));
	CHECK(!IsPrivacyKeySupported(Key::Invites));
	CHECK(!IsPrivacyKeySupported(Key::AddedByPhone));
	CHECK(!IsPrivacyKeySupported(Key::NoPaidMessages));
}

TEST_CASE(LastSeenExceptionsAllowOnlyUsers) {
	CHECK(IsPrivacyExceptionPeerAllowed(
		Key::LastSeen,
		PrivacyExceptionPeerType::User));
	CHECK(!IsPrivacyExceptionPeerAllowed(
		Key::LastSeen,
		PrivacyExceptionPeerType::Chat));
	CHECK(!IsPrivacyExceptionPeerAllowed(
		Key::LastSeen,
		PrivacyExceptionPeerType::Megagroup));
}

TEST_CASE(OtherPrivacyExceptionsKeepTheirPeerTypes) {
	CHECK(IsPrivacyExceptionPeerAllowed(
		Key::PhoneNumber,
		PrivacyExceptionPeerType::User));
	CHECK(IsPrivacyExceptionPeerAllowed(
		Key::PhoneNumber,
		PrivacyExceptionPeerType::Chat));
	CHECK(IsPrivacyExceptionPeerAllowed(
		Key::PhoneNumber,
		PrivacyExceptionPeerType::Megagroup));
	CHECK(!IsPrivacyExceptionPeerAllowed(
		Key::PhoneNumber,
		PrivacyExceptionPeerType::Other));
}

} // namespace
