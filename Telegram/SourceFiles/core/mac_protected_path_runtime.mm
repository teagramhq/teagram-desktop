/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mac_protected_path_runtime.h"

#include "core/mac_protected_path_access.h"
#include "settings.h"

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
#include "media/audio/media_audio_local_cache.h"
#include <QtCore/QTemporaryDir>
#endif

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSaveFile>
#include <QtCore/QThread>

#include <Cocoa/Cocoa.h>
#include <sandbox.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <pwd.h>
#include <spawn.h>
#include <set>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" char **environ;

namespace Core::MacProtectedPath {

struct CachePathDescriptor final {
	explicit CachePathDescriptor(int fd) : value(fd) {}
	~CachePathDescriptor() {
		if (value >= 0) {
			::close(value);
		}
	}

	const int value = -1;
};

namespace {

struct RuntimeState final {
	QMutex mutex;
	bool initialized = false;
	bool ready = false;
	QString initialWorkingDirectory;
	QString ipcDirectory;
	QString profile;
	std::shared_ptr<const MacProtectedPathPolicy> policy;
	RefusalLog refusals;
};

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
QMutex CachePathWalkCountMutex;
auto CachePathWalkCount = 0;
auto CachePathWalkMainThreadCount = 0;
Qt::HANDLE CachePathWalkMainThread = nullptr;
#endif

[[nodiscard]] RuntimeState &State() {
	static auto result = RuntimeState();
	return result;
}

[[nodiscard]] bool IntegrationTestRequested() {
	const auto value
		= std::getenv("TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST");
	return value && !std::strcmp(value, "1");
}

[[nodiscard]] QByteArray AccountDatabaseHome() {
	const auto suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
	const auto size = size_t(
		(suggested > 0 && suggested <= 1024 * 1024) ? suggested : 16384);
	auto buffer = std::vector<char>(size);
	struct passwd entry = {};
	auto result = static_cast<passwd *>(nullptr);
	if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result)
		|| !result || !result->pw_dir) {
		return {};
	}
	return QByteArray(result->pw_dir);
}

[[nodiscard]] QByteArray FoundationHome() {
	@autoreleasepool {
		const auto home = NSHomeDirectory();
		const auto path = [home fileSystemRepresentation];
		return path ? QByteArray(path) : QByteArray();
	}
}

[[nodiscard]] HomeRoots NativeHomeRoots() {
	auto result
		= HomeRoots{.accountDatabase = AccountDatabaseHome(),
					.environment =
						[] {
							const auto home = std::getenv("HOME");
							return home ? QByteArray(home) : QByteArray();
						}(),
					.foundation = FoundationHome()};
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	const auto testHome = std::getenv("TDESKTOP_MAC_PROFILE_TEST_HOME");
	if (testHome && *testHome) {
		result.accountDatabase = testHome;
		result.foundation = testHome;
	}
#endif
	return result;
}

[[nodiscard]] FileError FileErrorFromErrno(int value) {
	switch (value) {
	case ENOENT:
		return FileError::Missing;
	case ENOTDIR:
		return FileError::NotDirectory;
	case ELOOP:
		return FileError::Loop;
	default:
		return FileError::Unexpected;
	}
}

[[nodiscard]] LstatResult NativeLstat(const QByteArray &path) {
	struct stat info = {};
	if (::lstat(path.constData(), &info)) {
		return {.error = FileErrorFromErrno(errno)};
	}
	const auto type = S_ISDIR(info.st_mode)	  ? FileType::Directory
					  : S_ISREG(info.st_mode) ? FileType::Regular
					  : S_ISLNK(info.st_mode) ? FileType::Symlink
											  : FileType::Other;
	return {.type = type, .error = FileError::None};
}

[[nodiscard]] ReadlinkResult NativeReadlink(const QByteArray &path) {
	auto size = size_t(1024);
	while (size <= 65536) {
		auto target = QByteArray(int(size), Qt::Uninitialized);
		const auto count = ::readlink(path.constData(), target.data(),
									  size_t(target.size()));
		if (count < 0) {
			return {.error = FileErrorFromErrno(errno)};
		}
		if (size_t(count) < size) {
			target.resize(int(count));
			return {.target = std::move(target), .error = FileError::None};
		}
		size *= 2;
	}
	return {.error = FileError::Unexpected};
}

[[nodiscard]] FileSystem NativeFileSystem() {
	return {.lstat = NativeLstat, .readlink = NativeReadlink};
}

[[nodiscard]] QString OperationName(Operation operation) {
	switch (operation) {
	case Operation::Open:
		return u"open"_q;
	case Operation::Read:
		return u"read"_q;
	case Operation::Write:
		return u"write"_q;
	case Operation::Stat:
		return u"stat"_q;
	case Operation::Lstat:
		return u"lstat"_q;
	case Operation::OpenDir:
		return u"opendir"_q;
	case Operation::GetAttrList:
		return u"getattrlist"_q;
	case Operation::Mkdir:
		return u"mkdir"_q;
	case Operation::Lock:
		return u"lock"_q;
	case Operation::Rename:
		return u"rename"_q;
	case Operation::Copy:
		return u"copy"_q;
	case Operation::Link:
		return u"link"_q;
	case Operation::Unlink:
		return u"unlink"_q;
	case Operation::Rmdir:
		return u"rmdir"_q;
	case Operation::RecursiveDelete:
		return u"recursive-delete"_q;
	}
	return u"unknown"_q;
}

[[nodiscard]] QString ProtectedClassName(ProtectedClass protectedClass) {
	switch (protectedClass) {
	case ProtectedClass::None:
		return u"none"_q;
	case ProtectedClass::ApplicationSupport:
		return u"application-support"_q;
	case ProtectedClass::Container:
		return u"container"_q;
	case ProtectedClass::GroupContainer:
		return u"group-container"_q;
	case ProtectedClass::BundleKeyed:
		return u"bundle-keyed"_q;
	case ProtectedClass::Invalid:
		return u"invalid"_q;
	}
	return u"unknown"_q;
}

void ReportRefusal(const RefusalRecord &refusal) {
	if (!State().refusals.record(refusal)) {
		return;
	}
	const auto message
		= u"Mac protected path refusal: operation=%1 class=%2 callsite=%3"_q
			  .arg(OperationName(refusal.operation),
				   ProtectedClassName(refusal.protectedClass),
				   refusal.callsite);
	fprintf(stderr, "%s\n", message.toUtf8().constData());
}

