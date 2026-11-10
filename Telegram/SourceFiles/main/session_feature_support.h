#pragma once

#include "mtproto/mtproto_dc_options.h"

#include <optional>

namespace Main::details {

[[nodiscard]] inline bool callsSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool callLinkAllowed(const MTP::DcOptions &options) {
	return callsSupported(options);
}

[[nodiscard]] inline bool botAppsSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool paidFeaturesSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool storiesSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool exportSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool passportSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool aiComposeSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool serverTranslationSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool
cloudResourcesSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

class CloudResourceGrant final {
  public:
	CloudResourceGrant(const CloudResourceGrant &) = default;
	CloudResourceGrant(CloudResourceGrant &&) = default;
	CloudResourceGrant &operator=(const CloudResourceGrant &) = default;
	CloudResourceGrant &operator=(CloudResourceGrant &&) = default;

  private:
	CloudResourceGrant() = default;

	friend std::optional<CloudResourceGrant>
	cloudResourceGrant(const MTP::DcOptions &options);
};

[[nodiscard]] inline std::optional<CloudResourceGrant>
cloudResourceGrant(const MTP::DcOptions &options) {
	return cloudResourcesSupported(options)
			   ? std::optional<CloudResourceGrant>(CloudResourceGrant())
			   : std::nullopt;
}

[[nodiscard]] inline bool
dictionaryManifestAccessSupported(const MTP::DcOptions &options) {
	return cloudResourcesSupported(options);
}

[[nodiscard]] inline bool
cloudEmojiSetsListedSupported(const MTP::DcOptions &options) {
	return cloudResourcesSupported(options);
}

[[nodiscard]] inline bool
dictionaryManagerSaveAllowed(const MTP::DcOptions &options, bool rowsBuilt) {
	return rowsBuilt && cloudResourceGrant(options).has_value();
}

[[nodiscard]] inline bool
advancedManageDictionariesMenuItemSupported(const MTP::DcOptions &options) {
	return cloudResourcesSupported(options);
}

[[nodiscard]] inline bool
composerManageDictionariesMenuItemSupported(const MTP::DcOptions &options) {
	return cloudResourcesSupported(options);
}

[[nodiscard]] inline bool
automaticChatTranslationEnabled(bool serverSupported, bool settingEnabled,
								bool premium, bool channelAutoTranslation) {
	return serverSupported && settingEnabled
		   && (premium || channelAutoTranslation);
}

[[nodiscard]] inline bool canEditChannelAutoTranslation(bool serverSupported,
														bool canEdit) {
	return serverSupported && canEdit;
}

[[nodiscard]] inline bool shouldSaveChannelAutoTranslation(bool serverSupported,
														   bool hasSavedValue) {
	return serverSupported && hasSavedValue;
}

[[nodiscard]] inline bool
sharedFoldersSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool
accountBioEditSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool
accountUsernameEditSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool noForwardsSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool
chatThemeEditingSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

[[nodiscard]] inline bool
scheduledMessagesSupported(const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
}

template <typename Peer>
[[nodiscard]] inline bool noForwardsSupportedForPeer(const Peer &peer) {
	return noForwardsSupported(peer->session().mtp().dcOptions());
}

template <typename Peer>
[[nodiscard]] inline bool chatThemeEditingSupportedForPeer(const Peer &peer) {
	return chatThemeEditingSupported(peer->session().mtp().dcOptions());
}

template <typename Peer>
[[nodiscard]] inline bool scheduledMessagesSupportedForPeer(const Peer &peer) {
	return scheduledMessagesSupported(peer->session().mtp().dcOptions());
}

template <typename Peer>
[[nodiscard]] inline bool scheduledSendAllowedForPeer(const Peer &peer,
													  bool scheduled) {
	return !scheduled || scheduledMessagesSupportedForPeer(peer);
}

template <typename Callback>
inline void
refreshAccountUsernameCacheIfSupported(const MTP::DcOptions &options,
									   Callback &&refresh) {
	if (accountUsernameEditSupported(options)) {
		refresh();
	}
}

template <typename User>
[[nodiscard]] inline bool accountBioEditorTargetSupported(const User &self) {
	return accountBioEditSupported(self.session().mtp().dcOptions());
}

template <typename Session>
[[nodiscard]] inline bool
accountBioSearchTargetSupported(const Session &session) {
	return accountBioEditSupported(session.mtp().dcOptions());
}

} // namespace Main::details
