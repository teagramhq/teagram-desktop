/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QUrl>

namespace Ui::details {

enum class TranslateProviderChoice {
	Unavailable,
	Url,
	Platform,
	MTProto,
};

[[nodiscard]] inline bool HasTranslateUrlTemplate(const QString &urlTemplate) {
	return !urlTemplate.isEmpty() && urlTemplate.contains(u"%q"_q);
}

[[nodiscard]] inline bool
IsSecureTranslateUrlTemplate(const QString &urlTemplate) {
	if (!HasTranslateUrlTemplate(urlTemplate)) {
		return false;
	}
	auto substituted = urlTemplate;
	substituted.replace(u"%q"_q, u"translation"_q);
	substituted.replace(u"%f"_q, u"en"_q);
	substituted.replace(u"%t"_q, u"en"_q);
	const auto url = QUrl(substituted, QUrl::StrictMode);
	return url.isValid()
		   && (url.scheme().compare(u"https"_q, Qt::CaseInsensitive) == 0)
		   && !url.host().isEmpty();
}

[[nodiscard]] inline TranslateProviderChoice
ChooseTranslateProvider(bool serverSupported, bool hasUrlTemplate,
						bool secureUrlTemplate, bool platformAvailable,
						bool usePlatformTranslation) {
	if (serverSupported) {
		if (hasUrlTemplate) {
			return TranslateProviderChoice::Url;
		}
		if (platformAvailable && usePlatformTranslation) {
			return TranslateProviderChoice::Platform;
		}
		return TranslateProviderChoice::MTProto;
	}
	if (hasUrlTemplate && secureUrlTemplate) {
		return TranslateProviderChoice::Url;
	}
	return platformAvailable ? TranslateProviderChoice::Platform
							 : TranslateProviderChoice::Unavailable;
}

} // namespace Ui::details
