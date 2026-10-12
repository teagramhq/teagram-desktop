#pragma once

#include "mtproto/mtproto_dc_options.h"

namespace Main::details {

[[nodiscard]] inline bool callsSupported(
		const MTP::DcOptions &options) {
	return !(options.hasCustomServer() || options.blocked());
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
