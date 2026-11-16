/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/single_instance_lock.h"

#include <QtCore/QFile>
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

SingleInstanceLock::Result SingleInstanceLock::tryAcquire() {
	if (isAcquired()) {
		return Result::Acquired;
	}
#ifdef Q_OS_WIN
	const auto file = CreateFileW(
		reinterpret_cast<const wchar_t *>(_path.utf16()),
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return (GetLastError() == ERROR_SHARING_VIOLATION) ? Result::Busy
														   : Result::Error;
	}
#ifdef TDESKTOP_UNIT_TESTS
	if (AfterOpenForTests) {
		AfterOpenForTests();
	}
#endif // TDESKTOP_UNIT_TESTS
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(file, &info)
		|| (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		|| GetFileType(file) != FILE_TYPE_DISK) {
		CloseHandle(file);
		return Result::Error;
	}
	auto overlapped = OVERLAPPED();
	if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
					0, 1, 0, &overlapped)) {
		const auto error = GetLastError();
		CloseHandle(file);
		return (error == ERROR_LOCK_VIOLATION) ? Result::Busy : Result::Error;
	}
	_handle = file;
#else // Q_OS_WIN
	const auto path = QFile::encodeName(_path);
	const auto file = ::open(path.constData(),
							 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (file < 0) {
		return Result::Error;
	}
#ifdef TDESKTOP_UNIT_TESTS
	if (AfterOpenForTests) {
		AfterOpenForTests();
	}
#endif // TDESKTOP_UNIT_TESTS
	struct stat info = {};
	if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
		::close(file);
		return Result::Error;
	}
	if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
		const auto error = errno;
		::close(file);
		return (error == EWOULDBLOCK || error == EAGAIN) ? Result::Busy
														 : Result::Error;
	}
	_descriptor = file;
#endif // Q_OS_WIN
	return Result::Acquired;
}

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
