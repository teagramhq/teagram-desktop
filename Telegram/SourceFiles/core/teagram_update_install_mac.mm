/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_install.h"
#ifndef TDESKTOP_UNIT_TESTS
#include "core/application.h"
#endif

#include "base/platform/mac/base_utilities_mac.h"
#include "base/platform/base_platform_file_utilities.h"
#include "core/mac_protected_path_access.h"
#include "core/teagram_update_replacement.h"
#include "core/teagram_update_verification.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QThread>

#include <Cocoa/Cocoa.h>
#include <copyfile.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>

namespace Core {
namespace {

constexpr auto kStagingDirectory = ".teagram-update-staging";
constexpr auto kPreviousBundle = ".Teagram.app.previous";
constexpr auto kCommitMarker = ".teagram-update-pending";
constexpr auto kStartedMarker = ".teagram-update-started";
constexpr auto kStartupWaitMs = 30'000;
constexpr auto kDataFolder = "tdata";
constexpr auto kMaximumDataCopies = 100;
constexpr auto kCustomIconFile = "Icon\r";
constexpr auto kFinderInfoAttribute = "com.apple.FinderInfo";
constexpr auto kQuarantineAttribute = "com.apple.quarantine";
constexpr auto kFinderInfoSize = 32;
constexpr auto kFinderFlagsOffset = 8;
constexpr auto kHasCustomIconFlag = 0x04; // High byte of kHasCustomIcon.

struct BundleInfo {
	QString identifier;
	QString executable;
	QString channel;
	quint64 build = 0;
};

struct ShutdownInstall {
	std::shared_ptr<TeagramUpdateCandidate> candidate;
	QString workingDirectory;
	QString confirmedName;
	QString failureText;
};

std::optional<ShutdownInstall> PendingShutdownInstall;

[[nodiscard]] bool IsCanonicalDirectory(const QString &path,
										bool privateDirectory);

[[nodiscard]] QString FromNative(NSString *value) {
	return value ? QFile::decodeName([value fileSystemRepresentation])
				 : QString();
}

[[nodiscard]] QString FromProperty(NSDictionary *properties, NSString *key) {
	const id value = [properties objectForKey:key];
	return [value isKindOfClass:[NSString class]]
			   ? QString::fromUtf8([value UTF8String])
			   : QString();
}

[[nodiscard]] bool ReadBundleInfo(const QString &bundlePath,
								  BundleInfo *result) {
	if (!result) {
		return false;
	}
	const auto contentsPath = QDir(bundlePath).filePath(u"Contents"_q);
	if (!IsCanonicalDirectory(contentsPath, false)) {
		return false;
	}
	const auto plistPath = QDir(contentsPath).filePath(u"Info.plist"_q);
	const auto plistInfo = QFileInfo(plistPath);
	if (!plistInfo.isFile() || plistInfo.isSymLink()
		|| QDir::cleanPath(plistInfo.canonicalFilePath())
			   != QDir::cleanPath(plistInfo.absoluteFilePath())) {
		return false;
	}
	auto properties = [NSDictionary
		dictionaryWithContentsOfFile:Platform::Q2NSString(plistPath)];
	if (![properties isKindOfClass:[NSDictionary class]]) {
		return false;
	}
	const auto identifier = FromProperty(properties, @"CFBundleIdentifier");
	const auto executable = FromProperty(properties, @"CFBundleExecutable");
	const auto channel = FromProperty(properties, @"TeagramUpdateChannel");
	const auto buildText = FromProperty(properties, @"TeagramUpdateBuild");
	auto buildOk = false;
	const auto build = buildText.toULongLong(&buildOk, 10);
	if (identifier.isEmpty() || executable.isEmpty() || !buildOk
		|| QString::number(build) != buildText) {
		return false;
	}
	*result = {identifier, executable, channel, build};
	return true;
}

[[nodiscard]] bool IsCanonicalPath(const QString &path) {
	const auto info = QFileInfo(path);
	const auto canonical = info.canonicalFilePath();
	return info.isDir() && !info.isSymLink() && !canonical.isEmpty()
		   && QDir::cleanPath(canonical)
				  == QDir::cleanPath(info.absoluteFilePath());
}

[[nodiscard]] bool IsCanonicalDirectory(const QString &path,
										bool privateDirectory) {
	if (!IsCanonicalPath(path)) {
		return false;
	}
	struct stat stat = {};
	const auto bytes = QFile::encodeName(QFileInfo(path).canonicalFilePath());
	return ::lstat(bytes.constData(), &stat) == 0 && S_ISDIR(stat.st_mode)
		   && stat.st_uid == ::geteuid()
		   && (!privateDirectory || (stat.st_mode & 0077) == 0);
}

[[nodiscard]] QString CanonicalDirectory(const QString &path) {
	if (!IsCanonicalPath(path) || !IsTeagramUpdateParentWritable(path)) {
		return {};
	}
	return QDir::cleanPath(QFileInfo(path).canonicalFilePath());
}

[[nodiscard]] QString CanonicalBundlePath(const QString &path) {
	if (!IsCanonicalDirectory(path, false)) {
		return {};
	}
	const auto info = QFileInfo(path);
	const auto parent = CanonicalDirectory(info.absolutePath());
	if (parent.isEmpty() || info.fileName() != u"Teagram.app"_q) {
		return {};
	}
	return QDir(parent).filePath(info.fileName());
}

[[nodiscard]] QString CanonicalRunningBundlePath(const QString &path) {
	if (!IsCanonicalDirectory(path, false)) {
		return {};
	}
	const auto info = QFileInfo(path);
	const auto parent = CanonicalDirectory(info.absolutePath());
	if (parent.isEmpty()
		|| (info.fileName() != u"Teagram.app"_q
			&& info.fileName() != u".Teagram.app.previous"_q)) {
		return {};
	}
	return QDir(parent).filePath(info.fileName());
}

[[nodiscard]] bool IsTranslocated(const QString &path) {
	return path.split('/').contains(u"AppTranslocation"_q);
}

[[nodiscard]] bool RunTool(const QString &program, const QStringList &arguments,
						   QByteArray *output = nullptr) {
	auto process = QProcess();
	process.start(program, arguments);
	if (!process.waitForStarted() || !process.waitForFinished(30'000)) {
		if (process.state() != QProcess::NotRunning) {
			process.kill();
			process.waitForFinished();
		}
		return false;
	}
	if (output) {
		*output
			= process.readAllStandardOutput() + process.readAllStandardError();
	}
	return process.exitStatus() == QProcess::NormalExit
		   && process.exitCode() == 0;
}

[[nodiscard]] bool HasArchitecture(const QString &executable) {
	auto output = QByteArray();
	return RunTool(u"/usr/bin/lipo"_q, {u"-archs"_q, executable}, &output)
		   && QString::fromLatin1(output).trimmed() == u"arm64"_q;
}

[[nodiscard]] bool HasCleanCodeSeal(const QString &bundlePath) {
	if (!RunTool(u"/usr/bin/codesign"_q,
				 {u"--verify"_q, u"--deep"_q, u"--strict"_q, bundlePath})) {
		return false;
	}
	auto display = QByteArray();
	if (!RunTool(u"/usr/bin/codesign"_q,
				 {u"--display"_q, u"--verbose=4"_q, bundlePath}, &display)) {
		return false;
	}
	return display.contains("Identifier=io.teagram.desktop\n");
}

[[nodiscard]] bool IsRegularExecutable(const QString &path) {
	const auto info = QFileInfo(path);
	return info.isFile() && !info.isSymLink() && info.isExecutable();
}

[[nodiscard]] bool
ValidateBundle(const QString &bundlePath, const QString &expectedChannel,
			   quint64 expectedBuild, bool verifySeal,
			   const std::function<bool(const QString &)> &hasArchitecture = {},
			   const std::function<bool(const QString &)> &hasCleanCodeSeal
			   = {}) {
	if (CanonicalBundlePath(bundlePath) != QDir::cleanPath(bundlePath)) {
		return false;
	}
	const auto executable
		= QDir(bundlePath).filePath(u"Contents/MacOS/Teagram"_q);
	const auto contents = QDir(bundlePath).filePath(u"Contents"_q);
	const auto macos = QDir(contents).filePath(u"MacOS"_q);
	const auto resources = QDir(bundlePath).filePath(u"Contents/Info.plist"_q);
	if (!IsCanonicalDirectory(contents, false)
		|| !IsCanonicalDirectory(macos, false) || !QFileInfo(resources).isFile()
		|| QFileInfo(resources).isSymLink()
		|| !IsRegularExecutable(executable)) {
		return false;
	}
	auto info = BundleInfo();
	if (!ReadBundleInfo(bundlePath, &info)
		|| info.identifier != u"io.teagram.desktop"_q
		|| info.executable != u"Teagram"_q || info.build != expectedBuild
		|| (!expectedChannel.isEmpty() && info.channel != expectedChannel)
		|| !(hasArchitecture ? hasArchitecture(executable)
							 : HasArchitecture(executable))) {
		return false;
	}
	return !verifySeal
		   || (hasCleanCodeSeal ? hasCleanCodeSeal(bundlePath)
								: HasCleanCodeSeal(bundlePath));
}

[[nodiscard]] bool OwnedWritableTarget(const QString &bundlePath) {
	const auto parent = QFileInfo(bundlePath).absolutePath();
	const auto canonical = CanonicalBundlePath(bundlePath);
	if (canonical.isEmpty() || canonical != QDir::cleanPath(bundlePath)
		|| IsTranslocated(canonical)) {
		return false;
	}
	struct stat bundleInfo = {};
	struct stat parentInfo = {};
	const auto bundleBytes = QFile::encodeName(canonical);
	const auto parentBytes = QFile::encodeName(parent);
	return ::lstat(bundleBytes.constData(), &bundleInfo) == 0
		   && S_ISDIR(bundleInfo.st_mode) && bundleInfo.st_uid == ::geteuid()
		   && ::lstat(parentBytes.constData(), &parentInfo) == 0
		   && bundleInfo.st_dev == parentInfo.st_dev
		   && ::access(bundleBytes.constData(), W_OK) == 0
		   && IsTeagramUpdateParentWritable(parent);
}

[[nodiscard]] bool EnsurePrivateStage(const QString &parent,
									  const QString &staging) {
	const auto parentBytes = QFile::encodeName(parent);
	const auto name = QFile::encodeName(QFileInfo(staging).fileName());
	if (parentBytes.isEmpty() || name.isEmpty()) {
		return false;
	}
	auto parentDescriptor
		= ::open(parentBytes.constData(),
				 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (parentDescriptor < 0) {
		return false;
	}
	const auto created
		= ::mkdirat(parentDescriptor, name.constData(), 0700) == 0;
	if (!created) {
		::close(parentDescriptor);
		return false;
	}
	struct stat stageInfo = {};
	const auto valid = ::fstatat(parentDescriptor, name.constData(), &stageInfo,
								 AT_SYMLINK_NOFOLLOW)
						   == 0
					   && S_ISDIR(stageInfo.st_mode)
					   && stageInfo.st_uid == ::geteuid()
					   && (stageInfo.st_mode & 0077) == 0;
	::close(parentDescriptor);
	return valid;
}

[[nodiscard]] QString ChannelName(TeagramUpdateChannel channel) {
	return channel == TeagramUpdateChannel::Dev ? u"dev"_q : u"main"_q;
}

[[nodiscard]] QString InstalledChannel() {
	return QString::fromLatin1(kTeagramUpdateChannel.data(),
							   qsizetype(kTeagramUpdateChannel.size()));
}

[[nodiscard]] bool OtherInstanceRunning() {
	auto applications = [NSRunningApplication
		runningApplicationsWithBundleIdentifier:@"io.teagram.desktop"];
	for (NSRunningApplication *application in applications) {
		if ([application processIdentifier] != ::getpid()
			&& ![application isTerminated]) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool WriteMarker(const QString &parent, const char *name,
							   quint64 build) {
	return WriteTeagramUpdateMarker(parent, name, build);
}

[[nodiscard]] std::optional<quint64> ReadMarker(const QString &parent,
												const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(parentBytes.constData(),
							O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory < 0) {
		return std::nullopt;
	}
	const auto descriptor
		= ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	::close(directory);
	if (descriptor < 0) {
		return std::nullopt;
	}
	struct stat info = {};
	auto success = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)
				   && info.st_uid == ::geteuid() && info.st_nlink == 1
				   && (info.st_mode & 0077) == 0 && info.st_size > 0
				   && info.st_size <= 32;
	auto bytes = QByteArray();
	if (success) {
		bytes.resize(qsizetype(info.st_size));
		auto offset = qsizetype(0);
		while (offset < bytes.size()) {
			const auto read = ::read(descriptor, bytes.data() + offset,
									 size_t(bytes.size() - offset));
			if (read < 0 && errno == EINTR) {
				continue;
			}
			if (read <= 0) {
				success = false;
				break;
			}
			offset += qsizetype(read);
		}
	}
	::close(descriptor);
	if (!success || !bytes.endsWith('\n')) {
		return std::nullopt;
	}
	bytes.chop(1);
	const auto text = QString::fromLatin1(bytes);
	auto ok = false;
	const auto result = text.toULongLong(&ok, 10);
	return ok && QString::number(result) == text
			   ? std::optional<quint64>(result)
			   : std::nullopt;
}

[[nodiscard]] std::optional<bool> MarkerExists(const QString &parent,
											   const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(parentBytes.constData(),
							O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory < 0) {
		return std::nullopt;
	}
	struct stat info = {};
	const auto result = ::fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW);
	const auto error = errno;
	::close(directory);
	if (result == 0) {
		return true;
	}
	return error == ENOENT ? std::optional<bool>(false) : std::nullopt;
}

[[nodiscard]] bool RemoveMarker(const QString &parent, const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(parentBytes.constData(),
							O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory < 0) {
		return false;
	}
	struct stat info = {};
	if (::fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
		const auto missing = errno == ENOENT;
		::close(directory);
		return missing;
	}
	const auto removed = S_ISREG(info.st_mode) && info.st_uid == ::geteuid()
						 && info.st_nlink == 1 && (info.st_mode & 0077) == 0
						 && ::unlinkat(directory, name, 0) == 0
						 && ::fsync(directory) == 0;
	::close(directory);
	return removed;
}

[[nodiscard]] bool CarryCustomIconFile(const QString &installed,
									   const QString &candidate) {
	const auto source = QFile::encodeName(installed) + '/' + kCustomIconFile;
	const auto target = QFile::encodeName(candidate) + '/' + kCustomIconFile;
	struct stat info = {};
	if (::lstat(source.constData(), &info) != 0) {
		return errno == ENOENT;
	} else if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid()) {
		return false;
	}
	if (::copyfile(source.constData(), target.constData(), nullptr,
				   COPYFILE_DATA | COPYFILE_XATTR | COPYFILE_EXCL
					   | COPYFILE_NOFOLLOW)
		!= 0) {
		if (errno != EEXIST) {
			::unlink(target.constData());
		}
		return false;
	}
	if (::removexattr(target.constData(), kQuarantineAttribute, XATTR_NOFOLLOW)
			!= 0
		&& errno != ENOATTR) {
		::unlink(target.constData());
		return false;
	}
	return true;
}

[[nodiscard]] bool CarryCustomIconFlag(const QString &installed,
									   const QString &candidate) {
	const auto source = QFile::encodeName(installed);
	const auto target = QFile::encodeName(candidate);
	auto installedInfo = std::array<char, kFinderInfoSize>();
	const auto read = ::getxattr(source.constData(), kFinderInfoAttribute,
								 installedInfo.data(), installedInfo.size(), 0,
								 XATTR_NOFOLLOW);
	if (read < 0) {
		return errno == ENOATTR;
	} else if (read != kFinderInfoSize
			   || !(installedInfo[kFinderFlagsOffset] & kHasCustomIconFlag)) {
		return true;
	}
	auto candidateInfo = std::array<char, kFinderInfoSize>();
	const auto existing = ::getxattr(target.constData(), kFinderInfoAttribute,
									 candidateInfo.data(), candidateInfo.size(),
									 0, XATTR_NOFOLLOW);
	if (existing != kFinderInfoSize) {
		if (existing >= 0 || errno != ENOATTR) {
			return false;
		}
		candidateInfo.fill(0);
	}
	candidateInfo[kFinderFlagsOffset] |= kHasCustomIconFlag;
	return ::setxattr(target.constData(), kFinderInfoAttribute,
					  candidateInfo.data(), candidateInfo.size(), 0,
					  XATTR_NOFOLLOW)
		   == 0;
}

// A custom Finder icon lives outside the sealed Contents directory, so
// it is carried over only after the candidate's code seal was verified.
void CarryCustomIcon(const QString &installed, const QString &candidate) {
	if (!CarryCustomIconFile(installed, candidate)) {
		LOG(("Teagram update could not keep the custom icon file."));
	}
	if (!CarryCustomIconFlag(installed, candidate)) {
		LOG(("Teagram update could not keep the custom icon flag."));
	}
}

[[nodiscard]] TeagramUpdateInstallResult
InstallResult(TeagramUpdateInstallReason reason, const QString &previous = {},
			  bool replacementCommitted = false) {
	return {reason, previous, false, replacementCommitted};
}

[[nodiscard]] TeagramUpdateStartupRecoveryResult
RecoverAtStartup(const QString &installed, const QString &staging,
				 const QString &previous, const QString &running,
				 bool discardPrevious,
				 const std::function<bool()> &otherInstanceRunning = {}) {
	auto lock = TeagramUpdateInstallLock(installed);
	if (!lock.acquired()) {
		return {
			TeagramUpdateStartupRecoveryReason::Busy,
			{},
		};
	}
	const auto previousAllowed = MacProtectedPath::CheckExternalPathForUse(
		MacProtectedPath::Operation::RecursiveDelete, previous,
		"core.teagram-update.recover-previous");
	const auto targetAllowed = MacProtectedPath::CheckExternalPathForUse(
		MacProtectedPath::Operation::Rename, installed,
		"core.teagram-update.recover-target");
	const auto stageAllowed = MacProtectedPath::CheckExternalPathForUse(
		MacProtectedPath::Operation::RecursiveDelete, staging,
		"core.teagram-update.recover-staging");
	if (!previousAllowed || !targetAllowed || !stageAllowed
		|| !MacProtectedPath::CheckPair(MacProtectedPath::Operation::Rename,
										staging, previous,
										"core.teagram-update.recover-pair")) {
		return {
			TeagramUpdateStartupRecoveryReason::Failed,
			{},
		};
	}
	const auto stageBundle = QDir(staging).filePath(u"Teagram.app"_q);
	auto stagePrevious = false;
	const auto parent = QFileInfo(installed).absolutePath();
	const auto commitMarkerExists = MarkerExists(parent, kCommitMarker);
	if (!commitMarkerExists) {
		return {
			TeagramUpdateStartupRecoveryReason::Failed,
			{},
		};
	}
	const auto pending = *commitMarkerExists ? ReadMarker(parent, kCommitMarker)
											 : std::optional<quint64>();
	if (*commitMarkerExists && !pending) {
		return {
			TeagramUpdateStartupRecoveryReason::Failed,
			{},
		};
	}
	if (pending && QFileInfo(installed).exists()
		&& IsCanonicalDirectory(staging, true)
		&& IsCanonicalDirectory(stageBundle, false)) {
		auto installedInfo = BundleInfo();
		auto stagedInfo = BundleInfo();
		if (!ReadBundleInfo(installed, &installedInfo)
			|| !ReadBundleInfo(stageBundle, &stagedInfo)
			|| installedInfo.identifier != u"io.teagram.desktop"_q
			|| stagedInfo.identifier != u"io.teagram.desktop"_q) {
			return {
				TeagramUpdateStartupRecoveryReason::Failed,
				{},
			};
		}
		stagePrevious = installedInfo.build == *pending
						&& stagedInfo.build != installedInfo.build;
		if (stagedInfo.build != *pending && !stagePrevious) {
			return {
				TeagramUpdateStartupRecoveryReason::Failed,
				{},
			};
		}
	}
	const auto noOtherInstance
		= discardPrevious
		  && !(otherInstanceRunning ? otherInstanceRunning()
									: OtherInstanceRunning());
	return RecoverTeagramUpdateReplacement(lock, installed, staging, previous,
										   running, stagePrevious,
										   discardPrevious, noOtherInstance);
}

[[nodiscard]] bool RequestApplicationRestart() {
	auto *application = QCoreApplication::instance();
	if (!application) {
		return false;
	}
	const auto quit = [application] {
#ifndef TDESKTOP_UNIT_TESTS
		cSetRestarting(true);
#endif // !TDESKTOP_UNIT_TESTS
		application->quit();
	};
	if (QThread::currentThread() == application->thread()) {
		quit();
		return true;
	}
	return QMetaObject::invokeMethod(application, quit, Qt::QueuedConnection);
}

enum class StartupWaitResult {
	Acknowledged,
	PendingRemoved,
	Failed,
};

[[nodiscard]] StartupWaitResult WaitForStartup(quint64 build,
											   const QString &parent) {
	auto timer = QElapsedTimer();
	timer.start();
	while (timer.elapsed() < kStartupWaitMs) {
		if (const auto started = ReadMarker(parent, kStartedMarker);
			started && *started == build) {
			return StartupWaitResult::Acknowledged;
		}
		const auto pending = MarkerExists(parent, kCommitMarker);
		if (!pending) {
			return StartupWaitResult::Failed;
		} else if (!*pending) {
			return StartupWaitResult::PendingRemoved;
		}
		QThread::msleep(100);
	}
	return StartupWaitResult::Failed;
}

[[nodiscard]] bool LaunchBundle(const QString &bundlePath,
								const QStringList &arguments,
								pid_t *processId) {
	auto semaphore = dispatch_semaphore_create(0);
	__block NSRunningApplication *launched = nil;
	__block NSError *launchError = nil;
	auto configuration = [NSWorkspaceOpenConfiguration configuration];
	configuration.createsNewApplicationInstance = YES;
	NSMutableArray<NSString *> *nativeArguments =
		[NSMutableArray arrayWithCapacity:arguments.size()];
	for (const auto &argument : arguments) {
		[nativeArguments addObject:Platform::Q2NSString(argument)];
	}
	configuration.arguments = nativeArguments;
	const auto url = [NSURL fileURLWithPath:Platform::Q2NSString(bundlePath)
								isDirectory:YES];
	[[NSWorkspace sharedWorkspace]
		openApplicationAtURL:url
			   configuration:configuration
		   completionHandler:^(NSRunningApplication *application,
							   NSError *error) {
			 launched = application;
			 launchError = error;
			 dispatch_semaphore_signal(semaphore);
		   }];
	const auto deadline = dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC);
	if (dispatch_semaphore_wait(semaphore, deadline) != 0 || !launched
		|| launchError) {
		return false;
	}
	if (processId) {
		*processId = [launched processIdentifier];
	}
	return true;
}

[[nodiscard]] bool ProcessIsRunning(pid_t processId) {
	if (processId <= 0) {
		return false;
	}
	if (::kill(processId, 0) == 0) {
		return true;
	}
	return errno != ESRCH;
}

[[nodiscard]] bool WaitForProcessExit(pid_t processId, int timeoutMs) {
	if (processId <= 0) {
		return true;
	}
	auto timer = QElapsedTimer();
	timer.start();
	while (ProcessIsRunning(processId) && timer.elapsed() < timeoutMs) {
		QThread::msleep(100);
	}
	return !ProcessIsRunning(processId);
}

[[nodiscard]] bool StopLaunchedBundle(pid_t processId) {
	if (processId <= 0) {
		return true;
	}
	auto application = [NSRunningApplication
		runningApplicationWithProcessIdentifier:processId];
	if (application && ![application isTerminated]) {
		[application terminate];
	}
	if (WaitForProcessExit(processId, 5'000)) {
		return true;
	}
	application = [NSRunningApplication
		runningApplicationWithProcessIdentifier:processId];
	if (application && ![application isTerminated]) {
		[application forceTerminate];
	}
	return WaitForProcessExit(processId, 5'000);
}

[[nodiscard]] TeagramUpdateRelaunchResult RelaunchPendingUpdate(
	const QString &currentBundlePath, const QString &dataFile,
	const std::function<bool(const QString &, const QStringList &, pid_t *)>
		&launchBundle,
	const std::function<bool()> &otherInstanceRunning = {},
	const std::function<void()> &prepareForLaunch = {},
	const std::function<bool()> &candidateStartupAcknowledged = {},
	const std::function<bool(pid_t)> &stopCandidate = {}) {
	const auto current = CanonicalRunningBundlePath(currentBundlePath);
	if (current.isEmpty() || current != QDir::cleanPath(currentBundlePath)) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	const auto parent = QFileInfo(current).absolutePath();
	const auto installed = QDir(parent).filePath(u"Teagram.app"_q);
	const auto previous
		= QDir(parent).filePath(QString::fromLatin1(kPreviousBundle));
	if (CanonicalBundlePath(installed) != QDir::cleanPath(installed)
		|| !MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::Read, installed,
			"core.teagram-update.relaunch")) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	const auto commitMarkerExists = MarkerExists(parent, kCommitMarker);
	if (!commitMarkerExists) {
		return TeagramUpdateRelaunchResult::Failed;
	} else if (!*commitMarkerExists) {
		return TeagramUpdateRelaunchResult::NotPending;
	}
	const auto pending = ReadMarker(parent, kCommitMarker);
	if (!pending) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	if (!RemoveMarker(parent, kStartedMarker)) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	auto arguments = QStringList();
	if (dataFile != u"data"_q) {
		arguments = {u"-key"_q, dataFile};
	}
	if (prepareForLaunch) {
		prepareForLaunch();
	}
	auto processId = pid_t(0);
	if (launchBundle(installed, arguments, &processId)) {
		const auto startup = candidateStartupAcknowledged
								 ? (candidateStartupAcknowledged()
										? StartupWaitResult::Acknowledged
										: StartupWaitResult::Failed)
								 : WaitForStartup(*pending, parent);
		if (startup == StartupWaitResult::Acknowledged) {
			return RemoveMarker(parent, kCommitMarker)
						   && RemoveMarker(parent, kStartedMarker)
					   ? TeagramUpdateRelaunchResult::CandidateStarted
					   : TeagramUpdateRelaunchResult::Failed;
		} else if (startup == StartupWaitResult::PendingRemoved) {
			return TeagramUpdateRelaunchResult::CurrentBundleStarted;
		}
	}
	const auto candidateStopped = stopCandidate ? stopCandidate(processId)
												: StopLaunchedBundle(processId);
	if (!candidateStopped) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	if (otherInstanceRunning ? otherInstanceRunning()
							 : OtherInstanceRunning()) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	const auto canonicalPrevious = CanonicalRunningBundlePath(previous);
	auto previousInfo = BundleInfo();
	if (canonicalPrevious != QDir::cleanPath(previous)
		|| !MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::Read, previous,
			"core.teagram-update.fallback")
		|| !ReadBundleInfo(canonicalPrevious, &previousInfo)
		|| previousInfo.identifier != u"io.teagram.desktop"_q
		|| previousInfo.executable != u"Teagram"_q
		|| previousInfo.build == *pending) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	processId = 0;
	return launchBundle(canonicalPrevious, arguments, &processId)
			   ? TeagramUpdateRelaunchResult::PreviousStarted
			   : TeagramUpdateRelaunchResult::Failed;
}

[[nodiscard]] bool CompleteTeagramUpdateStartupForBundle(
	const QString &bundlePath,
	const std::function<bool()> &otherInstanceRunning = {}) {
	const auto running = CanonicalRunningBundlePath(bundlePath);
	if (running.isEmpty()) {
		return false;
	}
	const auto parent = QFileInfo(running).absolutePath();
	const auto installed = QDir(parent).filePath(u"Teagram.app"_q);
	const auto staging
		= QDir(parent).filePath(QString::fromLatin1(kStagingDirectory));
	const auto previous
		= QDir(parent).filePath(QString::fromLatin1(kPreviousBundle));
	const auto pending = ReadMarker(parent, kCommitMarker);
	auto info = BundleInfo();
	if (!ReadBundleInfo(running, &info)
		|| info.identifier != u"io.teagram.desktop"_q
		|| info.executable != u"Teagram"_q) {
		return false;
	}
	const auto runningIsInstalled = running == QDir::cleanPath(installed);
	const auto startupAcknowledged
		= runningIsInstalled && pending && *pending == info.build;
	const auto recovered
		= RecoverAtStartup(installed, staging, previous, running, false);
	if (recovered.reason == TeagramUpdateStartupRecoveryReason::Failed
		|| recovered.reason == TeagramUpdateStartupRecoveryReason::Busy) {
		LOG(("Teagram update recovery failed: reason=%1")
				.arg(static_cast<int>(recovered.reason)));
		return false;
	}
	if (runningIsInstalled && pending && *pending != info.build) {
		if (!RemoveMarker(parent, kStartedMarker)
			|| !RemoveMarker(parent, kCommitMarker)) {
			LOG(("Teagram update pending marker could not be cleared."));
		}
		return false;
	}
	if (!startupAcknowledged
		|| !WriteMarker(parent, kStartedMarker, info.build)) {
		return false;
	}
	const auto relaunchWaiting = otherInstanceRunning ? otherInstanceRunning()
													  : OtherInstanceRunning();
	if (!relaunchWaiting
		&& (!RemoveMarker(parent, kCommitMarker)
			|| !RemoveMarker(parent, kStartedMarker))) {
		LOG(("Teagram update markers could not be cleared after startup."));
	}
	return true;
}

[[nodiscard]] std::optional<bool> PathTaken(const QString &path) {
	struct stat info = {};
	if (::lstat(QFile::encodeName(path).constData(), &info) == 0) {
		return true;
	}
	return (errno == ENOENT) ? std::optional<bool>(false) : std::nullopt;
}

[[nodiscard]] bool SetOwnerOnlyDataCopyAccess(const QString &path) {
	const auto empty = ::acl_init(0);
	if (!empty) {
		return false;
	}
	const auto bytes = QFile::encodeName(path);
	const auto cleared
		= ::acl_set_file(bytes.constData(), ACL_TYPE_EXTENDED, empty) == 0;
	::acl_free(empty);
	return cleared && ::chmod(bytes.constData(), 0700) == 0;
}

[[nodiscard]] bool CopyDataFolder(const QString &source,
								  const QString &target) {
	const auto sourceBytes = QFile::encodeName(source);
	const auto targetBytes = QFile::encodeName(target);
	const auto contents
		= QDir(target).filePath(QString::fromLatin1(kDataFolder));
	const auto contentsBytes = QFile::encodeName(contents);
	const auto flags = COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW;
	for (const auto extra :
		 {copyfile_flags_t(COPYFILE_CLONE), copyfile_flags_t(0)}) {
		if (::mkdir(targetBytes.constData(), 0700) != 0) {
			return false;
		}
		if (SetOwnerOnlyDataCopyAccess(target)
			&& ::copyfile(sourceBytes.constData(), contentsBytes.constData(),
						  nullptr, flags | extra)
				   == 0
			&& SetOwnerOnlyDataCopyAccess(contents)) {
			return true;
		}
		if (!MacProtectedPath::CheckPath(
				MacProtectedPath::Operation::RecursiveDelete, target,
				"core.teagram-update.data-copy-partial")
			|| !QDir(target).removeRecursively()) {
			return false;
		}
	}
	return false;
}

[[nodiscard]] bool HasPreviousBundleBeside(const QString &installed) {
	const auto previous = QDir(QFileInfo(installed).absolutePath())
							  .filePath(QString::fromLatin1(kPreviousBundle));
	return CanonicalRunningBundlePath(previous) == QDir::cleanPath(previous);
}

} // namespace

TeagramUpdateInstallLocation CheckTeagramUpdateInstallLocation() {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return TeagramUpdateInstallLocation::Unwritable;
		}
		const auto bundle = QFileInfo(FromNative([mainBundle bundlePath]));
		const auto parent = bundle.absolutePath();
		if (bundle.fileName() != u"Teagram.app"_q
			|| !IsCanonicalPath(bundle.absoluteFilePath())
			|| !IsCanonicalPath(parent)) {
			return TeagramUpdateInstallLocation::Unwritable;
		}
		const auto installed = QDir::cleanPath(bundle.canonicalFilePath());
		if (IsTranslocated(installed)) {
			return TeagramUpdateInstallLocation::Translocated;
		}
		return ClassifyTeagramUpdateInstallLocation(
			ReadTeagramUpdateInstallLocationFacts(installed));
	}
}

