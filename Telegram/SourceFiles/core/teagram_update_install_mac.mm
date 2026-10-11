/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_install.h"

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
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>

namespace Core {
namespace {

constexpr auto kStagingDirectory = ".teagram-update-staging";
constexpr auto kPreviousBundle = ".Teagram.previous.app";
constexpr auto kCommitMarker = ".teagram-update-pending";
constexpr auto kStartedMarker = ".teagram-update-started";
constexpr auto kStartupWaitMs = 30'000;

struct BundleInfo {
	QString identifier;
	QString executable;
	QString channel;
	quint64 build = 0;
};

[[nodiscard]] bool IsCanonicalDirectory(const QString &path,
										bool privateDirectory);

[[nodiscard]] QString FromNative(NSString *value) {
	return value ? QFile::decodeName([value fileSystemRepresentation]) : QString();
}

[[nodiscard]] QString FromProperty(NSDictionary *properties,
								   NSString *key) {
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
	auto properties = [NSDictionary dictionaryWithContentsOfFile:
		Platform::Q2NSString(plistPath)];
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
	*result = { identifier, executable, channel, build };
	return true;
}

[[nodiscard]] bool IsCanonicalDirectory(const QString &path,
										bool privateDirectory) {
	const auto info = QFileInfo(path);
	const auto canonical = info.canonicalFilePath();
	if (!info.isDir() || info.isSymLink() || canonical.isEmpty()
		|| QDir::cleanPath(canonical)
			!= QDir::cleanPath(info.absoluteFilePath())) {
		return false;
	}
	struct stat stat = {};
	const auto bytes = QFile::encodeName(canonical);
	return ::lstat(bytes.constData(), &stat) == 0
		&& S_ISDIR(stat.st_mode)
		&& stat.st_uid == ::geteuid()
		&& (!privateDirectory || (stat.st_mode & 0077) == 0);
}

[[nodiscard]] QString CanonicalDirectory(const QString &path) {
	if (!IsCanonicalDirectory(path, false)) {
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
			&& info.fileName() != u".Teagram.previous.app"_q)) {
		return {};
	}
	return QDir(parent).filePath(info.fileName());
}

[[nodiscard]] bool IsTranslocated(const QString &path) {
	return path.split('/').contains(u"AppTranslocation"_q);
}

[[nodiscard]] bool RunTool(const QString &program,
						   const QStringList &arguments,
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
		*output = process.readAllStandardOutput()
			+ process.readAllStandardError();
	}
	return process.exitStatus() == QProcess::NormalExit
		&& process.exitCode() == 0;
}

[[nodiscard]] bool HasArchitecture(const QString &executable) {
	auto output = QByteArray();
	return RunTool(
		u"/usr/bin/lipo"_q,
		{ u"-archs"_q, executable },
		&output)
		&& QString::fromLatin1(output).trimmed() == u"arm64"_q;
}

[[nodiscard]] bool HasCleanCodeSeal(const QString &bundlePath) {
	if (!RunTool(
			u"/usr/bin/codesign"_q,
			{ u"--verify"_q, u"--deep"_q, u"--strict"_q, bundlePath })) {
		return false;
	}
	auto display = QByteArray();
	if (!RunTool(
			u"/usr/bin/codesign"_q,
			{ u"--display"_q, u"--verbose=4"_q, bundlePath },
			&display)) {
		return false;
	}
	return display.contains("Identifier=io.teagram.desktop\n");
}

[[nodiscard]] bool IsRegularExecutable(const QString &path) {
	const auto info = QFileInfo(path);
	return info.isFile() && !info.isSymLink() && info.isExecutable();
}

