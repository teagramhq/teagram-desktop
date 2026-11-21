/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/specific_linux.h"
#include "platform/linux/desktop_identity.h"
#include "platform/linux/local_socket_path.h"

#include "base/logs.h"

#include <QtCore/QCoreApplication>

#include <ksandbox.h>

#include <unistd.h>

namespace Platform {

QString SingleInstanceLocalServerName(const QString &hash,
									  QString *errorMessage) {
	const auto snap = KSandbox::isSnap();
	const auto serverName = Platform::Linux::SingleInstanceLocalServerName(
		hash, QCoreApplication::applicationName(), snap);
	auto failure = Platform::Linux::SingleInstanceSocketPathFailure::None;
	auto failureDetail = QString();
	auto selectionWarning = QString();
	const auto result = Platform::Linux::SingleInstanceSocketPath(
		qEnvironmentVariable("XDG_RUNTIME_DIR"), serverName, geteuid(),
		Platform::Linux::SingleInstanceSocketPathDetails::UserHomeDirectory(
			geteuid(), qEnvironmentVariable("HOME")),
		&failure, snap, &failureDetail, &selectionWarning);
	if (!selectionWarning.isEmpty()) {
		LOG((selectionWarning));
	}
	if (errorMessage) {
		*errorMessage = Platform::Linux::SingleInstanceSocketPathFailureMessage(
			failure, failureDetail);
	}
	return result;
}

} // namespace Platform
