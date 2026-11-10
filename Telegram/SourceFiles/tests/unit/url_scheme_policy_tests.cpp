/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/url_scheme_policy.h"

#include <array>

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
			 u"tc"_q,
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
		 }) {
		CHECK(File::internal::IsUrlSchemeForbiddenForCustomAllowlist(scheme));
	}
	CHECK(!File::internal::IsUrlSchemeForbiddenForCustomAllowlist(u"smb"_q));
}
