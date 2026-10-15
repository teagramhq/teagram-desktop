/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_chat_participant_status.h"

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
};

// Story rights have no rows on a session without stories, so the save
// keeps them from `existing` instead of from the checkboxes. `existing`
// must be what the server already holds for the target: an admin's
// current rights, a bot's current rights in the chat, or nothing for a
// new admin. Link-requested and bot-suggested rights never reach it, so
// hidden rows cannot grant anything. The actor limit applies last.
[[nodiscard]] inline ChatAdminRights RightsToSave(const SaveInput &input) {
	const auto hidden
		= input.storiesSupported ? ChatAdminRights() : StoryRights();
	const auto visible = input.checked & ~hidden;
	const auto preserved = input.existing & hidden;
	return (visible | preserved | ChatAdminRight::Other) & input.actorLimit;
}

} // namespace Ui::EditPeer::AdminRights
