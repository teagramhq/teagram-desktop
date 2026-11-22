/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/single_instance_lock.h"

#include <QtCore/QFile>
#include <system_error>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#else // Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // Q_OS_WIN

namespace Core {
#ifdef TDESKTOP_UNIT_TESTS
namespace {
void (*AfterOpenForTests)() = nullptr;
} // namespace

void SingleInstanceLock::SetAfterOpenForTests(void (*callback)()) {
	AfterOpenForTests = callback;
}
#endif // TDESKTOP_UNIT_TESTS

SingleInstanceLock::SingleInstanceLock(QString path) : _path(std::move(path)) {}

SingleInstanceLock::~SingleInstanceLock() { release(); }

void SingleInstanceLock::setSystemError(const char *operation, int code) {
	_error
		= QStringLiteral("%1 '%2': %3 (error %4)")
			  .arg(QString::fromLatin1(operation), _path,
				   QString::fromStdString(std::system_category().message(code)))
			  .arg(code);
}

SingleInstanceLock::Result SingleInstanceLock::tryAcquire() {
	if (isAcquired()) {
		return Result::Acquired;
	}
	_error.clear();
#ifdef Q_OS_WIN
	const auto file = CreateFileW(
		reinterpret_cast<const wchar_t *>(_path.utf16()),
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		if (error == ERROR_SHARING_VIOLATION) {
			return Result::Busy;
		}
		setSystemError("open", int(error));
		return Result::Error;
	}
#ifdef TDESKTOP_UNIT_TESTS
	if (AfterOpenForTests) {
		AfterOpenForTests();
	}
#endif // TDESKTOP_UNIT_TESTS
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(file, &info)) {
		const auto error = GetLastError();
		CloseHandle(file);
		setSystemError("inspect", int(error));
		return Result::Error;
	}
	if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		|| GetFileType(file) != FILE_TYPE_DISK) {
		CloseHandle(file);
		_error
			= QStringLiteral("lock path is not a regular file: %1").arg(_path);
		return Result::Error;
	}
	auto overlapped = OVERLAPPED();
	if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
					0, 1, 0, &overlapped)) {
		const auto error = GetLastError();
		CloseHandle(file);
		if (error == ERROR_LOCK_VIOLATION) {
			return Result::Busy;
		}
		setSystemError("lock", int(error));
		return Result::Error;
	}
	_handle = file;
#else // Q_OS_WIN
	const auto path = QFile::encodeName(_path);
	const auto file = ::open(path.constData(),
							 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (file < 0) {
		setSystemError("open", errno);
		return Result::Error;
	}
#ifdef TDESKTOP_UNIT_TESTS
	if (AfterOpenForTests) {
		AfterOpenForTests();
	}
#endif // TDESKTOP_UNIT_TESTS
	struct stat info = {};
	if (::fstat(file, &info) != 0) {
		const auto error = errno;
		::close(file);
		setSystemError("inspect", error);
		return Result::Error;
	}
	if (!S_ISREG(info.st_mode)) {
		::close(file);
		_error
			= QStringLiteral("lock path is not a regular file: %1").arg(_path);
		return Result::Error;
	}
#ifdef Q_OS_LINUX
	if ((info.st_uid != geteuid()) || (info.st_mode & 077)) {
		::close(file);
		_error
			= QStringLiteral("lock path is not a private user-owned file: %1")
				  .arg(_path);
		return Result::Error;
	}
#endif // Q_OS_LINUX
	if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
		const auto error = errno;
		::close(file);
		if (error == EWOULDBLOCK || error == EAGAIN) {
			return Result::Busy;
		}
		setSystemError("lock", error);
		return Result::Error;
	}
#ifdef Q_OS_LINUX
	if (::ftruncate(file, 0) != 0) {
		const auto error = errno;
		::flock(file, LOCK_UN);
		::close(file);
		setSystemError("clear", error);
		return Result::Error;
	}
#endif // Q_OS_LINUX
	_descriptor = file;
#endif // Q_OS_WIN
	return Result::Acquired;
}

#ifdef Q_OS_LINUX
bool SingleInstanceLock::setContents(const QString &contents) {
	if (!isAcquired()) {
		_error = QStringLiteral(
					 "lock must be acquired before writing contents: %1")
					 .arg(_path);
		return false;
	}
	const auto encoded = QFile::encodeName(contents);
	if (encoded.size() > 4095) {
		_error = QStringLiteral("lock contents are too long: %1").arg(_path);
		return false;
	}
	if (::ftruncate(_descriptor, 0) != 0) {
		setSystemError("truncate", errno);
		return false;
	}
	auto offset = 0;
	while (offset < encoded.size()) {
		const auto written
			= ::pwrite(_descriptor, encoded.constData() + offset,
					   size_t(encoded.size() - offset), off_t(offset));
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			setSystemError("write", errno);
			return false;
		}
		if (written == 0) {
			setSystemError("write", EIO);
			return false;
		}
		offset += int(written);
	}
	return true;
}

QString SingleInstanceLock::contents() const {
	const auto path = QFile::encodeName(_path);
	const auto file
		= ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (file < 0) {
		return QString();
	}
	struct stat info = {};
	if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)
		|| (info.st_uid != geteuid()) || (info.st_mode & 077)
		|| (info.st_size < 0) || (info.st_size > 4095)) {
		::close(file);
		return QString();
	}
	auto data = QByteArray(4096, Qt::Uninitialized);
	auto length = ssize_t(0);
	while (length < data.size()) {
		const auto read
			= ::read(file, data.data() + length, size_t(data.size() - length));
		if (read < 0) {
			if (errno == EINTR) {
				continue;
			}
			::close(file);
			return QString();
		}
		if (read == 0) {
			break;
		}
		length += read;
	}
	::close(file);
	if (length >= data.size()) {
		return QString();
	}
	data.resize(int(length));
	return QFile::decodeName(data);
}
#endif // Q_OS_LINUX

QString SingleInstanceLock::errorString() const { return _error; }

bool SingleInstanceLock::isAcquired() const {
#ifdef Q_OS_WIN
	return _handle != nullptr;
#else  // Q_OS_WIN
	return _descriptor >= 0;
#endif // Q_OS_WIN
}

void SingleInstanceLock::release() {
	// Keep the file in place: unlinking it would allow a second inode to be
	// locked while another process still holds the first one open.
#ifdef Q_OS_WIN
	if (_handle) {
		CloseHandle(_handle);
		_handle = nullptr;
	}
#else  // Q_OS_WIN
	if (_descriptor >= 0) {
		::close(_descriptor);
		_descriptor = -1;
	}
#endif // Q_OS_WIN
}

} // namespace Core
