/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <vector>

namespace File {
namespace internal {

[[nodiscard]] bool IsDefaultUrlSchemeAllowed(const QString &scheme);
[[nodiscard]] bool
IsUrlSchemeForbiddenForCustomAllowlist(const QString &scheme);
[[nodiscard]] bool
IsCustomUrlSchemeAllowed(const QString &scheme,
						 const std::vector<QString> &allowedProtocols);
[[nodiscard]] bool IsUrlSchemeAllowedForOpen(const QString &scheme,
											 bool customSchemeApproved);
[[nodiscard]] bool SchemeRequiresConfirmation(const QString &scheme,
											  bool allowedForOrigin);

} // namespace internal
} // namespace File