bool HasPreviousTeagramUpdateBundle() {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return false;
		}
		return HasPreviousBundleBeside(FromNative([mainBundle bundlePath]));
	}
}

void ShowPreviousTeagramUpdateBundleInFinder() {
	@autoreleasepool {
		if (!HasPreviousTeagramUpdateBundle()) {
			return;
		}
		const auto mainBundle = [NSBundle mainBundle];
		const auto installed = FromNative([mainBundle bundlePath]);
		const auto parent = QFileInfo(installed).absolutePath();
		const auto previous
			= QDir(parent).filePath(QString::fromLatin1(kPreviousBundle));
		[[NSWorkspace sharedWorkspace] selectFile:Platform::Q2NSString(previous)
						 inFileViewerRootedAtPath:Platform::Q2NSString(parent)];
	}
}

QString TeagramUpdateDataCopyName(const QString &workingDirectory,
								  quint64 runningBuild) {
	const auto root = QDir(workingDirectory);
	const auto base = QString::fromLatin1(kDataFolder) + u"-dev-"_q
					  + QString::number(runningBuild);
	for (auto index = 1; index <= kMaximumDataCopies; ++index) {
		const auto name
			= (index == 1) ? base : (base + u"-"_q + QString::number(index));
		const auto targetTaken = PathTaken(root.filePath(name));
		const auto partialTaken
			= PathTaken(root.filePath(u"."_q + name + u".partial"_q));
		if (!targetTaken || !partialTaken) {
			return {};
		} else if (!*targetTaken && !*partialTaken) {
			return name;
		}
	}
	return {};
}