[[nodiscard]] bool ValidateBundle(const QString &bundlePath,
								  const QString &expectedChannel,
								  quint64 expectedBuild,
								  bool verifySeal,
								  const std::function<bool(const QString &)> &hasArchitecture = {},
								  const std::function<bool(const QString &)> &hasCleanCodeSeal = {}) {
	if (CanonicalBundlePath(bundlePath) != QDir::cleanPath(bundlePath)) {
		return false;
	}
	const auto executable
		= QDir(bundlePath).filePath(u"Contents/MacOS/Teagram"_q);
	const auto contents = QDir(bundlePath).filePath(u"Contents"_q);
	const auto macos = QDir(contents).filePath(u"MacOS"_q);
	const auto resources
		= QDir(bundlePath).filePath(u"Contents/Info.plist"_q);
	if (!IsCanonicalDirectory(contents, false)
		|| !IsCanonicalDirectory(macos, false)
		|| !QFileInfo(resources).isFile() || QFileInfo(resources).isSymLink()
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
		&& S_ISDIR(bundleInfo.st_mode)
		&& bundleInfo.st_uid == ::geteuid()
		&& ::lstat(parentBytes.constData(), &parentInfo) == 0
		&& S_ISDIR(parentInfo.st_mode)
		&& parentInfo.st_uid == ::geteuid()
		&& bundleInfo.st_dev == parentInfo.st_dev
		&& ::access(bundleBytes.constData(), W_OK) == 0
		&& ::access(parentBytes.constData(), W_OK | X_OK) == 0;
}

[[nodiscard]] bool EnsurePrivateStage(const QString &parent,
									 const QString &staging) {
	const auto parentBytes = QFile::encodeName(parent);
	const auto name = QFile::encodeName(QFileInfo(staging).fileName());
	if (parentBytes.isEmpty() || name.isEmpty()) {
		return false;
	}
	auto parentDescriptor = ::open(
		parentBytes.constData(),
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
	const auto valid = ::fstatat(
			parentDescriptor, name.constData(), &stageInfo, AT_SYMLINK_NOFOLLOW)
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
	return QString::fromLatin1(
		kTeagramUpdateChannel.data(),
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

[[nodiscard]] bool WriteMarker(const QString &parent,
							   const char *name,
							   quint64 build) {
	return WriteTeagramUpdateMarker(parent, name, build);
}

[[nodiscard]] std::optional<quint64> ReadMarker(const QString &parent,
										 const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(
		parentBytes.constData(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory < 0) {
		return std::nullopt;
	}
	const auto descriptor = ::openat(
		directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	::close(directory);
	if (descriptor < 0) {
		return std::nullopt;
	}
	struct stat info = {};
	auto success = ::fstat(descriptor, &info) == 0
		&& S_ISREG(info.st_mode)
		&& info.st_uid == ::geteuid()
		&& info.st_nlink == 1
		&& (info.st_mode & 0077) == 0
		&& info.st_size > 0
		&& info.st_size <= 32;
	auto bytes = QByteArray();
	if (success) {
		bytes.resize(qsizetype(info.st_size));
		auto offset = qsizetype(0);
		while (offset < bytes.size()) {
			const auto read = ::read(
				descriptor,
				bytes.data() + offset,
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

[[nodiscard]] std::optional<bool> MarkerExists(
		const QString &parent,
		const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(
		parentBytes.constData(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory < 0) {
		return std::nullopt;
	}
	struct stat info = {};
	const auto result = ::fstatat(
		directory, name, &info, AT_SYMLINK_NOFOLLOW);
	const auto error = errno;
	::close(directory);
	if (result == 0) {
		return true;
	}
	return error == ENOENT ? std::optional<bool>(false) : std::nullopt;
}

[[nodiscard]] bool RemoveMarker(const QString &parent, const char *name) {
	const auto parentBytes = QFile::encodeName(parent);
	auto directory = ::open(
		parentBytes.constData(),
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
	const auto removed = S_ISREG(info.st_mode)
		&& info.st_uid == ::geteuid()
		&& info.st_nlink == 1
		&& (info.st_mode & 0077) == 0
		&& ::unlinkat(directory, name, 0) == 0
		&& ::fsync(directory) == 0;
	::close(directory);
	return removed;
}

[[nodiscard]] TeagramUpdateInstallResult InstallResult(
		TeagramUpdateInstallReason reason,
		const QString &previous = {}) {
	return { reason, previous, false };
}

[[nodiscard]] TeagramUpdateStartupRecoveryResult RecoverAtStartup(
		const QString &installed,
		const QString &staging,
		const QString &previous,
		const QString &running,
		const std::function<bool()> &otherInstanceRunning = {}) {
	auto lock = TeagramUpdateInstallLock(installed);
	if (!lock.acquired()) {
		return {
			TeagramUpdateStartupRecoveryReason::Busy,
			{},
		};
	}
	const auto previousAllowed
		= MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::RecursiveDelete,
			previous,
			"core.teagram-update.recover-previous");
	const auto targetAllowed
		= MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::Rename,
			installed,
			"core.teagram-update.recover-target");
	const auto stageAllowed
		= MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::RecursiveDelete,
			staging,
			"core.teagram-update.recover-staging");
	if (!previousAllowed || !targetAllowed || !stageAllowed
		|| !MacProtectedPath::CheckPair(
			MacProtectedPath::Operation::Rename,
			staging,
			previous,
			"core.teagram-update.recover-pair")) {
		return {
			TeagramUpdateStartupRecoveryReason::Failed,
			{},
		};
	}
	const auto stageBundle = QDir(staging).filePath(u"Teagram.app"_q);
	auto stagePrevious = false;
	const auto commitMarkerExists = MarkerExists(parent, kCommitMarker);
	if (!commitMarkerExists) {
		return {
			TeagramUpdateStartupRecoveryReason::Failed,
			{},
		};
	}
	const auto pending = *commitMarkerExists
		? ReadMarker(parent, kCommitMarker)
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
			&& stagedInfo.build < installedInfo.build;
		if (stagedInfo.build != *pending && !stagePrevious) {
			return {
				TeagramUpdateStartupRecoveryReason::Failed,
				{},
			};
		}
	}
	const auto noOtherInstance = !(otherInstanceRunning
		? otherInstanceRunning()
		: OtherInstanceRunning());
	return RecoverTeagramUpdateReplacement(
		lock,
		installed,
		staging,
		previous,
		running,
		stagePrevious,
		true,
		noOtherInstance);
}

[[nodiscard]] bool RequestApplicationRestart() {
	auto *application = QCoreApplication::instance();
	return application && QMetaObject::invokeMethod(
		application,
		[application] {
			cSetRestarting(true);
			application->quit();
		},
		Qt::QueuedConnection);
}

[[nodiscard]] bool WaitForStartup(quint64 build, const QString &parent) {
	auto timer = QElapsedTimer();
	timer.start();
	while (timer.elapsed() < kStartupWaitMs) {
		if (const auto started = ReadMarker(parent, kStartedMarker);
			started && *started == build) {
			return true;
		}
		QThread::msleep(100);
	}
	return false;
}

[[nodiscard]] bool LaunchBundle(const QString &bundlePath, pid_t *processId) {
	auto semaphore = dispatch_semaphore_create(0);
	__block NSRunningApplication *launched = nil;
	__block NSError *launchError = nil;
	auto configuration = [NSWorkspaceOpenConfiguration configuration];
	configuration.createsNewApplicationInstance = YES;
	const auto url = [NSURL fileURLWithPath:Platform::Q2NSString(bundlePath)
					isDirectory:YES];
	[[NSWorkspace sharedWorkspace]
		openApplicationAtURL:url
		configuration:configuration
		completionHandler:^(NSRunningApplication *application, NSError *error) {
			launched = application;
			launchError = error;
			dispatch_semaphore_signal(semaphore);
		}];
	const auto deadline = dispatch_time(
		DISPATCH_TIME_NOW,
		30 * NSEC_PER_SEC);
	if (dispatch_semaphore_wait(semaphore, deadline) != 0
		|| !launched || launchError) {
		return false;
	}
	if (processId) {
		*processId = [launched processIdentifier];
	}
	return true;
}

void StopLaunchedBundle(pid_t processId) {
	if (processId <= 0) {
		return;
	}
	auto application
		= [NSRunningApplication runningApplicationWithProcessIdentifier:processId];
	if (!application || [application isTerminated]) {
		return;
	}
	[application terminate];
	auto timer = QElapsedTimer();
	timer.start();
	while (timer.elapsed() < 5'000) {
		application = [NSRunningApplication
			runningApplicationWithProcessIdentifier:processId];
		if (!application || [application isTerminated]) {
			return;
		}
		QThread::msleep(100);
	}
	[application forceTerminate];
}

[[nodiscard]] TeagramUpdateRelaunchResult RelaunchPendingUpdate(
		const QString &installed,
		const QString &previous,
		const std::function<bool(const QString &, pid_t *)> &launchBundle,
		const std::function<bool()> &otherInstanceRunning = {}) {
	const auto parent = QFileInfo(installed).absolutePath();
	if (CanonicalBundlePath(installed) != QDir::cleanPath(installed)
		|| !MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::Read,
			installed,
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
	auto processId = pid_t(0);
	if (launchBundle(installed, &processId)
		&& WaitForStartup(*pending, parent)) {
		return RemoveMarker(parent, kCommitMarker)
			&& RemoveMarker(parent, kStartedMarker)
			? TeagramUpdateRelaunchResult::CandidateStarted
			: TeagramUpdateRelaunchResult::Failed;
	}
	StopLaunchedBundle(processId);
	if (otherInstanceRunning ? otherInstanceRunning() : OtherInstanceRunning()) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	const auto canonicalPrevious = CanonicalRunningBundlePath(previous);
	auto previousInfo = BundleInfo();
	if (canonicalPrevious != QDir::cleanPath(previous)
		|| !MacProtectedPath::CheckExternalPathForUse(
			MacProtectedPath::Operation::Read,
			previous,
			"core.teagram-update.fallback")
		|| !ReadBundleInfo(canonicalPrevious, &previousInfo)
		|| previousInfo.identifier != u"io.teagram.desktop"_q
		|| previousInfo.executable != u"Teagram"_q
		|| previousInfo.build >= *pending) {
		return TeagramUpdateRelaunchResult::Failed;
	}
	processId = 0;
	return launchBundle(canonicalPrevious, &processId)
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
	const auto staging = QDir(parent).filePath(
		QString::fromLatin1(kStagingDirectory));
	const auto previous = QDir(parent).filePath(
		QString::fromLatin1(kPreviousBundle));
	const auto pending = ReadMarker(parent, kCommitMarker);
	auto info = BundleInfo();
	if (!ReadBundleInfo(running, &info)
		|| info.identifier != u"io.teagram.desktop"_q
		|| info.executable != u"Teagram"_q) {
		return false;
	}
	const auto runningIsInstalled = running == QDir::cleanPath(installed);
	auto startupAcknowledged = false;
	if (runningIsInstalled && pending && *pending == info.build) {
		if (!WriteMarker(parent, kStartedMarker, info.build)) {
			return false;
		}
		startupAcknowledged = true;
	}
	const auto recovered = RecoverAtStartup(
		installed, staging, previous, running, otherInstanceRunning);
	if (recovered.reason == TeagramUpdateStartupRecoveryReason::Failed
		|| recovered.reason == TeagramUpdateStartupRecoveryReason::Busy) {
		LOG(("Teagram update recovery failed: reason=%1")
			.arg(static_cast<int>(recovered.reason)));
		if (startupAcknowledged && !RemoveMarker(parent, kStartedMarker)) {
			LOG(("Teagram update startup marker could not be removed."));
		}
		return false;
	}
	return startupAcknowledged;
}

} // namespace

#ifdef TDESKTOP_UNIT_TESTS
TeagramUpdateStartupRecoveryResult
RecoverTeagramUpdateInstallerAtStartupForTesting(
		const QString &installed,
		const QString &staging,
		const QString &previous,
		const QString &running,
		const std::function<bool()> &otherInstanceRunning) {
	return RecoverAtStartup(
		installed, staging, previous, running, otherInstanceRunning);
}

bool CompleteTeagramUpdateStartupForTesting(
		const QString &runningBundlePath,
		const std::function<bool()> &otherInstanceRunning) {
	@autoreleasepool {
		return CompleteTeagramUpdateStartupForBundle(
			runningBundlePath, otherInstanceRunning);
	}
}

TeagramUpdateRelaunchResult RelaunchTeagramUpdateAfterShutdownForTesting(
		const QString &installedBundlePath,
		const QString &previousBundlePath,
		const std::function<bool(const QString &)> &launchBundle,
		const std::function<bool()> &otherInstanceRunning) {
	@autoreleasepool {
		return RelaunchPendingUpdate(
			installedBundlePath,
			previousBundlePath,
			[&](const QString &path, pid_t *processId) {
				if (processId) {
					*processId = 0;
				}
				return launchBundle && launchBundle(path);
			},
			otherInstanceRunning);
	}
}
#endif // TDESKTOP_UNIT_TESTS

void TeagramUpdateInstaller::cancel() {
	_cancelled.store(true, std::memory_order_relaxed);
}

TeagramUpdateInstallResult TeagramUpdateInstaller::install(
	const TeagramUpdateCandidate &candidate,
	TeagramUpdateInstallProgressHandler progress) {
	if (_started.exchange(true, std::memory_order_relaxed)) {
		return InstallResult(TeagramUpdateInstallReason::Busy);
	}
	const auto cancelled = [&] {
		return _cancelled.load(std::memory_order_relaxed);
	};
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
#else // TDESKTOP_UNIT_TESTS
	const auto hasArchitecture = [](const QString &path) {
		return HasArchitecture(path);
	};
	const auto hasCleanCodeSeal = [](const QString &path) {
		return HasCleanCodeSeal(path);
	};
	const auto otherInstanceRunning = [] {
		return OtherInstanceRunning();
	};
	const auto requestRestart = [] {
		return RequestApplicationRestart();
	};
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
			return InstallResult(cancelled()
				? TeagramUpdateInstallReason::Cancelled
				: TeagramUpdateInstallReason::InvalidTarget);
		}
		const auto parent = QFileInfo(installed).absolutePath();
		const auto staging = QDir(parent).filePath(
			QString::fromLatin1(kStagingDirectory));
		const auto previous = QDir(parent).filePath(
			QString::fromLatin1(kPreviousBundle));
		auto installedInfo = BundleInfo();
		if (!OwnedWritableTarget(installed)
			|| !ReadBundleInfo(installed, &installedInfo)) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (installedInfo.identifier != u"io.teagram.desktop"_q
			|| installedInfo.executable != u"Teagram"_q
			|| installedInfo.channel != InstalledChannel()
			|| !hasArchitecture(QDir(installed).filePath(
				u"Contents/MacOS/Teagram"_q))) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (installedInfo.build != candidate.installedBuild()) {
			return InstallResult(TeagramUpdateInstallReason::InstalledBuildChanged);
		}
		if (candidate.metadata().build() <= candidate.installedBuild()) {
			return InstallResult(TeagramUpdateInstallReason::CandidateRejected);
		}
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write,
				installed,
				"core.teagram-update.target")
			|| !MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write,
				parent,
				"core.teagram-update.parent")
			|| !MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read,
				candidate.archivePath(),
				"core.teagram-update.archive")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (cancelled()) {
			return InstallResult(TeagramUpdateInstallReason::Cancelled);
		}
		const auto recovery = RecoverAtStartup(
			installed, staging, previous, installed, otherInstanceRunning);
		if (recovery.reason == TeagramUpdateStartupRecoveryReason::Busy
			|| recovery.reason == TeagramUpdateStartupRecoveryReason::PreviousRetained) {
			return InstallResult(TeagramUpdateInstallReason::Busy,
				recovery.previousBundlePath);
		} else if (recovery.reason == TeagramUpdateStartupRecoveryReason::Failed
			|| recovery.reason == TeagramUpdateStartupRecoveryReason::RestoredPrevious) {
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
				MacProtectedPath::Operation::Mkdir,
				staging,
				"core.teagram-update.staging")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		if (!EnsurePrivateStage(parent, staging)) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		emit(TeagramUpdateInstallStage::Extracting, 0);
		const auto extraction = ExtractAuthenticatedTeagramUpdateArchive(
			candidate,
			staging,
			cancelled,
			[&](int value) { emit(TeagramUpdateInstallStage::Extracting, value); });
		if (!extraction.extracted()) {
			if (::rmdir(QFile::encodeName(staging).constData()) != 0
				&& errno != ENOENT) {
				return InstallResult(TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(extraction.reason
					== TeagramUpdateArchiveReason::Cancelled
				? TeagramUpdateInstallReason::Cancelled
				: TeagramUpdateInstallReason::ArchiveRejected);
		}
		emit(TeagramUpdateInstallStage::Verifying, 0);
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read,
				extraction.bundlePath,
				"core.teagram-update.candidate")
			|| !ValidateBundle(
				extraction.bundlePath,
				ChannelName(candidate.metadata().channel()),
				candidate.metadata().build(),
				true,
				hasArchitecture,
				hasCleanCodeSeal)) {
			if (RecoverTeagramUpdateReplacement(
					lock,
					installed,
					staging,
					previous,
					installed,
					false,
					false,
					false)
				.reason == TeagramUpdateStartupRecoveryReason::Failed) {
				return InstallResult(TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(TeagramUpdateInstallReason::CandidateRejected);
		}
		emit(TeagramUpdateInstallStage::Verifying, 100);
		if (cancelled()) {
			const auto cleanup = RecoverTeagramUpdateReplacement(
				lock, installed, staging, previous, installed, false, false, false);
			return InstallResult(cleanup.reason
				== TeagramUpdateStartupRecoveryReason::Failed
				? TeagramUpdateInstallReason::RecoveryRequired
				: TeagramUpdateInstallReason::Cancelled);
		}
		if (!ValidateBundle(
				installed,
				InstalledChannel(),
				candidate.installedBuild(),
				false,
				hasArchitecture,
				hasCleanCodeSeal)) {
			return InstallResult(TeagramUpdateInstallReason::InstalledBuildChanged);
		}
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Write,
				extraction.bundlePath,
				"core.teagram-update.quarantine")
			|| !MacProtectedPath::CheckPair(
				MacProtectedPath::Operation::Rename,
				installed,
				extraction.bundlePath,
				"core.teagram-update.swap")) {
			return InstallResult(TeagramUpdateInstallReason::InvalidTarget);
		}
		base::Platform::RemoveQuarantine(extraction.bundlePath);
		if (!RemoveMarker(parent, kStartedMarker)) {
			return InstallResult(TeagramUpdateInstallReason::ReplacementFailed);
		}
		if (!WriteMarker(parent, kCommitMarker, candidate.metadata().build())) {
			return RemoveMarker(parent, kCommitMarker)
				? InstallResult(TeagramUpdateInstallReason::ReplacementFailed)
				: InstallResult(TeagramUpdateInstallReason::RecoveryRequired);
		}
		emit(TeagramUpdateInstallStage::Replacing, 0);
		const auto replacement = CommitTeagramUpdateReplacement(
			lock,
			installed,
			extraction.bundlePath,
			previous,
			cancelled);
		if (replacement.reason
			!= TeagramUpdateReplacementReason::Replaced) {
			if (replacement.reason
					== TeagramUpdateReplacementReason::RecoveryRequired) {
				return InstallResult(
					TeagramUpdateInstallReason::RecoveryRequired,
					replacement.previousBundlePath);
			}
			if (!RemoveMarker(parent, kCommitMarker)) {
				return InstallResult(TeagramUpdateInstallReason::RecoveryRequired);
			}
			if (::rmdir(QFile::encodeName(staging).constData()) != 0
				&& errno != ENOENT) {
				return InstallResult(TeagramUpdateInstallReason::RecoveryRequired);
			}
			return InstallResult(replacement.reason
					== TeagramUpdateReplacementReason::Cancelled
				? TeagramUpdateInstallReason::Cancelled
				: replacement.reason == TeagramUpdateReplacementReason::Busy
					? TeagramUpdateInstallReason::Busy
					: TeagramUpdateInstallReason::ReplacementFailed);
		}
		emit(TeagramUpdateInstallStage::Replacing, 100);
		if (::rmdir(QFile::encodeName(staging).constData()) != 0
			&& errno != ENOENT) {
			return InstallResult(
				TeagramUpdateInstallReason::RecoveryRequired,
				replacement.previousBundlePath);
		}
		emit(TeagramUpdateInstallStage::Relaunching, 0);
		if (!MacProtectedPath::CheckExternalPathForUse(
				MacProtectedPath::Operation::Read,
				installed,
				"core.teagram-update.relaunch")) {
			return InstallResult(
				TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
				replacement.previousBundlePath);
		}
		if (!requestRestart()) {
			return InstallResult(
				TeagramUpdateInstallReason::RelaunchFailedPreviousAvailable,
				replacement.previousBundlePath);
		}
		emit(TeagramUpdateInstallStage::Relaunching, 100);
		return InstallResult(
			TeagramUpdateInstallReason::RelaunchRequested,
			replacement.previousBundlePath);
	}
}

void CompleteTeagramUpdateStartup() {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return;
		}
		CompleteTeagramUpdateStartupForBundle(
			FromNative([mainBundle bundlePath]));
	}
}

TeagramUpdateRelaunchResult RelaunchTeagramUpdateAfterShutdown() {
	@autoreleasepool {
		const auto mainBundle = [NSBundle mainBundle];
		if (FromNative([mainBundle bundleIdentifier])
			!= u"io.teagram.desktop"_q) {
			return TeagramUpdateRelaunchResult::NotPending;
		}
		const auto installed = FromNative([mainBundle bundlePath]);
		const auto parent = QFileInfo(installed).absolutePath();
		const auto previous = QDir(parent).filePath(
			QString::fromLatin1(kPreviousBundle));
		return RelaunchPendingUpdate(
			installed,
			previous,
			[](const QString &path, pid_t *processId) {
				return LaunchBundle(path, processId);
			});
	}
}

} // namespace Core
