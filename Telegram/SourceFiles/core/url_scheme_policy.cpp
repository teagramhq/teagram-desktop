/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/url_scheme_policy.h"

#include "base/basic_types.h"

#include <array>

namespace File {
namespace internal {
namespace {

const auto kDefaultUrlSchemes = std::array{
	u"http"_q, u"https"_q, u"ftp"_q, u"itmss"_q, u"mailto"_q,
	u"tel"_q,  u"tg"_q,	   u"tc"_q,	 u"ton"_q,	 u"tonsite"_q,
};

} // namespace

bool IsDefaultUrlSchemeAllowed(const QString &scheme) {
	for (const auto &allowed : kDefaultUrlSchemes) {
		if (scheme.compare(allowed, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

bool IsUrlSchemeForbiddenForCustomAllowlist(const QString &scheme) {
	return (scheme.size() <= 1)
		   || (scheme.compare(u"file"_q, Qt::CaseInsensitive) == 0)
		   || (scheme.compare(u"webdavs"_q, Qt::CaseInsensitive) == 0);
}

} // namespace internal
} // namespace File