bool ScheduleOlderTeagramUpdateAfterShutdown(
	std::shared_ptr<TeagramUpdateCandidate> candidate,
	const QString &workingDirectory, const QString &confirmedName,
	const QString &failureText) {
	if (PendingShutdownInstall || !candidate || !candidate->downloaded()
		|| !candidate->olderThanInstalled() || confirmedName.isEmpty()) {
		return false;
	}
	PendingShutdownInstall = ShutdownInstall{
		std::move(candidate), workingDirectory, confirmedName, failureText};
	return true;
}

bool KeepTeagramUpdateDataCopy(const QString &workingDirectory,
							   quint64 runningBuild,
							   const QString &confirmedName) {
	const auto root = QDir(workingDirectory);
	const auto source = root.filePath(QString::fromLatin1(kDataFolder));
	struct stat info = {};
	if (::lstat(QFile::encodeName(source).constData(), &info) != 0) {
		return (errno == ENOENT);
	} else if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
		return false;
	}
	const auto base = QString::fromLatin1(kDataFolder) + u"-dev-"_q
					  + QString::number(runningBuild);
	for (auto index = 1; index <= kMaximumDataCopies; ++index) {
		const auto name
			= (index == 1) ? base : (base + u"-"_q + QString::number(index));
		if (!confirmedName.isEmpty() && name != confirmedName) {
			continue;
		}
		const auto target = root.filePath(name);
		const auto partial = root.filePath(u"."_q + name + u".partial"_q);
		const auto contents
			= QDir(partial).filePath(QString::fromLatin1(kDataFolder));
		const auto targetTaken = PathTaken(target);
		const auto partialTaken = PathTaken(partial);
		if (!targetTaken || !partialTaken) {
			return false;
		} else if (*targetTaken || *partialTaken) {
			if (!confirmedName.isEmpty()) {
				return false;
			}
			continue;
		}
		if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Copy,
										 source,
										 "core.teagram-update.data-copy")
			|| !MacProtectedPath::CheckPath(MacProtectedPath::Operation::Mkdir,
											partial,
											"core.teagram-update.data-copy")
			|| !MacProtectedPath::CheckPair(MacProtectedPath::Operation::Rename,
											contents, target,
											"core.teagram-update.data-copy")
			|| !CopyDataFolder(source, partial)) {
			LOG(("Teagram update could not copy the data folder."));
			return false;
		}
		if (::renamex_np(QFile::encodeName(contents).constData(),
						 QFile::encodeName(target).constData(), RENAME_EXCL)
			!= 0) {
			LOG(("Teagram update could not keep the data copy."));
			if (MacProtectedPath::CheckPath(
					MacProtectedPath::Operation::RecursiveDelete, partial,
					"core.teagram-update.data-copy-partial")) {
				QDir(partial).removeRecursively();
			}
			return false;
		}
		if (!QDir().rmdir(partial)) {
			LOG(("Teagram update could not remove the empty data copy staging "
				 "folder."));
			return false;
		}
		LOG(("Teagram update kept a copy of the data folder: %1").arg(name));
		return true;
	}
	return false;
}

