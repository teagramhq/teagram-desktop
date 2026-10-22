/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

#include <QtCore/QString>

#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Data::details {

enum class BasicChatRole {
	Member,
	Admin,
	Creator,
};

[[nodiscard]] inline constexpr bool CanAddBasicChatAdmins(bool isMember,
														  bool isCreator) {
	return isMember && isCreator;
}

struct BasicChatAdminCapabilities {
	bool canEditRights = true;
	bool canSetRank = true;
	bool canTransferOwnership = true;
};

[[nodiscard]] inline constexpr BasicChatAdminCapabilities
BasicChatAdminCapabilitiesFor(bool customServer) {
	return {
		!customServer,
		!customServer,
		!customServer,
	};
}

enum class ChatPhotoPeerKind {
	BasicGroup,
	Supergroup,
	Channel,
};

[[nodiscard]] inline constexpr ChatPhotoPeerKind
ChatPhotoPeerKindForChannel(bool isMegagroup) {
	return isMegagroup ? ChatPhotoPeerKind::Supergroup
					   : ChatPhotoPeerKind::Channel;
}

[[nodiscard]] inline constexpr bool
ChatPhotoSupportedFor(bool customServer, ChatPhotoPeerKind kind) {
	return !customServer || (kind == ChatPhotoPeerKind::BasicGroup);
}

// Custom servers support basic group photos; the other controls below remain
// unavailable there.
struct BasicChatControls {
	bool canEditPhoto = true;
	bool canEditType = true;
	bool canEditHistoryVisibility = true;
	bool canRestrictMembers = true;
	bool canEditMemberTags = true;
};

[[nodiscard]] inline constexpr BasicChatControls
BasicChatControlsFor(bool customServer) {
	return {
		.canEditPhoto
		= ChatPhotoSupportedFor(customServer, ChatPhotoPeerKind::BasicGroup),
		.canEditType = !customServer,
		.canEditHistoryVisibility = !customServer,
		.canRestrictMembers = !customServer,
		.canEditMemberTags = !customServer,
	};
}

template <typename Chat>
[[nodiscard]] inline BasicChatControls BasicChatControlsOf(const Chat &chat) {
	return BasicChatControlsFor(chat->usesCustomServer());
}

[[nodiscard]] inline constexpr bool
CanEditChatPhotoFor(bool customServer, ChatPhotoPeerKind kind,
					bool canEditInformation, bool newlyCreatedBySelf = false) {
	return ChatPhotoSupportedFor(customServer, kind)
		   && (canEditInformation || newlyCreatedBySelf);
}

[[nodiscard]] inline constexpr bool
CanRemoveChatPhotoFor(bool customServer, ChatPhotoPeerKind kind, bool hasPhoto,
					  bool canEditInformation) {
	return hasPhoto
		   && CanEditChatPhotoFor(customServer, kind, canEditInformation);
}

template <typename Peer>
[[nodiscard]] inline bool SupportsMemberRestrictions(const Peer &peer) {
	const auto chat = peer->asChat();
	return !chat || BasicChatControlsOf(chat).canRestrictMembers;
}

template <typename Rights>
[[nodiscard]] inline constexpr bool
BasicChatAdminRoleForSave(Rights selectedRights) {
	return selectedRights != Rights();
}

template <typename Rights>
[[nodiscard]] inline constexpr Rights
BasicChatAdminDefaultRightsFor(bool customServer, Rights defaultRights,
							   Rights roleOnlyRights) {
	return customServer ? roleOnlyRights : defaultRights;
}

template <typename Rights>
[[nodiscard]] inline constexpr Rights
BasicChatAdminRightsFor(bool isAdmin, bool customServer, Rights defaultRights) {
	return (isAdmin && !customServer) ? defaultRights : Rights();
}

[[nodiscard]] inline constexpr bool CanManageBasicChatCall(BasicChatRole role,
														   bool customServer) {
	return (role == BasicChatRole::Creator)
		   || (!customServer && (role == BasicChatRole::Admin));
}

struct ChatParticipantInfo {
	UserId userId;
	UserId inviterId;
	bool creator = false;
	bool admin = false;
	QString rank;
};

template <typename User>
struct ResolvedChatParticipant {
	ChatParticipantInfo participant;
	User user;
};

template <typename Resolve>
[[nodiscard]] auto ResolveChatParticipants(
		const std::vector<ChatParticipantInfo> &participants,
		Resolve &&resolve)
-> std::optional<std::vector<ResolvedChatParticipant<
	typename std::invoke_result_t<Resolve &, UserId>::value_type>>> {
	using User = typename std::invoke_result_t<Resolve &, UserId>::value_type;
	auto result = std::vector<ResolvedChatParticipant<User>>();
	result.reserve(participants.size());
	for (const auto &participant : participants) {
		auto user = resolve(participant.userId);
		if (!user) {
			return std::nullopt;
		}
		result.push_back({ participant, std::move(*user) });
	}
	return result;
}

} // namespace Data::details
