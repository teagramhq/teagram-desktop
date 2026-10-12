/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QThread>

#include <utility>

namespace Core {

template <typename Cleanup>
void ShutdownTeagramUpdateWorker(QThread *worker, QObject *receiver,
								 Cleanup &&cleanup) {
	while (worker && worker->isRunning()) {
		QCoreApplication::sendPostedEvents(receiver, QEvent::MetaCall);
		worker->wait(1);
	}
	if (worker) {
		worker->wait();
	}
	QCoreApplication::sendPostedEvents(receiver, QEvent::MetaCall);
	std::forward<Cleanup>(cleanup)();
}

} // namespace Core