namespace {

[[nodiscard]] std::optional<TeagramUpdateInstallResult>
InstallScheduledOlderTeagramUpdate(
#ifdef TDESKTOP_UNIT_TESTS
	TeagramUpdateInstallerTestHooks hooks = {}
#endif
) {
	if (!PendingShutdownInstall) {
		return std::nullopt;
	}
#ifdef TDESKTOP_UNIT_TESTS
	const auto applicationRunning
		= hooks.applicationRunning && hooks.applicationRunning();
#else
	const auto applicationRunning = IsAppLaunched();
#endif
	if (applicationRunning) {
		return InstallResult(TeagramUpdateInstallReason::Busy);
	}
	const auto pending = std::exchange(PendingShutdownInstall, std::nullopt);
#ifdef TDESKTOP_UNIT_TESTS
	const auto otherInstance = hooks.otherInstanceRunning
								   ? hooks.otherInstanceRunning()
								   : OtherInstanceRunning();
#else
	const auto otherInstance = OtherInstanceRunning();
#endif
	if (otherInstance) {
		return InstallResult(TeagramUpdateInstallReason::Busy);
	}
	if (!KeepTeagramUpdateDataCopy(pending->workingDirectory,
								   pending->candidate->installedBuild(),
								   pending->confirmedName)) {
		return InstallResult(TeagramUpdateInstallReason::DataCopyFailed);
	}
#ifdef TDESKTOP_UNIT_TESTS
	auto installer = TeagramUpdateInstaller(std::move(hooks));
#else
	auto installer = TeagramUpdateInstaller();
#endif
	return installer.install(
		*pending->candidate, {},
		[](const std::function<bool()> &) { return true; });
}

} // namespace

