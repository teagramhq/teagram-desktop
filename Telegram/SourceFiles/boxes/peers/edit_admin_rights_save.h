/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_chat_participant_status.h"

#include <QtCore/QString>

namespace Ui::EditPeer::AdminRights {

[[nodiscard]] inline ChatAdminRights StoryRights() {
	return ChatAdminRight::PostStories | ChatAdminRight::EditStories
		   | ChatAdminRight::DeleteStories;
}

struct SaveInput final {
	ChatAdminRights checked;
	ChatAdminRights existing;
	ChatAdminRights actorLimit = ~ChatAdminRights(0);
	bool storiesSupported = true;
	bool callsSupported = true;
};

// Rights without editor rows are kept from `existing` instead of from the
// checkboxes. `existing` must be what the server already holds for the
// target: an admin's current rights, a bot's current rights in the chat,
// or nothing for a new admin. Link-requested and bot-suggested rights
// never reach it, so hidden rows cannot grant anything. The actor limit
// applies last.
[[nodiscard]] inline ChatAdminRights RightsToSave(const SaveInput &input) {
	auto hidden = ChatAdminRights();
	if (!input.storiesSupported) {
		hidden |= StoryRights();
	}
	if (!input.callsSupported) {
		hidden |= ChatAdminRight::ManageCall;
	}
	const auto visible = input.checked & ~hidden;
	const auto preserved = input.existing & hidden;
	return (visible | preserved | ChatAdminRight::Other) & input.actorLimit;
}

// A bot promotion has to know the bot's current rights before saving
// whenever hidden story rights could be lost, including when the offered
// rights come from the bot's defaults rather than an admin link.
[[nodiscard]] inline bool NeedsExistingBotRights(bool requestedAdmin,
												 ChatAdminRights offered,
												 bool storiesSupported) {
	return requestedAdmin || (!storiesSupported && offered != 0);
}

// Only a bot that is not in the chat has nothing to preserve. Any other
// lookup failure leaves its hidden story rights unknown, and saving
// would clear them, so the editor must not open.
[[nodiscard]] inline bool
CanEditAfterExistingRightsFailure(const QString &errorType,
								  bool storiesSupported) {
	return storiesSupported || (errorType == u"USER_NOT_PARTICIPANT"_q);
}

} // namespace Ui::EditPeer::AdminRights
