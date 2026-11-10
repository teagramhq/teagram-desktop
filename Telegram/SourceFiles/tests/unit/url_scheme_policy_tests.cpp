/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/url_scheme_policy.h"

#include <array>
#include <vector>

TEST_CASE(DefaultUrlSchemePolicyAllowsTeagramSchemes) {
	for (const auto &scheme : std::array{
			 u"http"_q,
			 u"https"_q,
			 u"HTTPS"_q,
			 u"ftp"_q,
			 u"itmss"_q,
			 u"mailto"_q,
			 u"tel"_q,
			 u"tg"_q,
			 u"ton"_q,
			 u"tonsite"_q,
		 }) {
		CHECK(File::internal::IsDefaultUrlSchemeAllowed(scheme));
	}
}

TEST_CASE(DefaultUrlSchemePolicyRejectsUnsafeAndUnknownSchemes) {
	for (const auto &scheme : std::array{
			 u"file"_q,
			 u"FILE"_q,
			 u"webdavs"_q,
			 u"WEBDAVS"_q,
			 u"c"_q,
			 u"tc"_q,
			 u"smb"_q,
		 }) {
		CHECK(!File::internal::IsDefaultUrlSchemeAllowed(scheme));
	}
}

TEST_CASE(CustomUrlSchemeAllowlistCannotEnableForbiddenSchemes) {
	for (const auto &scheme : std::array{
			 u"file"_q,
			 u"FILE"_q,
			 u"webdavs"_q,
			 u"WEBDAVS"_q,
			 u"c"_q,
			 u"tc"_q,
			 u"TC"_q,
		 }) {
		CHECK(File::internal::IsUrlSchemeForbiddenForCustomAllowlist(scheme));
	}
	CHECK(!File::internal::IsUrlSchemeForbiddenForCustomAllowlist(u"smb"_q));
}

TEST_CASE(CustomUrlSchemePolicyUsesOnlyTheSuppliedAccountProtocols) {
	const auto firstAccount = std::vector<QString>{u"first-app"_q};
	const auto secondAccount = std::vector<QString>{u"second-app"_q};
	CHECK(
		File::internal::IsCustomUrlSchemeAllowed(u"first-app"_q, firstAccount));
	CHECK(!File::internal::IsCustomUrlSchemeAllowed(u"second-app"_q,
													firstAccount));
	CHECK(File::internal::IsCustomUrlSchemeAllowed(u"second-app"_q,
												   secondAccount));
	CHECK(File::internal::SchemeRequiresConfirmation(u"first-app"_q, false));
	CHECK(!File::internal::SchemeRequiresConfirmation(u"first-app"_q, true));
	CHECK(!File::internal::IsUrlSchemeAllowedForOpen(u"first-app"_q, false));
	CHECK(File::internal::IsUrlSchemeAllowedForOpen(u"first-app"_q, true));
}

TEST_CASE(TonConnectSchemeIsNotDefaultOrCustomAllowedAndNeedsConfirmation) {
	const auto tc = u"tc"_q;
	const auto allowed = File::internal::IsDefaultUrlSchemeAllowed(tc)
						 || File::internal::IsCustomUrlSchemeAllowed(tc, {tc});
	CHECK(!allowed);
	CHECK(!File::internal::IsUrlSchemeAllowedForOpen(tc, true));
	CHECK(File::internal::SchemeRequiresConfirmation(tc, allowed));
}
