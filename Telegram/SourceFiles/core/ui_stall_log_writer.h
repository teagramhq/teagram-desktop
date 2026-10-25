/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

class QFile;

namespace Core {

class UiStallLogWriter final {
  public:
	[[nodiscard]] bool writeDetection(QFile &file, const QString &prefix,
									  const QString &message);
	[[nodiscard]] QString fileName() const;
	[[nodiscard]] bool writeCompletion(QFile &file, const QString &message);
	void reset();

  private:
	struct Pending final {
		QString fileName;
		QByteArray prefix;
		qint64 offset = 0;
		qint64 size = 0;
	};

	std::optional<Pending> _pending;
};

} // namespace Core