#ifdef TDESKTOP_UNIT_TESTS
std::optional<TeagramUpdateInstallResult>
InstallScheduledOlderTeagramUpdateForTesting(
	TeagramUpdateInstallerTestHooks hooks) {
	return InstallScheduledOlderTeagramUpdate(std::move(hooks));
}

TeagramUpdateStartupRecoveryResult
RecoverTeagramUpdateInstallerAtStartupForTesting(
	const QString &installed, const QString &staging, const QString &previous,
	const QString &running, const std::function<bool()> &otherInstanceRunning) {
	return RecoverAtStartup(installed, staging, previous, running, false,
							otherInstanceRunning);
}

bool CompleteTeagramUpdateStartupForTesting(
	const QString &runningBundlePath,
	const std::function<bool()> &otherInstanceRunning) {
	@autoreleasepool {
		return CompleteTeagramUpdateStartupForBundle(runningBundlePath,
													 otherInstanceRunning);
	}
}

bool HasPreviousTeagramUpdateBundleForTesting(
	const QString &installedBundlePath) {
	return HasPreviousBundleBeside(installedBundlePath);
}

TeagramUpdateRelaunchResult RelaunchTeagramUpdateAfterShutdownForTesting(
	const QString &currentBundlePath, const QString &dataFile,
	const std::function<bool(const QString &, const QStringList &, qint64 *)>
		&launchBundle,
	const std::function<bool()> &otherInstanceRunning,
	const std::function<void()> &prepareForLaunch,
	const std::function<bool()> &candidateStartupAcknowledged,
	const std::function<bool(qint64)> &stopCandidate) {
	@autoreleasepool {
		return RelaunchPendingUpdate(
			currentBundlePath, dataFile,
			[&](const QString &path, const QStringList &arguments,
				pid_t *processId) {
				qint64 testProcessId = 0;
				const auto launched
					= launchBundle
					  && launchBundle(path, arguments, &testProcessId);
				if (processId) {
					*processId = pid_t(testProcessId);
				}
				return launched;
			},
			otherInstanceRunning, prepareForLaunch,
			candidateStartupAcknowledged,
			[&](pid_t processId) {
				return stopCandidate ? stopCandidate(qint64(processId))
									 : StopLaunchedBundle(processId);
			});
	}
}
#endif // TDESKTOP_UNIT_TESTS

