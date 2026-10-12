/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_user_privacy.h"

namespace Settings {
namespace details {

enum class PrivacyExceptionPeerType {
	User,
	Chat,
	Megagroup,
	Other,
};

[[nodiscard]] constexpr bool IsPrivacyKeySupported(Api::UserPrivacy::Key key) {
	return key == Api::UserPrivacy::Key::LastSeen;
}

[[nodiscard]] constexpr bool
IsPrivacyExceptionPeerAllowed(Api::UserPrivacy::Key key,
							  PrivacyExceptionPeerType type) {
	switch (type) {
	case PrivacyExceptionPeerType::User:
		return true;
	case PrivacyExceptionPeerType::Chat:
	case PrivacyExceptionPeerType::Megagroup:
		return key != Api::UserPrivacy::Key::LastSeen;
	case PrivacyExceptionPeerType::Other:
		return false;
	}
	return false;
}

} // namespace details
} // namespace Settings