void ReportInvalidInitialization(const QString &callsite) {
	ReportRefusal({.operation = Operation::Open,
				   .protectedClass = ProtectedClass::Invalid,
				   .callsite = callsite});
}

[[nodiscard]] bool AppSandboxed() {
#ifdef OS_MAC_STORE
	return true;
#else  // OS_MAC_STORE
	return false;
#endif // !OS_MAC_STORE
}

[[nodiscard]] int InitializeSeatbelt(const char *profile, char **error) {
	int output[2] = {};
	if (::pipe(output) != 0) {
		return -1;
	}
	const auto savedStderr = ::dup(STDERR_FILENO);
	if (savedStderr < 0) {
		::close(output[0]);
		::close(output[1]);
		return -1;
	}
	if (::dup2(output[1], STDERR_FILENO) < 0) {
		::close(savedStderr);
		::close(output[0]);
		::close(output[1]);
		return -1;
	}
	::close(output[1]);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	const auto status = sandbox_init(profile, 0, error);
#pragma clang diagnostic pop
	const auto restoreStatus = ::dup2(savedStderr, STDERR_FILENO);
	::close(savedStderr);
	::close(output[0]);
	return (restoreStatus < 0) ? -1 : status;
}

} // namespace

bool IntegrationTestActive() {
#if defined(TDESKTOP_TEAGRAM)                                                  \
	&& defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	return IntegrationTestRequested();
#else
	return false;
#endif
}

bool IsActive() {
	auto &state = State();
	QMutexLocker lock(&state.mutex);
	return state.ready;
}

bool InitializeProfile() {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return !IntegrationTestRequested();
	}
#else  // TDESKTOP_TEAGRAM
	if (IntegrationTestRequested() && !IntegrationTestActive()) {
		return false;
	}
#endif // TDESKTOP_TEAGRAM
	const auto failForIntegrationTest = [](const char *stage) {
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
		const auto diagnostics
			= std::getenv("TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS");
		if (IntegrationTestActive() && diagnostics
			&& !std::strcmp(diagnostics, "1")) {
			fprintf(stderr,
					"Mac profile integration initialization failed: stage=%s\n",
					stage);
		}
#endif
		return false;
	};
	auto &state = State();
	{
		QMutexLocker lock(&state.mutex);
		if (state.initialized) {
			return state.ready;
		}
		state.initialized = true;
	}

	const auto homes = NativeHomeRoots();
	const auto filesystem = NativeFileSystem();
	const auto policy = MacProtectedPathPolicy::Build(homes, filesystem);
	if (!policy.valid()) {
		return failForIntegrationTest("policy");
	}
	auto profileText = policy.SeatbeltProfile();
	if (profileText.isEmpty()) {
		return failForIntegrationTest("profile");
	}
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	if (IntegrationTestActive()) {
		const auto forceFailure
			= std::getenv("TDESKTOP_MAC_SEATBELT_FORCE_COMPILE_FAILURE");
		if (forceFailure && !std::strcmp(forceFailure, "1")) {
			profileText.append("(\n");
		}
	}
#endif
	auto *error = static_cast<char *>(nullptr);
	const auto sandboxStatus
		= InitializeSeatbelt(profileText.constData(), &error);
	if (sandboxStatus != 0) {
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
		const auto diagnostics
			= std::getenv("TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS");
		if (IntegrationTestActive() && diagnostics
			&& !std::strcmp(diagnostics, "1")) {
			fprintf(stderr,
					"Mac profile integration initialization failed: "
					"stage=seatbelt status=%d error=%s\n",
					sandboxStatus, error ? error : "unavailable");
			const auto artifact
				= std::getenv("TDESKTOP_MAC_PROFILE_TEST_PROFILE_ARTIFACT");
			if (artifact && *artifact) {
				auto file = QSaveFile(QString::fromUtf8(artifact));
				if (file.open(QIODevice::WriteOnly)
					&& file.write(profileText) == profileText.size()
					&& file.commit()) {
					fprintf(stderr,
							"Mac profile artifact saved: path=%s bytes=%lld\n",
							artifact,
							static_cast<long long>(profileText.size()));
				} else {
					const auto message = file.errorString().toUtf8();
					fprintf(stderr,
							"Mac profile artifact save failed: path=%s "
							"error=%s\n",
							artifact, message.constData());
				}
			}
		}
#endif
	}
	if (error) {
		sandbox_free_error(error);
	}
	if (sandboxStatus != 0) {
		return failForIntegrationTest("seatbelt");
	}
	const auto currentWorkingDirectory = QDir::currentPath();
	if (currentWorkingDirectory.isEmpty()) {
		return failForIntegrationTest("working-directory");
	}
	const auto initialWorkingDirectory = currentWorkingDirectory + '/';
	const auto profileBytes = TeagramProfileRoot(homes, AppSandboxed());
	if (profileBytes.isEmpty()) {
		return failForIntegrationTest("profile-root");
	}
	const auto profile
		= policy.Resolve(Operation::Open, profileBytes, homes.accountDatabase,
						 u"profile.root"_q);
	if (!profile.allowed()) {
		return failForIntegrationTest("profile-resolution");
	}
	const auto profilePath = QString::fromUtf8(profile.resolvedPath);
	const auto create
		= policy.Resolve(Operation::Mkdir, profile.resolvedPath,
						 homes.accountDatabase, u"profile.create"_q);
	if (!create.allowed()) {
		return failForIntegrationTest("profile-create-resolution");
	}
	const auto temporaryPath = profilePath + u"/tdata/temp"_q;
	const auto temporaryPrepared = PrepareExternalDirectoryIfAllowed(
		temporaryPath, "profile.helper-temp",
		[&](Operation operation, const QString &path, const char *callsite) {
			return policy
				.Resolve(operation, QFile::encodeName(path),
						 homes.accountDatabase, QString::fromUtf8(callsite))
				.allowed();
		},
		[&] {
			if (!QDir().mkpath(profilePath) || !QDir().mkpath(temporaryPath)) {
				ReportInvalidInitialization(u"profile.mkdir"_q);
				return false;
			}
			if (!qputenv("TMPDIR", QFile::encodeName(temporaryPath))
				|| !SameCleanPathAfterUnicodeNormalization(QDir::tempPath(),
														   temporaryPath)) {
				ReportInvalidInitialization(u"profile.helper-temp"_q);
				return false;
			}
			return true;
		});
	if (!temporaryPrepared) {
		return failForIntegrationTest("helper-temp");
	}

	const auto appSandboxed = AppSandboxed();
	auto ipcDirectory = appSandboxed
							? QString::fromUtf8(homes.foundation) + u"/tmp"_q
							: u"/tmp"_q;
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	if (IntegrationTestActive()) {
		const auto testIpc = std::getenv("TDESKTOP_MAC_PROFILE_TEST_IPC");
		if (testIpc && *testIpc) {
			const auto directory = QString::fromUtf8(testIpc);
			if (!QDir::isAbsolutePath(directory)
				|| !PrepareExternalDirectoryIfAllowed(
					directory, "profile.test-ipc",
					[&](Operation operation, const QString &path,
						const char *callsite) {
						return policy
							.Resolve(operation, QFile::encodeName(path),
									 homes.accountDatabase,
									 QString::fromUtf8(callsite))
							.allowed();
					},
					[&] { return QDir().mkpath(directory); })) {
				return failForIntegrationTest("test-ipc");
			}
			ipcDirectory = directory;
		}
	}