void TeagramUpdateInstaller::cancel() {
	_cancelled.store(true, std::memory_order_relaxed);
}

TeagramUpdateInstallResult
TeagramUpdateInstaller::install(const TeagramUpdateCandidate &candidate,
								TeagramUpdateInstallProgressHandler progress,
								TeagramUpdateBeforeRelaunch beforeRelaunch) {
	if (_started.exchange(true, std::memory_order_relaxed)) {
		return InstallResult(TeagramUpdateInstallReason::Busy);
	}
	const auto cancelled
		= [&] { return _cancelled.load(std::memory_order_relaxed); };
	const auto emit = [&](TeagramUpdateInstallStage stage, int value) {
		if (progress) {
			progress(stage, std::clamp(value, 0, 100));
		}
	};
#ifdef TDESKTOP_UNIT_TESTS
	const auto hasArchitecture = [this](const QString &path) {
		if (_testHooks && _testHooks->hasArchitecture) {
			return _testHooks->hasArchitecture(path);
		}
		return HasArchitecture(path);
	};
	const auto hasCleanCodeSeal = [this](const QString &path) {
		if (_testHooks && _testHooks->hasCleanCodeSeal) {
			return _testHooks->hasCleanCodeSeal(path);
		}
		return HasCleanCodeSeal(path);
	};
	const auto otherInstanceRunning = [this] {
		if (_testHooks && _testHooks->otherInstanceRunning) {
			return _testHooks->otherInstanceRunning();
		}
		return OtherInstanceRunning();
	};
	const auto requestRestart = [this] {
		if (_testHooks && _testHooks->requestRestart) {
			return _testHooks->requestRestart();
		}
		return RequestApplicationRestart();
	};
#else  // TDESKTOP_UNIT_TESTS
	const auto hasArchitecture
		= [](const QString &path) { return HasArchitecture(path); };
	const auto hasCleanCodeSeal
		= [](const QString &path) { return HasCleanCodeSeal(path); };
	const auto otherInstanceRunning = [] { return OtherInstanceRunning(); };
	const auto requestRestart = [] { return RequestApplicationRestart(); };
#endif // TDESKTOP_UNIT_TESTS
	@autoreleasepool {
		emit(TeagramUpdateInstallStage::Verifying, 0);
		const auto mainBundle = [NSBundle mainBundle];
		auto identifier = FromNative([mainBundle bundleIdentifier]);
		auto runningBundle = FromNative([mainBundle bundlePath]);
#ifdef TDESKTOP_UNIT_TESTS
		if (_testHooks) {
			identifier = _testHooks->runningBundleIdentifier;
			runningBundle = _testHooks->runningBundlePath;
		}
#endif // TDESKTOP_UNIT_TESTS
		const auto installed = CanonicalBundlePath(runningBundle);
		if (identifier != u"io.teagram.desktop"_q || installed.isEmpty()
			|| IsTranslocated(installed) || cancelled()) {
			return InstallResult(
				cancelled() ? TeagramUpdateInstallReason::Cancelled
							: TeagramUpdateInstallReason::InvalidTarget);
		}
		const auto parent = QFileInfo(installed).absolutePath();
		const auto staging
			= QDir(parent).filePath(QString::fromLatin1(kStagingDirectory));
		const auto previous
			= QDir(parent).filePath(QString::fromLatin1(kPreviousBundle));
		auto installedInfo = BundleInfo();
		if (!OwnedWritableTarget(installed)
			|| !ReadBundleInfo(installed, &installedInfo)) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (installedInfo.identifier != u"io.teagram.desktop"_q
			|| installedInfo.executable != u"Teagram"_q
			|| installedInfo.channel != InstalledChannel()
			|| !hasArchitecture(
				QDir(installed).filePath(u"Contents/MacOS/Teagram"_q))) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (installedInfo.build != candidate.installedBuild()) {
			return InstallResult(
				TeagramUpdateInstallReason::InstalledBuildChanged);
		}
		const auto build = candidate.metadata().build();
		const auto switchToMain
			= candidate.selection() == TeagramUpdateSelection::SwitchToMain
			  && candidate.metadata().channel() == TeagramUpdateChannel::Main;
		if (build == candidate.installedBuild()
			|| (build < candidate.installedBuild() && !switchToMain)) {
			return InstallResult(TeagramUpdateInstallReason::CandidateRejected);
		}
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write, installed,
				"core.teagram-update.target")
			|| !MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write, parent,
				"core.teagram-update.parent")
			|| !MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read, candidate.archivePath(),
				"core.teagram-update.archive")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (cancelled()) {
			return InstallResult(TeagramUpdateInstallReason::Cancelled);
		}
		const auto recovery
			= RecoverAtStartup(installed, staging, previous, installed, true,
							   otherInstanceRunning);
		if (recovery.reason == TeagramUpdateStartupRecoveryReason::Busy
			|| recovery.reason
				   == TeagramUpdateStartupRecoveryReason::PreviousRetained) {
			return InstallResult(TeagramUpdateInstallReason::Busy,
								 recovery.previousBundlePath);
		} else if (recovery.reason == TeagramUpdateStartupRecoveryReason::Failed
				   || recovery.reason
						  == TeagramUpdateStartupRecoveryReason::
							  RestoredPrevious) {
			return InstallResult(TeagramUpdateInstallReason::RecoveryRequired,
								 recovery.previousBundlePath);
		}
		auto lock = TeagramUpdateInstallLock(installed);
		if (!lock.acquired()) {
			return InstallResult(TeagramUpdateInstallReason::Busy);
		}
		if (cancelled()) {
			return InstallResult(TeagramUpdateInstallReason::Cancelled);
		}
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Mkdir, staging,
				"core.teagram-update.staging")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (!EnsurePrivateStage(parent, staging)) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		emit(TeagramUpdateInstallStage::Extracting, 0);
		const auto extraction = ExtractAuthenticatedTeagramUpdateArchive(
			candidate, staging, cancelled, [&](int value) {
				emit(TeagramUpdateInstallStage::Extracting, value);
			});
		if (!extraction.extracted()) {
			if (::rmdir(QFile::encodeName(staging).constData()) != 0
				&& errno != ENOENT) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(
				extraction.reason == TeagramUpdateArchiveReason::Cancelled
					? TeagramUpdateInstallReason::Cancelled
					: TeagramUpdateInstallReason::ArchiveRejected);
		}
		emit(TeagramUpdateInstallStage::Verifying, 0);
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read, extraction.bundlePath,
				"core.teagram-update.candidate")
			|| !ValidateBundle(extraction.bundlePath,
							   ChannelName(candidate.metadata().channel()),
							   candidate.metadata().build(), true,
							   hasArchitecture, hasCleanCodeSeal)) {
			if (RecoverTeagramUpdateReplacement(lock, installed, staging,
												previous, installed, false,
												false, false)
					.reason
				== TeagramUpdateStartupRecoveryReason::Failed) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(TeagramUpdateInstallReason::CandidateRejected);
		}
		emit(TeagramUpdateInstallStage::Verifying, 100);
		if (cancelled()) {
			const auto cleanup = RecoverTeagramUpdateReplacement(
				lock, installed, staging, previous, installed, false, false,
				false);
			return InstallResult(
				cleanup.reason == TeagramUpdateStartupRecoveryReason::Failed
					? TeagramUpdateInstallReason::RecoveryRequired
					: TeagramUpdateInstallReason::Cancelled);
		}
		if (!ValidateBundle(installed, InstalledChannel(),
							candidate.installedBuild(), false, hasArchitecture,
							hasCleanCodeSeal)) {
			return InstallResult(
				TeagramUpdateInstallReason::InstalledBuildChanged);
		}
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write, extraction.bundlePath,
				"core.teagram-update.quarantine")
			|| !MacProtectedPath::CheckPair(MacProtectedPath::Operation::Rename,
											installed, extraction.bundlePath,
											"core.teagram-update.swap")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		base::Platform::RemoveQuarantine(extraction.bundlePath);
		CarryCustomIcon(installed, extraction.bundlePath);
		if (!RemoveMarker(parent, kStartedMarker)) {
			return InstallResult(TeagramUpdateInstallReason::ReplacementFailed);
		}
		if (!WriteMarker(parent, kCommitMarker, candidate.metadata().build())) {
			return RemoveMarker(parent, kCommitMarker)
					   ? InstallResult(
							 TeagramUpdateInstallReason::ReplacementFailed)
					   : InstallResult(
							 TeagramUpdateInstallReason::RecoveryRequired);
		}
		emit(TeagramUpdateInstallStage::Replacing, 0);
		const auto replacement = CommitTeagramUpdateReplacement(
			lock, installed, extraction.bundlePath, previous, cancelled
#ifdef TDESKTOP_UNIT_TESTS
			,
			_testHooks ? _testHooks->replacementFailureInjector
					   : TeagramUpdateReplacementFailureInjector()
#endif // TDESKTOP_UNIT_TESTS
		);
		if (replacement.reason != TeagramUpdateReplacementReason::Replaced) {
			if (replacement.reason
				== TeagramUpdateReplacementReason::RecoveryRequired) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired,
					replacement.previousBundlePath, replacement.committed);
			}
			const auto cleanup = RecoverTeagramUpdateReplacement(
				lock, installed, staging, previous, installed, false, false,
				false);
			if (cleanup.reason == TeagramUpdateStartupRecoveryReason::Failed
				|| cleanup.reason == TeagramUpdateStartupRecoveryReason::Busy
				|| cleanup.reason
					   == TeagramUpdateStartupRecoveryReason::RestoredPrevious
				|| cleanup.reason
					   == TeagramUpdateStartupRecoveryReason::PreviousRemoved) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired);
			}
			if (!RemoveMarker(parent, kCommitMarker)) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(
				replacement.reason == TeagramUpdateReplacementReason::Cancelled
					? TeagramUpdateInstallReason::Cancelled
				: replacement.reason == TeagramUpdateReplacementReason::Busy
					? TeagramUpdateInstallReason::Busy
					: TeagramUpdateInstallReason::ReplacementFailed);
		}
		emit(TeagramUpdateInstallStage::Replacing, 100);
		if (::rmdir(QFile::encodeName(staging).constData()) != 0
			&& errno != ENOENT) {
			return InstallResult(TeagramUpdateInstallReason::RecoveryRequired,
								 replacement.previousBundlePath, true);
		}
		emit(TeagramUpdateInstallStage::Relaunching, 0);
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read, installed,
				"core.teagram-update.relaunch")) {
			return InstallResult(
				TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
				replacement.previousBundlePath, true);
		}
		if (beforeRelaunch) {
			if (!beforeRelaunch(requestRestart)) {
				return InstallResult(
					TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
					replacement.previousBundlePath, true);
			}
		} else if (!requestRestart()) {
			return InstallResult(
				TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
				replacement.previousBundlePath, true);
		}
		emit(TeagramUpdateInstallStage::Relaunching, 100);
		return InstallResult(TeagramUpdateInstallReason::RelaunchRequested,
							 replacement.previousBundlePath, true);
	}
}

