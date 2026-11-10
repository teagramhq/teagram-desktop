/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace File {
namespace internal {

[[nodiscard]] bool IsDefaultUrlSchemeAllowed(const QString &scheme);
[[nodiscard]] bool
IsUrlSchemeForbiddenForCustomAllowlist(const QString &scheme);

} // namespace internal
} // namespace File
