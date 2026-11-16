/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Core {

class SingleInstanceLock final {
  public:
	enum class Result { Acquired, Busy, Error };

	explicit SingleInstanceLock(QString path);
	SingleInstanceLock(const SingleInstanceLock &) = delete;
	SingleInstanceLock &operator=(const SingleInstanceLock &) = delete;
	~SingleInstanceLock();

	[[nodiscard]] Result tryAcquire();
	[[nodiscard]] bool isAcquired() const;
	[[nodiscard]] QString errorString() const;
	void release();
#ifdef TDESKTOP_UNIT_TESTS
	static void SetAfterOpenForTests(void (*callback)());
#endif // TDESKTOP_UNIT_TESTS

  private:
	void setSystemError(const char *operation, int code);
	const QString _path;
	QString _error;
#ifdef Q_OS_WIN
	void *_handle = nullptr;
#else  // Q_OS_WIN
	int _descriptor = -1;
#endif // Q_OS_WIN
};

} // namespace Core