#endif
	cForceWorkingDir(profilePath + '/');
	{
		QMutexLocker lock(&state.mutex);
		state.initialWorkingDirectory = initialWorkingDirectory;
		state.ipcDirectory = ipcDirectory;
		state.profile = cWorkingDir();
		state.policy = std::make_shared<MacProtectedPathPolicy>(policy);
		state.ready = true;
	}
	if (IntegrationTestActive()) {
		const auto path = ipcDirectory.toUtf8();
		fprintf(stderr, "Mac profile IPC selected: variant=%s directory=%s\n",
				appSandboxed ? "build_macstore" : "non-store",
				path.constData());
	}
	return true;
}

QString InitialWorkingDirectory() {
	auto &state = State();
	QMutexLocker lock(&state.mutex);
	return state.ready ? state.initialWorkingDirectory : QString();
}

QString ProfileRoot() {
	auto &state = State();
	QMutexLocker lock(&state.mutex);
	return state.ready ? state.profile : QString();
}

QString NotificationSoundsDirectory() {
	const auto home = FoundationHome();
	return home.isEmpty() ? QString()
						  : QString::fromUtf8(home) + u"/Library/Sounds"_q;
}

QString IpcDirectory() {
	{
		auto &state = State();
		QMutexLocker lock(&state.mutex);
		if (state.ready) {
			return state.ipcDirectory;
		}
	}
	if (AppSandboxed()) {
		const auto home = FoundationHome();
		return home.isEmpty() ? QString() : QString::fromUtf8(home) + u"/tmp"_q;
	}
	return u"/tmp"_q;
}

bool CheckPathAt(Operation operation, const QString &path,
				 const QString &anchor, const char *callsite) {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	auto policy = std::shared_ptr<const MacProtectedPathPolicy>();
	{
		auto &state = State();
		QMutexLocker lock(&state.mutex);
		if (!state.ready || !state.policy) {
			ReportInvalidInitialization(u"operation.before-profile"_q);
			return false;
		}
		policy = state.policy;
	}
	const auto result = policy->Resolve(operation, QFile::encodeName(path),
										QFile::encodeName(anchor),
										QString::fromUtf8(callsite));
	if (!result.allowed()) {
		ReportRefusal(result.refusal);
		return false;
	}
	return true;
}

bool CheckPath(Operation operation, const QString &path, const char *callsite) {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	auto anchor = QString();
	{
		auto &state = State();
		QMutexLocker lock(&state.mutex);
		if (!state.ready || !state.policy) {
			ReportInvalidInitialization(u"operation.before-profile"_q);
			return false;
		}
		anchor = state.profile;
	}
	return CheckPathAt(operation, path, anchor, callsite);
}

bool CheckExternalPath(Operation operation, const QString &path,
					   const char *callsite) {
	return CheckPathAt(operation, path, QString(), callsite);
}

