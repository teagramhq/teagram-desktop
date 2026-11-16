/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QLockFile>

namespace Core {

class SingleInstanceLock final {
  public:
	enum class Result { Acquired, Busy, Error };

	explicit SingleInstanceLock(const QString &path) : _lock(path) {
		_lock.setStaleLockTime(0);
	}

	[[nodiscard]] Result tryAcquire() {
		if (_lock.isLocked() || _lock.tryLock()) {
			return Result::Acquired;
		}
		return (_lock.error() == QLockFile::LockFailedError) ? Result::Busy
															 : Result::Error;
	}
	[[nodiscard]] bool isAcquired() const { return _lock.isLocked(); }
	void release() { _lock.unlock(); }

  private:
	QLockFile _lock;
};

} // namespace Core
