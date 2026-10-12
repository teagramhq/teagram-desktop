/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_replacement.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined Q_OS_MAC
#include <stdio.h>
#endif // Q_OS_MAC

#include <cerrno>
#include <cstring>
#include <utility>

namespace Core {
namespace {

struct FileDescriptor final {
	FileDescriptor() = default;
	explicit FileDescriptor(int value) : value(value) {}
	~FileDescriptor() {
		if (value >= 0) {
			::close(value);
		}
	}
	FileDescriptor(FileDescriptor &&other) noexcept
		: value(std::exchange(other.value, -1)) {}
	FileDescriptor &operator=(FileDescriptor &&other) noexcept {
		if (this != &other) {
			if (value >= 0) {
				::close(value);
			}
			value = std::exchange(other.value, -1);
		}
		return *this;
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;

	int value = -1;
};

struct ResolvedPath {
	QString parent;
	QByteArray name;
	QString full;
};

[[nodiscard]] QString CanonicalDirectory(const QString &path) {
	const auto info = QFileInfo(path);
	const auto canonical = info.canonicalFilePath();
	if (!info.isDir() || info.isSymLink() || canonical.isEmpty()
		|| QDir::cleanPath(canonical)
			   != QDir::cleanPath(info.absoluteFilePath())) {
		return {};
	}
	return QDir::cleanPath(canonical);
}

[[nodiscard]] ResolvedPath ResolvePath(const QString &path) {
	const auto info = QFileInfo(path);
	const auto parent = CanonicalDirectory(info.absolutePath());
	const auto name = QFile::encodeName(info.fileName());
	if (parent.isEmpty() || name.isEmpty() || name.contains('/')) {
		return {};
	}
	return {parent, name, QDir(parent).filePath(info.fileName())};
}

[[nodiscard]] FileDescriptor OpenDirectory(const QString &path,
										   bool requirePrivate) {
	const auto encoded = QFile::encodeName(path);
	if (encoded.isEmpty()) {
		return {};
	}
	const auto descriptor = ::open(
		encoded.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0) {
		return {};
	}
	struct stat info = {};
	if (::fstat(descriptor, &info) != 0 || !S_ISDIR(info.st_mode)
		|| info.st_uid != ::geteuid()
		|| (requirePrivate && (info.st_mode & 0077) != 0)) {
		::close(descriptor);
		return {};
	}
	return FileDescriptor(descriptor);
}

[[nodiscard]] bool IsOwnedDirectoryAt(int parent, const QByteArray &name,
									  dev_t *device) {
	struct stat info = {};
	if (::fstatat(parent, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0
		|| !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
		return false;
	}
	if (device) {
		*device = info.st_dev;
	}
	return true;
}

[[nodiscard]] bool RenameNoReplace(int sourceParent, const QByteArray &source,
								   int targetParent, const QByteArray &target) {
#if defined Q_OS_MAC
	return ::renameatx_np(sourceParent, source.constData(), targetParent,
						  target.constData(), RENAME_EXCL)
		   == 0;
#else  // Q_OS_MAC
	struct stat info = {};
	if (::fstatat(targetParent, target.constData(), &info, AT_SYMLINK_NOFOLLOW)
		== 0) {
		errno = EEXIST;
		return false;
	}
	if (errno != ENOENT) {
		return false;
	}
	return ::renameat(sourceParent, source.constData(), targetParent,
					  target.constData())
		   == 0;
#endif // Q_OS_MAC
}

[[nodiscard]] bool RemoveTreeAt(int parent, const char *name, int depth = 0) {
	if (depth > 256) {
		return false;
	}
	struct stat info = {};
	if (::fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT;
	}
	if (!S_ISDIR(info.st_mode)) {
		return ::unlinkat(parent, name, 0) == 0;
	}
	const auto opened = ::openat(
		parent, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (opened < 0) {
		return false;
	}
	auto directory = FileDescriptor(opened);
	auto duplicate = ::dup(opened);
	if (duplicate < 0) {
		return false;
	}
	auto stream = ::fdopendir(duplicate);
	if (!stream) {
		::close(duplicate);
		return false;
	}
	auto removed = true;
	while (const auto entry = ::readdir(stream)) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		if (!RemoveTreeAt(opened, entry->d_name, depth + 1)) {
			removed = false;
			break;
		}
	}
	::closedir(stream);
	return removed && ::unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

[[nodiscard]] bool InspectAt(int parent, const QByteArray &name,
							 struct stat *info, bool *exists) {
	if (!info || !exists) {
		return false;
	}
	if (::fstatat(parent, name.constData(), info, AT_SYMLINK_NOFOLLOW) == 0) {
		*exists = true;
		return true;
	}
	if (errno == ENOENT) {
		*exists = false;
		return true;
	}
	return false;
}

#if defined Q_OS_MAC
[[nodiscard]] bool SwapPaths(int firstParent, const QByteArray &first,
							 int secondParent, const QByteArray &second) {
	return ::renameatx_np(firstParent, first.constData(), secondParent,
						  second.constData(), RENAME_SWAP)
		   == 0;
}
#endif // Q_OS_MAC

[[nodiscard]] TeagramUpdateReplacementResult
Result(TeagramUpdateReplacementReason reason,
	   const QString &previousPath = {},
	   bool committed = false) {
	return {reason, previousPath, committed};
}

#ifdef TDESKTOP_UNIT_TESTS
[[nodiscard]] bool
Injected(const TeagramUpdateReplacementFailureInjector &failureInjector,
		 TeagramUpdateReplacementFailurePoint point) {
	return failureInjector && failureInjector(point);
}
#endif // TDESKTOP_UNIT_TESTS

} // namespace

bool WriteTeagramUpdateMarker(const QString &parent, const char *name,
							  quint64 build) {
	if (!name || !*name || strchr(name, '/') || !strcmp(name, ".")
		|| !strcmp(name, "..")) {
		return false;
	}
	auto directory = OpenDirectory(parent, false);
	if (directory.value < 0) {
		return false;
	}
	const auto markerName = QByteArray(name);
	struct stat existing = {};
	if (::fstatat(directory.value, markerName.constData(), &existing,
				  AT_SYMLINK_NOFOLLOW)
		== 0) {
		if (!S_ISREG(existing.st_mode) || existing.st_uid != ::geteuid()
			|| existing.st_nlink != 1 || (existing.st_mode & 0077) != 0) {
			return false;
		}
	} else if (errno != ENOENT) {
		return false;
	}
	const auto temporaryName
		= QByteArrayLiteral(".teagram-marker-")
		  + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
	const auto descriptor
		= ::openat(directory.value, temporaryName.constData(),
				   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (descriptor < 0) {
		return false;
	}
	struct stat temporary = {};
	const auto text = QByteArray::number(build) + '\n';
	auto success = ::fstat(descriptor, &temporary) == 0
				   && S_ISREG(temporary.st_mode)
				   && temporary.st_uid == ::geteuid() && temporary.st_nlink == 1
				   && (temporary.st_mode & 0077) == 0;
	auto offset = qsizetype(0);
	while (success && offset < text.size()) {
		const auto written = ::write(descriptor, text.constData() + offset,
									 size_t(text.size() - offset));
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			success = false;
		} else {
			offset += qsizetype(written);
		}
	}
	success = success && ::fsync(descriptor) == 0;
	::close(descriptor);
	if (success) {
		success = ::renameat(directory.value, temporaryName.constData(),
							 directory.value, markerName.constData())
				  == 0;
	}
	if (success) {
		success = ::fsync(directory.value) == 0;
	}
	if (!success) {
		::unlinkat(directory.value, temporaryName.constData(), 0);
	}
	return success;
}

TeagramUpdateInstallLock::TeagramUpdateInstallLock(
	const QString &installedBundlePath) {
	const auto parent = QFileInfo(installedBundlePath).absolutePath();
	_directory = CanonicalDirectory(parent);
	if (_directory.isEmpty()) {
		return;
	}
	const auto path = QFile::encodeName(
		QDir(_directory).filePath(u".teagram-update.lock"_q));
	_descriptor = ::open(path.constData(),
						 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (_descriptor < 0) {
		return;
	}
	struct stat info = {};
	if (::fstat(_descriptor, &info) != 0 || !S_ISREG(info.st_mode)
		|| info.st_uid != ::geteuid() || info.st_nlink != 1
		|| (info.st_mode & 0077) != 0
		|| ::flock(_descriptor, LOCK_EX | LOCK_NB) != 0) {
		::close(_descriptor);
		_descriptor = -1;
	}
}

TeagramUpdateInstallLock::~TeagramUpdateInstallLock() {
	if (_descriptor >= 0) {
		::close(_descriptor);
	}
}

TeagramUpdateInstallLock::TeagramUpdateInstallLock(
	TeagramUpdateInstallLock &&other) noexcept
	: _directory(std::move(other._directory)),
	  _descriptor(std::exchange(other._descriptor, -1)) {}

TeagramUpdateInstallLock &
TeagramUpdateInstallLock::operator=(TeagramUpdateInstallLock &&other) noexcept {
	if (this != &other) {
		if (_descriptor >= 0) {
			::close(_descriptor);
		}
		_directory = std::move(other._directory);
		_descriptor = std::exchange(other._descriptor, -1);
	}
	return *this;
}

bool TeagramUpdateInstallLock::acquired() const { return _descriptor >= 0; }

const QString &TeagramUpdateInstallLock::directory() const {
	return _directory;
}

TeagramUpdateReplacementResult CommitTeagramUpdateReplacement(
	const TeagramUpdateInstallLock &lock, const QString &installedBundlePath,
	const QString &stagedBundlePath, const QString &previousBundlePath,
	const std::function<bool()> &isCancelled
#ifdef TDESKTOP_UNIT_TESTS
	,
	const TeagramUpdateReplacementFailureInjector &failureInjector
#endif // TDESKTOP_UNIT_TESTS
) {
	if (!lock.acquired()) {
		return Result(TeagramUpdateReplacementReason::Busy);
	}
	const auto installed = ResolvePath(installedBundlePath);
	const auto candidate = ResolvePath(stagedBundlePath);
	const auto previous = ResolvePath(previousBundlePath);
	if (installed.parent.isEmpty() || candidate.parent.isEmpty()
		|| previous.parent != installed.parent
		|| installed.parent != lock.directory()
		|| installed.full == candidate.full || installed.full == previous.full
		|| candidate.full == previous.full) {
		return Result(TeagramUpdateReplacementReason::InvalidPath);
	}
	auto installedParent = OpenDirectory(installed.parent, false);
	auto candidateParent = OpenDirectory(candidate.parent, true);
	if (installedParent.value < 0 || candidateParent.value < 0
		|| ::access(installed.parent.toLocal8Bit().constData(), W_OK | X_OK)
			   != 0
		|| ::access(candidate.parent.toLocal8Bit().constData(), W_OK | X_OK)
			   != 0) {
		return Result(TeagramUpdateReplacementReason::InvalidPath);
	}
	auto targetDevice = dev_t();
	auto candidateDevice = dev_t();
	if (!IsOwnedDirectoryAt(installedParent.value, installed.name,
							&targetDevice)
		|| !IsOwnedDirectoryAt(candidateParent.value, candidate.name,
							   &candidateDevice)) {
		return Result(TeagramUpdateReplacementReason::InvalidPath);
	}
	struct stat targetParentStat = {};
	struct stat candidateParentStat = {};
	if (::fstat(installedParent.value, &targetParentStat) != 0
		|| ::fstat(candidateParent.value, &candidateParentStat) != 0
		|| targetDevice != candidateDevice
		|| targetParentStat.st_dev != candidateParentStat.st_dev
		|| candidateParentStat.st_uid != ::geteuid()
		|| (candidateParentStat.st_mode & 0077) != 0) {
		return Result(TeagramUpdateReplacementReason::InvalidPath);
	}
	struct stat existingPrevious = {};
	if (::fstatat(installedParent.value, previous.name.constData(),
				  &existingPrevious, AT_SYMLINK_NOFOLLOW)
		== 0) {
		return Result(TeagramUpdateReplacementReason::PreviousBundleExists);
	} else if (errno != ENOENT) {
		return Result(TeagramUpdateReplacementReason::InvalidPath);
	}
	const auto cancelledBeforeCommit = isCancelled && isCancelled();
#ifdef TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCommit
		= Injected(failureInjector,
				   TeagramUpdateReplacementFailurePoint::BeforePreviousMove);
#else  // TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCommit = false;
#endif // TDESKTOP_UNIT_TESTS
	if (cancelledBeforeCommit || injectedBeforeCommit) {
#ifdef TDESKTOP_UNIT_TESTS
		return Result(injectedBeforeCommit
						  ? TeagramUpdateReplacementReason::InjectedFailure
						  : TeagramUpdateReplacementReason::Cancelled);
#else  // TDESKTOP_UNIT_TESTS
		return Result(TeagramUpdateReplacementReason::Cancelled);
#endif // TDESKTOP_UNIT_TESTS
	}
#if defined Q_OS_MAC
	if (!SwapPaths(installedParent.value, installed.name, candidateParent.value,
				   candidate.name)) {
		return Result(TeagramUpdateReplacementReason::ReplacementFailed);
	}
	const auto rollback = [&](TeagramUpdateReplacementReason reason) {
		const auto restored = SwapPaths(
			installedParent.value, installed.name,
			candidateParent.value, candidate.name);
#ifdef TDESKTOP_UNIT_TESTS
		const auto injectedSyncFailure = restored
			&& Injected(
				failureInjector,
				TeagramUpdateReplacementFailurePoint::RollbackSyncFailure);
#else  // TDESKTOP_UNIT_TESTS
		const auto injectedSyncFailure = false;
#endif // TDESKTOP_UNIT_TESTS
		if (!restored || injectedSyncFailure
			|| ::fsync(installedParent.value) != 0
			|| ::fsync(candidateParent.value) != 0) {
			return Result(TeagramUpdateReplacementReason::RecoveryRequired,
						  candidate.full);
		}
		return Result(reason);
	};
#ifdef TDESKTOP_UNIT_TESTS
	if (failureInjector
		&& failureInjector(
			TeagramUpdateReplacementFailurePoint::AfterPreviousMove)) {
		return rollback(TeagramUpdateReplacementReason::InjectedFailure);
	}
#endif // TDESKTOP_UNIT_TESTS
	const auto cancelledBeforeCandidate = isCancelled && isCancelled();
#ifdef TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCandidate
		= Injected(failureInjector,
				   TeagramUpdateReplacementFailurePoint::BeforeCandidateMove);
#else  // TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCandidate = false;
#endif // TDESKTOP_UNIT_TESTS
	if (cancelledBeforeCandidate || injectedBeforeCandidate) {
#ifdef TDESKTOP_UNIT_TESTS
		return rollback(injectedBeforeCandidate
							? TeagramUpdateReplacementReason::InjectedFailure
							: TeagramUpdateReplacementReason::Cancelled);
#else  // TDESKTOP_UNIT_TESTS
		return rollback(TeagramUpdateReplacementReason::Cancelled);
#endif // TDESKTOP_UNIT_TESTS
	}
	if (!RenameNoReplace(candidateParent.value, candidate.name,
						 installedParent.value, previous.name)) {
		return rollback(TeagramUpdateReplacementReason::ReplacementFailed);
	}
#ifdef TDESKTOP_UNIT_TESTS
	const auto injectedCommitSyncFailure
		= Injected(
			failureInjector,
			TeagramUpdateReplacementFailurePoint::CommittedSyncFailure);
#else  // TDESKTOP_UNIT_TESTS
	const auto injectedCommitSyncFailure = false;
#endif // TDESKTOP_UNIT_TESTS
	if (injectedCommitSyncFailure
		|| ::fsync(candidateParent.value) != 0
		|| ::fsync(installedParent.value) != 0) {
		return Result(TeagramUpdateReplacementReason::RecoveryRequired,
					  previous.full, true);
	}
#else // Q_OS_MAC
	if (!RenameNoReplace(installedParent.value, installed.name,
						 installedParent.value, previous.name)) {
		return Result(TeagramUpdateReplacementReason::ReplacementFailed);
	}
	const auto restorePrevious = [&] {
		return RenameNoReplace(installedParent.value, previous.name,
							   installedParent.value, installed.name);
	};
	const auto rollback = [&](TeagramUpdateReplacementReason reason) {
		if (!restorePrevious() || ::fsync(installedParent.value) != 0) {
			return Result(TeagramUpdateReplacementReason::RecoveryRequired,
						  previous.full);
		}
		return Result(reason);
	};
	if (::fsync(installedParent.value) != 0) {
		return rollback(TeagramUpdateReplacementReason::ReplacementFailed);
	}
#ifdef TDESKTOP_UNIT_TESTS
	if (failureInjector
		&& failureInjector(
			TeagramUpdateReplacementFailurePoint::AfterPreviousMove)) {
		return rollback(TeagramUpdateReplacementReason::InjectedFailure);
	}
#endif // TDESKTOP_UNIT_TESTS
	const auto cancelledBeforeCandidate = isCancelled && isCancelled();
#ifdef TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCandidate
		= Injected(failureInjector,
				   TeagramUpdateReplacementFailurePoint::BeforeCandidateMove);
#else  // TDESKTOP_UNIT_TESTS
	const auto injectedBeforeCandidate = false;
#endif // TDESKTOP_UNIT_TESTS
	if (cancelledBeforeCandidate || injectedBeforeCandidate) {
#ifdef TDESKTOP_UNIT_TESTS
		return rollback(injectedBeforeCandidate
							? TeagramUpdateReplacementReason::InjectedFailure
							: TeagramUpdateReplacementReason::Cancelled);
#else  // TDESKTOP_UNIT_TESTS
		return rollback(TeagramUpdateReplacementReason::Cancelled);
#endif // TDESKTOP_UNIT_TESTS
	}
	if (!RenameNoReplace(candidateParent.value, candidate.name,
						 installedParent.value, installed.name)) {
		return rollback(TeagramUpdateReplacementReason::ReplacementFailed);
	}
#ifdef TDESKTOP_UNIT_TESTS
	const auto injectedCommitSyncFailure
		= Injected(
			failureInjector,
			TeagramUpdateReplacementFailurePoint::CommittedSyncFailure);
#else  // TDESKTOP_UNIT_TESTS
	const auto injectedCommitSyncFailure = false;
#endif // TDESKTOP_UNIT_TESTS
	if (injectedCommitSyncFailure
		|| ::fsync(candidateParent.value) != 0
		|| ::fsync(installedParent.value) != 0) {
		return Result(TeagramUpdateReplacementReason::RecoveryRequired,
					  previous.full, true);
	}
#endif // Q_OS_MAC
	return Result(TeagramUpdateReplacementReason::Replaced, previous.full, true);
}

TeagramUpdateStartupRecoveryResult RecoverTeagramUpdateReplacement(
	const TeagramUpdateInstallLock &lock, const QString &installedBundlePath,
	const QString &stagingDirectory, const QString &previousBundlePath,
	const QString &runningBundlePath, bool stagingBundleIsPrevious,
	bool startupReady, bool noOtherInstance) {
	using Reason = TeagramUpdateStartupRecoveryReason;
	if (!lock.acquired()) {
		return {Reason::Busy, {}};
	}
	const auto installed = ResolvePath(installedBundlePath);
	const auto stage = ResolvePath(stagingDirectory);
	const auto previous = ResolvePath(previousBundlePath);
	const auto running = ResolvePath(runningBundlePath);
	if (installed.parent.isEmpty() || stage.parent != installed.parent
		|| previous.parent != installed.parent || running.parent.isEmpty()
		|| lock.directory() != installed.parent) {
		return {Reason::Failed, {}};
	}
	auto installedParent = OpenDirectory(installed.parent, false);
	if (installedParent.value < 0) {
		return {Reason::Failed, {}};
	}
	struct stat targetInfo = {};
	auto targetExists = false;
	struct stat previousInfo = {};
	auto previousExists = false;
	if (!InspectAt(installedParent.value, installed.name, &targetInfo,
				   &targetExists)
		|| !InspectAt(installedParent.value, previous.name, &previousInfo,
					  &previousExists)) {
		return {Reason::Failed, {}};
	}
	if (!targetExists) {
		if (!previousExists || !S_ISDIR(previousInfo.st_mode)
			|| previousInfo.st_uid != ::geteuid()
			|| !RenameNoReplace(installedParent.value, previous.name,
								installedParent.value, installed.name)
			|| ::fsync(installedParent.value) != 0) {
			return {Reason::Failed, {}};
		}
		return {Reason::RestoredPrevious, installed.full};
	}
	if (!S_ISDIR(targetInfo.st_mode) || targetInfo.st_uid != ::geteuid()
		|| (previousExists
			&& (!S_ISDIR(previousInfo.st_mode)
				|| previousInfo.st_uid != ::geteuid()))) {
		return {Reason::Failed, {}};
	}
	struct stat stageRootInfo = {};
	auto stageExists = false;
	if (!InspectAt(installedParent.value, stage.name, &stageRootInfo,
				   &stageExists)) {
		return {Reason::Failed, {}};
	}
	auto movedPrevious = false;
	auto changed = false;
	if (stageExists) {
		if (!S_ISDIR(stageRootInfo.st_mode)
			|| stageRootInfo.st_uid != ::geteuid()
			|| (stageRootInfo.st_mode & 0077) != 0) {
			return {Reason::Failed, {}};
		}
		const auto stagePath
			= QDir(installed.parent).filePath(QFile::decodeName(stage.name));
		auto stageRoot = OpenDirectory(stagePath, true);
		if (stageRoot.value < 0) {
			return {Reason::Failed, {}};
		}
		struct stat stagedInfo = {};
		auto stagedExists = false;
		if (!InspectAt(stageRoot.value, QByteArrayLiteral("Teagram.app"),
					   &stagedInfo, &stagedExists)) {
			return {Reason::Failed, {}};
		}
		if (stagedExists) {
			if (!S_ISDIR(stagedInfo.st_mode)
				|| stagedInfo.st_uid != ::geteuid()) {
				return {Reason::Failed, {}};
			}
			if (stagingBundleIsPrevious && !previousExists) {
				if (!RenameNoReplace(stageRoot.value,
									 QByteArrayLiteral("Teagram.app"),
									 installedParent.value, previous.name)) {
					return {Reason::Failed, {}};
				}
				movedPrevious = true;
				changed = true;
			} else if (!RemoveTreeAt(stageRoot.value, "Teagram.app")) {
				return {Reason::Failed, {}};
			} else {
				changed = true;
			}
		}
		if (::unlinkat(installedParent.value, stage.name.constData(),
					   AT_REMOVEDIR)
			== 0) {
			changed = true;
		} else {
			return {Reason::Failed, {}};
		}
	}
	const auto currentIsInstalled = running.full == installed.full;
	if (previousExists && currentIsInstalled && startupReady && noOtherInstance
		&& !movedPrevious) {
		if (!RemoveTreeAt(installedParent.value, previous.name.constData())
			|| ::fsync(installedParent.value) != 0) {
			return {Reason::Failed, previous.full};
		}
		return {Reason::PreviousRemoved, {}};
	}
	if (::fsync(installedParent.value) != 0) {
		return {Reason::Failed, previousExists ? previous.full : QString()};
	}
	if (movedPrevious || previousExists) {
		return {Reason::PreviousRetained, previous.full};
	}
	return {
		changed ? Reason::IncompleteCandidateRemoved : Reason::NoPending,
		{},
	};
}

} // namespace Core