namespace {

bool CheckCachePathImpl(
	const QString &path, const char *callsite,
	const std::function<void(const QString &)> &beforeEntryStat) {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	{
		QMutexLocker lock(&CachePathWalkCountMutex);
		++CachePathWalkCount;
		if (QThread::currentThreadId() == CachePathWalkMainThread) {
			++CachePathWalkMainThreadCount;
		}
	}
#endif
	if (!CheckPath(Operation::OpenDir, path, callsite)) {
		return false;
	}
	auto allowed = true;
	auto directories = std::vector<QString>{path};
	auto visited = std::set<QString>();
	while (!directories.empty()) {
		const auto directory = std::move(directories.back());
		directories.pop_back();
		if (!CheckPath(Operation::OpenDir, directory, callsite)) {
			allowed = false;
			continue;
		}
		const auto canonical = QFileInfo(directory).canonicalFilePath();
		const auto identity
			= canonical.isEmpty() ? QDir::cleanPath(directory) : canonical;
		if (!visited.emplace(identity).second) {
			continue;
		}
		const auto native = QFile::encodeName(directory);
		const auto descriptor
			= ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (descriptor < 0) {
			if (errno != ENOENT) {
				allowed = false;
			}
			continue;
		}
		const auto stream = fdopendir(descriptor);
		if (!stream) {
			::close(descriptor);
			allowed = false;
			continue;
		}
		const auto close = gsl::finally([=] { closedir(stream); });
		while (const auto entry = readdir(stream)) {
			const auto local = entry->d_name;
			if (!std::strcmp(local, ".") || !std::strcmp(local, "..")) {
				continue;
			}
			const auto name = QFile::decodeName(local);
			const auto child = QDir(directory).filePath(name);
			if (!CheckPath(Operation::Open, child, callsite)) {
				allowed = false;
				continue;
			}
			if (beforeEntryStat) {
				beforeEntryStat(child);
			}
			struct stat info = {};
			if (fstatat(dirfd(stream), local, &info, AT_SYMLINK_NOFOLLOW)
				!= 0) {
				if (errno != ENOENT) {
					allowed = false;
				}
				continue;
			}
			if (S_ISDIR(info.st_mode)) {
				if (!CheckPath(Operation::OpenDir, child, callsite)) {
					allowed = false;
					continue;
				}
				directories.push_back(child);
			} else if (S_ISLNK(info.st_mode)) {
				if (!CheckPath(Operation::OpenDir, child, callsite)) {
					allowed = false;
					continue;
				}
				if (fstatat(dirfd(stream), local, &info, 0) != 0) {
					if (errno != ENOENT) {
						allowed = false;
					}
				} else if (S_ISDIR(info.st_mode)) {
					directories.push_back(child);
				}
			}
		}
	}
	return allowed;
}

[[nodiscard]] QString AbsoluteCachePath(const QString &path) {
	if (QDir::isAbsolutePath(path)) {
		return QDir::cleanPath(path);
	}
	const auto profile = ProfileRoot();
	return profile.isEmpty() ? QString()
							 : QDir::cleanPath(QDir(profile).filePath(path));
}

[[nodiscard]] std::vector<QString>
CachePathPrefixes(const QString &absolutePath) {
	auto result = std::vector<QString>{u"/"_q};
	auto prefix = u"/"_q;
	for (const auto &component : absolutePath.split('/', Qt::SkipEmptyParts)) {
		if (prefix != u"/"_q) {
			prefix.append('/');
		}
		prefix.append(component);
		result.push_back(prefix);
	}
	return result;
}

void SetCachePathLinkIdentity(const struct stat &info,
							  CachePathComponentIdentity *identity) {
	identity->device = quint64(info.st_dev);
	identity->inode = quint64(info.st_ino);
	identity->type = quint32(info.st_mode & S_IFMT);
	identity->size = qint64(info.st_size);
	identity->modifiedSeconds = qint64(info.st_mtimespec.tv_sec);
	identity->modifiedNanoseconds = qint64(info.st_mtimespec.tv_nsec);
	identity->changedSeconds = qint64(info.st_ctimespec.tv_sec);
	identity->changedNanoseconds = qint64(info.st_ctimespec.tv_nsec);
	identity->exists = true;
}

void SetCachePathTargetIdentity(const struct stat &info,
								CachePathComponentIdentity *identity) {
	identity->targetDevice = quint64(info.st_dev);
	identity->targetInode = quint64(info.st_ino);
	identity->targetType = quint32(info.st_mode & S_IFMT);
}

[[nodiscard]] bool
SameCachePathStat(const struct stat &info,
				  const CachePathComponentIdentity &identity) {
	return (quint64(info.st_dev) == identity.device)
		   && (quint64(info.st_ino) == identity.inode)
		   && (quint32(info.st_mode & S_IFMT) == identity.type);
}

// The marker only changes during open or clear; metadata detects those changes
// in one lstat without reopening and reading it on every cache access.
[[nodiscard]] bool
SameCachePathVersionMarkerStat(const struct stat &info,
							   const CachePathComponentIdentity &identity) {
	return SameCachePathStat(info, identity)
		   && (qint64(info.st_size) == identity.size)
		   && (qint64(info.st_mtimespec.tv_sec) == identity.modifiedSeconds)
		   && (qint64(info.st_mtimespec.tv_nsec)
			   == identity.modifiedNanoseconds)
		   && (qint64(info.st_ctimespec.tv_sec) == identity.changedSeconds)
		   && (qint64(info.st_ctimespec.tv_nsec)
			   == identity.changedNanoseconds);
}

[[nodiscard]] bool
SameCachePathTargetStat(const struct stat &info,
						const CachePathComponentIdentity &identity) {
	return (quint64(info.st_dev) == identity.targetDevice)
		   && (quint64(info.st_ino) == identity.targetInode)
		   && (quint32(info.st_mode & S_IFMT) == identity.targetType);
}

[[nodiscard]] bool CachePathDescriptorPath(int descriptor, QString *path) {
	auto native = std::array<char, MAXPATHLEN>();
	if (::fcntl(descriptor, F_GETPATH, native.data()) != 0) {
		return false;
	}
	*path = QFile::decodeName(QByteArray(native.data()));
	return !path->isEmpty();
}

[[nodiscard]] bool
ReadCachePathLinkIdentity(const QString &path,
						  CachePathComponentIdentity *identity, bool *missing) {
	const auto native = QFile::encodeName(path);
	auto link = struct stat();
	if (::lstat(native.constData(), &link) != 0) {
		if (errno == ENOENT) {
			*missing = true;
			return true;
		}
		return false;
	}
	if (!S_ISDIR(link.st_mode) && !S_ISLNK(link.st_mode)) {
		return false;
	}
	SetCachePathLinkIdentity(link, identity);
	return true;
}

[[nodiscard]] bool
ReadCachePathTargetIdentity(const QString &path,
							CachePathComponentIdentity *identity) {
	const auto native = QFile::encodeName(path);
	auto target = struct stat();
	if (::stat(native.constData(), &target) != 0 || !S_ISDIR(target.st_mode)) {
		return false;
	}
	SetCachePathTargetIdentity(target, identity);
	return true;
}

[[nodiscard]] bool
SameCachePathLinkIdentity(const CachePathComponentIdentity &left,
						  const CachePathComponentIdentity &right);

[[nodiscard]] bool
CaptureCachePathRootDescriptor(const QString &path,
							   const CachePathComponentIdentity &expected,
							   CachePathIdentity *identity) {
	const auto native = QFile::encodeName(path);
	const auto descriptor
		= ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0) {
		return false;
	}
	auto target = struct stat();
	if ((::fstat(descriptor, &target) != 0)
		|| !SameCachePathTargetStat(target, expected)) {
		::close(descriptor);
		return false;
	}
	auto current = CachePathComponentIdentity{.path = path};
	auto missing = false;
	if (!ReadCachePathLinkIdentity(path, &current, &missing) || missing
		|| !SameCachePathLinkIdentity(current, expected)) {
		::close(descriptor);
		return false;
	}
	auto resolved = QString();
	if (!CachePathDescriptorPath(descriptor, &resolved)) {
		::close(descriptor);
		return false;
	}
	identity->rootDescriptor
		= std::make_shared<CachePathDescriptor>(descriptor);
	identity->resolvedPath = std::move(resolved);
	return true;
}

