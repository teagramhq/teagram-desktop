/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mac_protected_path_runtime.h"

#include "core/mac_protected_path_access.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutexLocker>

#include <Cocoa/Cocoa.h>
#include <sandbox.h>

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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" char **environ;

namespace Core::MacProtectedPath {
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
				|| QDir::cleanPath(QDir::tempPath())
					   != QDir::cleanPath(temporaryPath)) {
				ReportInvalidInitialization(u"profile.helper-temp"_q);
				return false;
			}
			return true;
		});
	if (!temporaryPrepared) {
		return failForIntegrationTest("helper-temp");
	}

	const auto appSandboxed = AppSandboxed();
	const auto ipcDirectory
		= appSandboxed ? QString::fromUtf8(homes.foundation) + u"/tmp"_q
					   : u"/tmp"_q;
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
	if (IsActive()) {
		return ProfileRoot() + u"/tdata/sounds"_q;
	}
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

} // namespace

bool CheckCachePath(const QString &path, const char *callsite) {
	return CheckCachePathImpl(path, callsite, {});
}

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
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
