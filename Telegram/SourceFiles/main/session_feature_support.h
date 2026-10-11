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

struct AccountBioTargets {
	bool editor = false;
	bool search = false;
};

[[nodiscard]] inline AccountBioTargets accountBioTargets(
	const MTP::DcOptions &options) {
	const auto supported = accountBioEditSupported(options);
	return {
		.editor = supported,
		.search = supported,
	};
}

} // namespace Main::details