[[nodiscard]] QString ActiveCacheVersionPath(const QString &cachePath) {
	const auto versionPath = QDir(cachePath).filePath("version");
	const auto native = QFile::encodeName(versionPath);
	const auto versionZeroPath = [&] {
		const auto zeroPath = QDir(cachePath).filePath("0");
		const auto zeroNative = QFile::encodeName(zeroPath);
		struct stat zero = {};
		if (::lstat(zeroNative.constData(), &zero) != 0 && errno == ENOENT) {
			return QString();
		}
		return zeroPath;
	};
	const auto descriptor = ::open(
		native.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0) {
		if (errno != ENOENT) {
			return {};
		}
		struct stat marker = {};
		if (::lstat(native.constData(), &marker) == 0 || errno != ENOENT) {
			return {};
		}
		return versionZeroPath();
	}
	const auto close = gsl::finally([=] { ::close(descriptor); });
	struct stat opened = {};
	if (::fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode)) {
		return {};
	}
	auto version = qint32();
	auto complete = false;
	for (auto offset = size_t(); offset != sizeof(version);) {
		const auto read
			= ::read(descriptor, reinterpret_cast<char *>(&version) + offset,
					 sizeof(version) - offset);
		if (read < 0 && errno == EINTR) {
			continue;
		} else if (read < 0) {
			return {};
		} else if (read == 0) {
			break;
		}
		offset += size_t(read);
		complete = (offset == sizeof(version));
	}
	struct stat current = {};
	if (::lstat(native.constData(), &current) != 0 || !S_ISREG(current.st_mode)
		|| (opened.st_dev != current.st_dev)
		|| (opened.st_ino != current.st_ino)) {
		return {};
	}
	if (!complete) {
		return versionZeroPath();
	} else if (version < 0) {
		return {};
	}
	return QDir(cachePath).filePath(QString::number(version));
}

[[nodiscard]] bool ReadCacheVersionDescriptor(int descriptor, qint32 *version) {
	auto value = qint32();
	auto complete = false;
	for (auto offset = size_t(); offset != sizeof(value);) {
		const auto read
			= ::pread(descriptor, reinterpret_cast<char *>(&value) + offset,
					  sizeof(value) - offset, off_t(offset));
		if (read < 0 && errno == EINTR) {
			continue;
		} else if (read < 0) {
			return false;
		} else if (read == 0) {
			break;
		}
		offset += size_t(read);
		complete = (offset == sizeof(value));
	}
	if (complete && value < 0) {
		return false;
	}
	*version = complete ? value : 0;
	return true;
}

[[nodiscard]] bool CaptureCachePathVersionMarker(CachePathIdentity *identity) {
	if (!identity) {
		return false;
	}
	auto marker = CachePathComponentIdentity{
		.path = QDir(identity->absolutePath).filePath("version")};
	if (!identity->rootDescriptor) {
		identity->version = 0;
		identity->versionName = u"0"_q;
		identity->versionMarkerDescriptor.reset();
		identity->versionMarker = std::move(marker);
		return true;
	}
	const auto native = QFile::encodeName(marker.path);
	const auto descriptor = ::open(
		native.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0) {
		if (errno != ENOENT) {
			return false;
		}
		auto current = struct stat();
		if (::lstat(native.constData(), &current) == 0 || errno != ENOENT) {
			return false;
		}
		identity->version = 0;
		identity->versionName = u"0"_q;
		identity->versionMarkerDescriptor.reset();
		identity->versionMarker = std::move(marker);
		return true;
	}
	auto opened = struct stat();
	auto current = struct stat();
	auto version = qint32();
	const auto openedMatches
		= (::fstat(descriptor, &opened) == 0) && S_ISREG(opened.st_mode);
	if (!openedMatches || !ReadCacheVersionDescriptor(descriptor, &version)) {
		::close(descriptor);
		return false;
	}
	SetCachePathLinkIdentity(opened, &marker);
	SetCachePathTargetIdentity(opened, &marker);
	const auto nativeMatches
		= (::lstat(native.constData(), &current) == 0)
		  && S_ISREG(current.st_mode)
		  && SameCachePathVersionMarkerStat(current, marker);
	if (!nativeMatches) {
		::close(descriptor);
		return false;
	}
	identity->versionMarkerDescriptor
		= std::make_shared<CachePathDescriptor>(descriptor);
	identity->version = version;
	identity->versionName = QString::number(version);
	identity->versionMarker = std::move(marker);
	return true;
}

[[nodiscard]] bool
CaptureCachePathActiveVersionDirectory(const QString &path,
									   CachePathIdentity *identity) {
	if (!identity) {
		return false;
	}
	auto active = CachePathComponentIdentity{.path = path};
	auto missing = false;
	if (!ReadCachePathLinkIdentity(path, &active, &missing) || missing
		|| (active.type != quint32(S_IFDIR))
		|| !ReadCachePathTargetIdentity(path, &active)) {
		return false;
	}
	const auto native = QFile::encodeName(path);
	const auto descriptor = ::open(
		native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0) {
		return false;
	}
	auto opened = struct stat();
	const auto openedMatches = (::fstat(descriptor, &opened) == 0)
							   && SameCachePathTargetStat(opened, active);
	auto current = CachePathComponentIdentity{.path = path};
	missing = false;
	const auto pathMatches = ReadCachePathLinkIdentity(path, &current, &missing)
							 && !missing
							 && SameCachePathLinkIdentity(current, active);
	if (!openedMatches || !pathMatches) {
		::close(descriptor);
		return false;
	}
	identity->activeVersionDescriptor
		= std::make_shared<CachePathDescriptor>(descriptor);
	identity->activeVersionDirectory = std::move(active);
	identity->activeVersionName = QFileInfo(path).fileName();
	return true;
}

[[nodiscard]] bool
CachePathVersionMarkerStillValid(const CachePathIdentity &identity,
								 qint32 *version) {
	if (identity.components.empty() || !version) {
		return false;
	}
	if (!identity.rootDescriptor) {
		if (identity.versionMarker.exists || (identity.version != 0)
			|| !identity.activeVersionDirectory.path.isEmpty()) {
			return false;
		}
		*version = 0;
		return true;
	}
	const auto native = QFile::encodeName(identity.versionMarker.path);
	auto current = struct stat();
	if (identity.versionMarker.exists) {
		if (!identity.versionMarkerDescriptor
			|| (::lstat(native.constData(), &current) != 0)
			|| !S_ISREG(current.st_mode)
			|| !SameCachePathVersionMarkerStat(current,
											   identity.versionMarker)) {
			return false;
		}
		*version = identity.version;
	} else {
		if (::lstat(native.constData(), &current) == 0) {
			return false;
		} else if (errno != ENOENT) {
			return false;
		}
		*version = 0;
	}
	return (*version == identity.version);
}