void CompleteTeagramUpdateStartup() {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return;
		}
		(void)CompleteTeagramUpdateStartupForBundle(
			FromNative([mainBundle bundlePath]));
	}
}

void CompleteScheduledTeagramUpdateAfterShutdown() {
	@autoreleasepool {
		const auto failureText = PendingShutdownInstall
									 ? PendingShutdownInstall->failureText
									 : QString();
		if (const auto result = InstallScheduledOlderTeagramUpdate();
			result && !result->replacementCommitted) {
			LOG(("Teagram shutdown update failed: reason=%1")
					.arg(int(result->reason)));
			const auto alert = [[NSAlert alloc] init];
			alert.messageText = Platform::Q2NSString(failureText);
			[alert runModal];
			return;
		}
	}
}

TeagramUpdateRelaunchResult RelaunchTeagramUpdateAfterShutdown(
	const QString &dataFile, const std::function<void()> &prepareForLaunch) {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return TeagramUpdateRelaunchResult::NotPending;
		}
		const auto current = FromNative([mainBundle bundlePath]);
		return RelaunchPendingUpdate(
			current, dataFile,
			[](const QString &path, const QStringList &arguments,
			   pid_t *processId) {
				return LaunchBundle(path, arguments, processId);
			},
			{}, prepareForLaunch);
	}
}

} // namespace Core