[[nodiscard]] bool CaptureCachePathIdentity(const QString &path,
											CachePathIdentity *identity) {
	const auto absolutePath = AbsoluteCachePath(path);
	if (absolutePath.isEmpty() || !identity) {
		return false;
	}
	auto result = CachePathIdentity();
	result.absolutePath = absolutePath;
	auto missingSuffix = false;
	for (const auto &prefix : CachePathPrefixes(absolutePath)) {
		auto component = CachePathComponentIdentity{.path = prefix};
		if (!missingSuffix) {
			auto missing = false;
			if (!ReadCachePathLinkIdentity(prefix, &component, &missing)) {
				return false;
			}
			missingSuffix = missing;
			if (component.exists
				&& !ReadCachePathTargetIdentity(prefix, &component)) {
				return false;
			}
		}
		result.components.push_back(std::move(component));
	}
	if (result.components.back().exists) {
		if (!CaptureCachePathRootDescriptor(
				absolutePath, result.components.back(), &result)) {
			return false;
		}
	}
	if (!CaptureCachePathVersionMarker(&result)) {
		return false;
	}
	const auto activePath = ActiveCacheVersionPath(absolutePath);
	const auto expectedActivePath
		= QDir(absolutePath).filePath(result.versionName);
	if (!activePath.isEmpty() && (activePath != expectedActivePath)) {
		return false;
	}
	if (activePath.isEmpty() && (result.version != 0)) {
		return false;
	}
	result.activeVersionDirectory.path = activePath;
	if (!result.activeVersionDirectory.path.isEmpty()) {
		if (!CaptureCachePathActiveVersionDirectory(
				result.activeVersionDirectory.path, &result)) {
			return false;
		}
	}
	*identity = std::move(result);
	return true;
}

[[nodiscard]] bool
SameCachePathLinkIdentity(const CachePathComponentIdentity &left,
						  const CachePathComponentIdentity &right) {
	return left.exists && right.exists && (left.device == right.device)
		   && (left.inode == right.inode) && (left.type == right.type);
}

void ReportCachePathIdentityRefusal(const char *callsite) {
	ReportRefusal({
		.operation = Operation::OpenDir,
		.protectedClass = ProtectedClass::Invalid,
		.callsite = callsite ? QString::fromUtf8(callsite) : QString(),
	});
}

[[nodiscard]] bool CachePathRootStillValid(const QString &path,
										   CachePathIdentity *identity) {
	const auto absolutePath = AbsoluteCachePath(path);
	if (!identity || absolutePath.isEmpty()
		|| (absolutePath != identity->absolutePath)
		|| identity->components.empty()) {
		return false;
	}
	const auto &expected = identity->components.back();
	if (!identity->rootDescriptor) {
		auto actual = CachePathComponentIdentity{.path = absolutePath};
		auto missing = false;
		return !expected.exists
			   && ReadCachePathLinkIdentity(absolutePath, &actual, &missing)
			   && missing;
	}
	if (!expected.exists || (expected.path != absolutePath)
		|| identity->resolvedPath.isEmpty()) {
		return false;
	}
	// F_GETPATH detects an ancestor rename or redirect without walking each
	// prefix for every cache access.
	auto current = CachePathComponentIdentity{.path = absolutePath};
	auto missing = false;
	auto resolved = QString();
	if (!ReadCachePathLinkIdentity(absolutePath, &current, &missing) || missing
		|| !SameCachePathLinkIdentity(current, expected)
		|| !CachePathDescriptorPath(identity->rootDescriptor->value, &resolved)
		|| (resolved != identity->resolvedPath)) {
		return false;
	}
	if (expected.type != quint32(S_IFLNK)) {
		return true;
	}
	auto target = struct stat();
	const auto native = QFile::encodeName(absolutePath);
	return (::stat(native.constData(), &target) == 0)
		   && SameCachePathTargetStat(target, expected);
}

[[nodiscard]] bool
CachePathActiveVersionStillValid(const CachePathIdentity &identity) {
	qint32 version = 0;
	if (!CachePathVersionMarkerStillValid(identity, &version)
		|| (identity.components.empty())) {
		return false;
	}
	if (!identity.rootDescriptor) {
		return identity.activeVersionDirectory.path.isEmpty();
	}
	if (identity.activeVersionDirectory.path.isEmpty()) {
		if ((version != 0) || (identity.versionName != u"0"_q)
			|| !identity.activeVersionName.isEmpty()) {
			return false;
		}
		auto zero = struct stat();
		const auto native
			= QFile::encodeName(QDir(identity.absolutePath).filePath("0"));
		return (::lstat(native.constData(), &zero) != 0) && (errno == ENOENT);
	}
	if ((identity.activeVersionName != identity.versionName)
		|| !identity.activeVersionDescriptor) {
		return false;
	}
	auto activeDescriptor = struct stat();
	if ((::fstat(identity.activeVersionDescriptor->value, &activeDescriptor)
		 != 0)
		|| !SameCachePathTargetStat(activeDescriptor,
									identity.activeVersionDirectory)) {
		return false;
	}
	auto current = CachePathComponentIdentity{
		.path = identity.activeVersionDirectory.path};
	auto missing = false;
	return ReadCachePathLinkIdentity(identity.activeVersionDirectory.path,
									 &current, &missing)
		   && !missing
		   && SameCachePathLinkIdentity(current,
										identity.activeVersionDirectory);
}

[[nodiscard]] bool CachePathIdentityStillValid(const QString &path,
											   const char *callsite,
											   CachePathIdentity *identity) {
	if (!CachePathRootStillValid(path, identity)
		|| !CachePathActiveVersionStillValid(*identity)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	return true;
}

} // namespace

bool CheckCachePath(const QString &path, const char *callsite) {
	return CheckCachePathImpl(path, callsite, {});
}

bool CheckCachePathAndCaptureIdentity(const QString &path, const char *callsite,
									  CachePathIdentity *identity) {
	if (!identity) {
		return false;
	}
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		*identity = CachePathIdentity();
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	if (!CheckCachePathImpl(path, callsite, {})) {
		return false;
	}
	if (!CaptureCachePathIdentity(path, identity)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	return true;
}

bool CaptureCachePathActiveVersionIdentity(const QString &path,
										   const char *callsite,
										   CachePathIdentity *identity) {
	if (!identity) {
		return false;
	}
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	auto refreshed = *identity;
	if (refreshed.components.empty()
		|| (AbsoluteCachePath(path) != refreshed.absolutePath)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	if (refreshed.rootDescriptor) {
		if (!CachePathRootStillValid(path, &refreshed)) {
			ReportCachePathIdentityRefusal(callsite);
			return false;
		}
	} else {
		const auto absolutePath = AbsoluteCachePath(path);
		if (refreshed.components.back().exists) {
			ReportCachePathIdentityRefusal(callsite);
			return false;
		}
		auto root = CachePathComponentIdentity{.path = absolutePath};
		auto missing = false;
		if (!CheckPath(Operation::OpenDir, absolutePath, callsite)
			|| !ReadCachePathLinkIdentity(absolutePath, &root, &missing)
			|| missing || !ReadCachePathTargetIdentity(absolutePath, &root)
			|| !CaptureCachePathRootDescriptor(absolutePath, root,
											   &refreshed)) {
			ReportCachePathIdentityRefusal(callsite);
			return false;
		}
		refreshed.components.back() = std::move(root);
	}
	if (!CaptureCachePathVersionMarker(&refreshed)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	const auto activePath = ActiveCacheVersionPath(refreshed.absolutePath);
	const auto expectedActivePath
		= QDir(refreshed.absolutePath).filePath(refreshed.versionName);
	if (activePath.isEmpty() || (activePath != expectedActivePath)
		|| !CheckPath(Operation::OpenDir, activePath, callsite)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	if (!CaptureCachePathActiveVersionDirectory(activePath, &refreshed)) {
		ReportCachePathIdentityRefusal(callsite);
		return false;
	}
	*identity = std::move(refreshed);
	return CachePathIdentityStillValid(path, callsite, identity);
}

bool CheckCachePathIdentity(const QString &path, const char *callsite,
							CachePathIdentity *identity) {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	return CachePathIdentityStillValid(path, callsite, identity);
}

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
void ResetCachePathWalkCountForTesting() {
	QMutexLocker lock(&CachePathWalkCountMutex);
	CachePathWalkCount = 0;
	CachePathWalkMainThreadCount = 0;
	CachePathWalkMainThread = QThread::currentThreadId();
}

int CachePathWalkCountForTesting() {
	QMutexLocker lock(&CachePathWalkCountMutex);
	return CachePathWalkCount;
}

int CachePathWalkMainThreadCountForTesting() {
	QMutexLocker lock(&CachePathWalkCountMutex);
	return CachePathWalkMainThreadCount;
}

int RunNotificationSoundProbe() {
	if (!IntegrationTestActive()) {
		return 1;
	}
	const auto directory = NotificationSoundsDirectory();
	if (directory.isEmpty()) {
		return 1;
	}
	const auto id = uint64(getpid());
	const auto destination
		= directory
		  + u"/Teagram_%1.wav"_q.arg(QString::number(id, 16).toUpper());
	if (!CheckPath(Operation::Lstat, destination, Q_FUNC_INFO)
		|| QFileInfo::exists(destination)) {
		return 1;
	}
	auto source = QTemporaryDir(ProfileRoot() + u"/sound-lookup-XXXXXX"_q);
	if (!source.isValid()) {
		return 1;
	}
	const auto wav = QByteArray::fromHex(
		"524946462600000057415645666d74201000000001000100401f0000"
		"803e00000200100064617461020000000000");
	auto cache = Media::Audio::LocalDiskCache(source.path());
	const auto name = cache.name({id, wav});
	if (name.isEmpty()) {
		return 1;
	}
	auto file = QFile(destination);
	const auto readable = CheckPath(Operation::Open, destination, Q_FUNC_INFO)
						  && file.open(QIODevice::ReadOnly)
						  && file.readAll() == wav;
	file.close();
	auto found = false;
	@autoreleasepool {
		const auto nativeName =
			[NSString stringWithUTF8String:name.toUtf8().constData()];
		found = readable && [NSSound soundNamed:nativeName] != nil;
	}
	const auto removed = CheckPath(Operation::Unlink, destination, Q_FUNC_INFO)
						 && QFile::remove(destination);
	if (!found || !removed) {
		fprintf(stderr,
				"Notification sound lookup failed: readable=%d found=%d "
				"removed=%d\n",
				readable, found, removed);
		return 1;
	}
	return 0;
}

bool CheckCachePathForTesting(
	const QString &path, const char *callsite,
	std::function<void(const QString &)> beforeEntryStat) {
	return CheckCachePathImpl(path, callsite, beforeEntryStat);
}

namespace {

[[nodiscard]] bool ProbeMatchesExpectation(int error, bool expectDenied,
										   const char *mode) {
	const auto passed = expectDenied ? (error == EPERM) : (error == 0);
	if (!passed) {
		fprintf(stderr,
				"Seatbelt open probe failed: mode=%s expected_errno=%d "
				"actual_errno=%d\n",
				mode, expectDenied ? EPERM : 0, error);
	}
	return passed;
}

[[nodiscard]] int RunSeatbeltCatProbeImpl(const char *path, bool expectDenied,
										  bool forkExec) {
	if (!IntegrationTestActive() || !path || !*path) {
		return 1;
	}
	int output[2] = {};
	if (::pipe(output) != 0) {
		return 1;
	}
	auto child = pid_t(0);
	auto executable = QByteArray("/bin/cat");
	auto argument = QByteArray(path);
	char *arguments[] = {executable.data(), argument.data(), nullptr};
	if (forkExec) {
		child = ::fork();
		if (child == 0) {
			::close(output[0]);
			const auto sink = ::open("/dev/null", O_WRONLY);
			if (sink < 0 || ::dup2(output[1], STDERR_FILENO) < 0
				|| ::dup2(sink, STDOUT_FILENO) < 0) {
				::_exit(126);
			}
			::close(output[1]);
			::close(sink);
			::execve(executable.constData(), arguments, environ);
			const auto message = "Seatbelt /bin/cat execve failed.\n";
			(void)::write(STDERR_FILENO, message, sizeof(message) - 1);
			::_exit(127);
		}
	} else {
		auto actions = posix_spawn_file_actions_t();
		if (posix_spawn_file_actions_init(&actions) != 0) {
			::close(output[0]);
			::close(output[1]);
			return 1;
		}
		if (posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO)
				!= 0
			|| posix_spawn_file_actions_addclose(&actions, output[0]) != 0
			|| posix_spawn_file_actions_addclose(&actions, output[1]) != 0
			|| posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
												"/dev/null", O_WRONLY, 0)
				   != 0) {
			::close(output[0]);
			::close(output[1]);
			posix_spawn_file_actions_destroy(&actions);
			return 1;
		}
		const auto spawnStatus
			= posix_spawn(&child, executable.constData(), &actions, nullptr,
						  arguments, environ);
		posix_spawn_file_actions_destroy(&actions);
		if (spawnStatus != 0) {
			fprintf(stderr, "Seatbelt /bin/cat posix_spawn failed: %d\n",
					spawnStatus);
			::close(output[0]);
			::close(output[1]);
			return 1;
		}
	}
	::close(output[1]);
	if (child < 0) {
		fprintf(stderr, "Seatbelt /bin/cat process creation failed: errno=%d\n",
				errno);
		::close(output[0]);
		return 1;
	}
	auto diagnostic = QByteArray();
	char buffer[1024] = {};
	while (true) {
		const auto count = ::read(output[0], buffer, sizeof(buffer));
		if (count > 0) {
			diagnostic.append(buffer, int(count));
			continue;
		}
		if (count < 0 && errno == EINTR) {
			continue;
		}
		break;
	}
	::close(output[0]);
	auto status = int(0);
	auto waited = pid_t(0);
	do {
		waited = ::waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	const auto mode = forkExec ? "fork-exec" : "posix_spawn";
	if (waited != child || !WIFEXITED(status)) {
		fprintf(stderr,
				"Seatbelt /bin/cat %s probe failed: wait_status=%d "
				"diagnostic=%s\n",
				mode, status, diagnostic.constData());
		return 1;
	}
	if (expectDenied) {
		const auto catExitedWithError = WEXITSTATUS(status) == 1;
		const auto reportedPermissionError
			= diagnostic.contains(QByteArray(std::strerror(EPERM)));
		if (!catExitedWithError || !reportedPermissionError) {
			fprintf(stderr,
					"Seatbelt /bin/cat %s probe failed: expected=EPERM exit=%d "
					"diagnostic=%s\n",
					mode, WEXITSTATUS(status), diagnostic.constData());
			const auto descriptor = ::open(path, O_RDONLY);
			const auto parentError = (descriptor < 0) ? errno : 0;
			if (descriptor >= 0) {
				::close(descriptor);
			}
			fprintf(stderr, "Seatbelt parent open probe: errno=%d\n",
					parentError);
			const auto policy = State().policy;
			if (policy) {
				const auto profile = policy->SeatbeltProfile();
				fprintf(stderr, "Seatbelt profile:\n%s\n", profile.constData());
			}
			return 1;
		}
		return 0;
	}
	return WEXITSTATUS(status) == 0 && diagnostic.isEmpty() ? 0 : 1;
}

} // namespace

int RunSeatbeltOpenProbe(const char *path, bool expectDenied, bool forkChild) {
	if (!IntegrationTestActive() || !path || !*path) {
		return 1;
	}
	if (!forkChild) {
		const auto descriptor = ::open(path, O_RDONLY);
		const auto error = (descriptor < 0) ? errno : 0;
		if (descriptor >= 0) {
			::close(descriptor);
		}
		return ProbeMatchesExpectation(error, expectDenied, "parent") ? 0 : 1;
	}
	int resultPipe[2] = {};
	if (::pipe(resultPipe) != 0) {
		return 1;
	}
	const auto child = ::fork();
	if (child == 0) {
		::close(resultPipe[0]);
		const auto descriptor = ::open(path, O_RDONLY);
		const auto error = (descriptor < 0) ? errno : 0;
		if (descriptor >= 0) {
			::close(descriptor);
		}
		const auto written = ::write(resultPipe[1], &error, sizeof(error));
		::close(resultPipe[1]);
		::_exit(written == ssize_t(sizeof(error)) ? 0 : 1);
	}
	::close(resultPipe[1]);
	if (child < 0) {
		::close(resultPipe[0]);
		return 1;
	}
	auto error = int(-1);
	auto received = size_t(0);
	while (received < sizeof(error)) {
		const auto count
			= ::read(resultPipe[0], reinterpret_cast<char *>(&error) + received,
					 sizeof(error) - received);
		if (count > 0) {
			received += size_t(count);
			continue;
		}
		if (count < 0 && errno == EINTR) {
			continue;
		}
		break;
	}
	::close(resultPipe[0]);
	auto status = int(0);
	auto waited = pid_t(0);
	do {
		waited = ::waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (received != sizeof(error) || waited != child || !WIFEXITED(status)
		|| WEXITSTATUS(status) != 0) {
		fprintf(stderr,
				"Seatbelt fork open probe failed: bytes=%zu status=%d\n",
				received, status);
		return 1;
	}
	return ProbeMatchesExpectation(error, expectDenied, "fork") ? 0 : 1;
}

int RunSeatbeltCatProbe(const char *path, bool expectDenied) {
	return RunSeatbeltCatProbeImpl(path, expectDenied, false);
}

int RunSeatbeltForkExecCatProbe(const char *path, bool expectDenied) {
	return RunSeatbeltCatProbeImpl(path, expectDenied, true);
}
#endif

bool CheckPair(Operation operation, const QString &first, const QString &second,
			   const char *callsite) {
#ifndef TDESKTOP_TEAGRAM
	if (!IntegrationTestActive()) {
		return true;
	}
#endif // TDESKTOP_TEAGRAM
	auto policy = std::shared_ptr<const MacProtectedPathPolicy>();
	auto anchor = QString();
	{
		auto &state = State();
		QMutexLocker lock(&state.mutex);
		if (!state.ready || !state.policy) {
			ReportInvalidInitialization(u"operation.before-profile"_q);
			return false;
		}
		policy = state.policy;
		anchor = state.profile;
	}
	const auto result = policy->ResolvePair(
		operation, QFile::encodeName(first), QFile::encodeName(second),
		QFile::encodeName(anchor), QString::fromUtf8(callsite));
	if (!result.first.allowed()) {
		ReportRefusal(result.first.refusal);
	}
	if (!result.second.allowed()) {
		ReportRefusal(result.second.refusal);
	}
	return result.allowed();
}

} // namespace Core::MacProtectedPath
