/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/sandbox.h"

#include "base/platform/base_platform_info.h"
#include "platform/platform_specific.h"
#if !defined Q_OS_WIN && !defined Q_OS_MAC
#include "platform/linux/local_socket_path.h"
#endif // !Q_OS_WIN && !Q_OS_MAC
#include "mainwidget.h"
#include "mainwindow.h"
#include "storage/localstorage.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "core/crash_reports.h"
#include "core/crash_report_window.h"
#include "core/application.h"
#include "core/external_control.h"
#include "core/launcher.h"
#include "core/single_instance_lock.h"
#include "core/local_url_handlers.h"
#include "core/update_checker.h"
#include "core/deadlock_detector.h"
#include "core/ui_stall_watchdog.h"
#include "logs.h"
#ifdef Q_OS_MAC
#include "window/main_window.h"
#include "core/core_settings.h"
#include "core/mac_protected_path_runtime.h"
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
#include "tests/font_shutdown_probe.h"
#endif
#include "ui/style/style_core_font.h"
#endif // Q_OS_MAC
#include "base/timer.h"
#include "base/concurrent_timer.h"
#include "base/invoke_queued.h"
#include "base/options.h"
#include "base/qthelp_url.h"
#include "base/qthelp_regex.h"
#include "ui/ui_utility.h"
#include "ui/emoji_config.h"
#include "ui/effects/animations.h"

#ifdef Q_OS_MAC
#include "platform/mac/global_menu_mac.h"
#endif // Q_OS_MAC

#include <QtCore/QLockFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtGui/QSessionManager>
#include <QtGui/QScreen>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <sys/stat.h>
#endif // Q_OS_UNIX
#ifdef Q_OS_LINUX
#include <unistd.h>
#include <ksandbox.h>
#endif // Q_OS_LINUX
#if !defined Q_OS_WIN && !defined Q_OS_MAC
#include <QtWidgets/QMessageBox>
#endif // !Q_OS_WIN && !Q_OS_MAC
#ifdef Q_OS_MAC
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtGui/QBackingStore>
#include <QtGui/QExposeEvent>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPalette>
#include <QtGui/QPainter>
#include <QtGui/QRegion>
#include <QtGui/QResizeEvent>
#include <QtGui/QWindow>
#endif // Q_OS_MAC
#include <QtGui/qpa/qplatformscreen.h>

#ifdef Q_OS_MAC
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <functional>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <sys/un.h>
#endif // Q_OS_MAC

namespace Core {
namespace {

base::options::toggle OptionDeadlockDetector({
	.id = kOptionDeadlockDetector,
	.name = "Deadlock Detector",
	.description = "Check once every 30 seconds that main thread is still responsive.",
});

constexpr auto kCleanupIpcTimeout = 10 * crl::time(1000);
constexpr auto kCleanupQuitTimeout = 30 * crl::time(1000);
constexpr auto kUnexpectedSingleInstanceErrorLimit = 3;
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
constexpr auto kSocketCollisionFixtureContents
	= "Teagram IPC collision fixture";
#endif

#if !defined Q_OS_WINRT && (defined Q_OS_WIN || defined Q_OS_MAC)
[[nodiscard]] bool
RemoveStaleLocalServer([[maybe_unused]] const QString &name) {
#if defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
	// Linux uses an abstract-namespace socket, with no filesystem entry.
	return true;
#elif defined(Q_OS_UNIX)
	const auto encoded = QFile::encodeName(name);
	struct stat info = {};
	if (::lstat(encoded.constData(), &info) != 0) {
		return (errno == ENOENT);
	}
	return S_ISSOCK(info.st_mode) && QFile::remove(name);
#else  // Q_OS_UNIX
	return true;
#endif // Q_OS_UNIX
}
#endif // !Q_OS_WINRT && (Q_OS_WIN || Q_OS_MAC)

#if !defined Q_OS_WIN && !defined Q_OS_MAC
void ShowSingleInstanceStartupError(const QString &error) {
	const auto message
		= error.isEmpty()
			  ? u"Could not start the single-instance IPC service."_q
			  : error;
	QMessageBox::critical(nullptr, QApplication::applicationName(), message);
}
#endif // !Q_OS_WIN && !Q_OS_MAC
#ifdef Q_OS_MAC
constexpr auto kFontStartupSplashExposureTimeout = 1000;
#endif // Q_OS_MAC

#ifdef Q_OS_MAC
constexpr auto kProbeMaxFrames = 64;
void *ProbeFrames[kProbeMaxFrames];
std::atomic_int ProbeDepth = -1;

// The main thread decides from the captured frames whether the worker stays
// parked: Wait until decided, then Release returns at once while Hold keeps
// the worker inside this handler, and so inside whatever it was executing,
// until the quit has been observed.
enum ProbeDecisionValue {
	ProbeWait = 0,
	ProbeRelease = 1,
	ProbeHold = 2,
};
std::atomic_int ProbeDecision = ProbeWait;
std::atomic_bool ProbeFinalRelease = false;
std::atomic_bool ProbeInHandler = false;

void ProbeSignal(int) {
	ProbeInHandler.store(true, std::memory_order_release);
	const auto depth = backtrace(ProbeFrames, kProbeMaxFrames);
	ProbeDepth.store(depth, std::memory_order_release);
	while (ProbeDecision.load(std::memory_order_acquire) == ProbeWait) {
		sched_yield();
	}
	if (ProbeDecision.load(std::memory_order_acquire) == ProbeHold) {
		while (!ProbeFinalRelease.load(std::memory_order_acquire)) {
			sched_yield();
		}
	}
	ProbeInHandler.store(false, std::memory_order_release);
}

[[nodiscard]] bool InstallProbeSignal(bool forceFailure) {
	if (forceFailure) {
		return false;
	}
	// The first backtrace() call may allocate, so make it outside a handler.
	void *primed[1];
	static_cast<void>(backtrace(primed, 1));
	struct sigaction action = {};
	action.sa_handler = ProbeSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	return sigaction(SIGUSR1, &action, nullptr) == 0;
}

[[nodiscard]] bool FramesInSystemEnumeration(void *const *frames, int depth) {
	auto coreTextEnumeration = false;
	auto qtFamilyList = false;
	auto qtCoreTextPopulation = false;
	auto qtDatabasePopulation = false;
	for (auto i = 0; i != depth; ++i) {
		auto info = Dl_info();
		if (!dladdr(frames[i], &info) || !info.dli_sname) {
			continue;
		}
		const auto name = QByteArray(info.dli_sname);
		if (name.contains("CTFontManagerCopyAvailableFontFamilyNames")) {
			coreTextEnumeration = true;
		}
		if (name.contains("QFontDatabase") && name.contains("families")) {
			qtFamilyList = true;
		}
		if (name.contains("QCoreTextFontDatabase")
			&& (name.contains("populateFromDescriptor")
				|| name.contains("populateFontDatabase"))) {
			qtCoreTextPopulation = true;
		}
		if (name.contains("QFontDatabasePrivate")
			&& name.contains("ensureFontDatabase")) {
			qtDatabasePopulation = true;
		}
	}
	const auto qtFontDatabasePopulation
		= qtFamilyList || qtCoreTextPopulation || qtDatabasePopulation;
	// Only real CoreText family enumeration during Qt population counts.
	// The test interposer repeats the query while Qt holds its lock.
	return coreTextEnumeration && qtFontDatabasePopulation;
}

enum class StackSample {
	Failed,
	OtherWork,
	InSystemEnumeration,
};

enum class ProbeExit {
	None,
	Quit,
	Failure,
};

constexpr auto kProbeReportedFrames = 8;
struct ProbeFrameSnapshot final {
	void *frames[kProbeReportedFrames] = {};
	int count = 0;
};

void PrintProbeFrameSnapshot(const ProbeFrameSnapshot &snapshot) {
	auto line = QByteArray("Font shutdown regression: last_other_work_frames=");
	if (!snapshot.count) {
		line += "unavailable";
	}
	for (auto i = 0; i != snapshot.count; ++i) {
		const auto address
			= reinterpret_cast<std::uintptr_t>(snapshot.frames[i]);
		const auto lookup = address ? reinterpret_cast<void *>(address - 1)
									: snapshot.frames[i];
		auto info = Dl_info();
		if (i) {
			line += " <- ";
		}
		if (dladdr(lookup, &info) && info.dli_sname) {
			auto status = 0;
			const auto demangled = abi::__cxa_demangle(info.dli_sname, nullptr,
													   nullptr, &status);
			line += (status == 0 && demangled) ? demangled : info.dli_sname;
			std::free(demangled);
		} else {
			line += "0x" + QByteArray::number(qulonglong(address), 16);
		}
	}
	fprintf(stderr, "%s\n", line.constData());
}

// Signals the worker and waits until its capture completes, however long the
// signal takes, then scans a bounded copy of the captured frames. A worker
// caught inside the system enumeration is held there until the final release.
[[nodiscard]] StackSample SampleWorkerStack(pthread_t worker,
											ProbeFrameSnapshot &lastOtherWork) {
	using Clock = std::chrono::steady_clock;
	const auto deadline = Clock::now() + std::chrono::seconds(2);
	// The previous handler must have seen its Release and returned before
	// the decision is reset, or it would park again for this sample.
	while (ProbeInHandler.load(std::memory_order_acquire)) {
		if (Clock::now() >= deadline) {
			fprintf(stderr, "Font shutdown regression: "
							"probe_error=handler_release_timeout\n");
			return StackSample::Failed;
		}
		std::this_thread::yield();
	}
	ProbeDepth.store(-1, std::memory_order_release);
	ProbeDecision.store(ProbeWait, std::memory_order_release);
	if (const auto error = pthread_kill(worker, SIGUSR1)) {
		fprintf(stderr,
				"Font shutdown regression: probe_error=pthread_kill code=%d\n",
				error);
		return StackSample::Failed;
	}
	auto depth = -1;
	while ((depth = ProbeDepth.load(std::memory_order_acquire)) < 0) {
		if (Clock::now() >= deadline) {
			ProbeDecision.store(ProbeRelease, std::memory_order_release);
			fprintf(stderr, "Font shutdown regression: "
							"probe_error=handler_ack_timeout\n");
			return StackSample::Failed;
		}
		std::this_thread::yield();
	}
	depth = std::min(depth, kProbeMaxFrames);
	void *frames[kProbeMaxFrames];
	std::copy(ProbeFrames, ProbeFrames + depth, frames);
	const auto inEnumeration = FramesInSystemEnumeration(frames, depth);
	if (!inEnumeration) {
		lastOtherWork.count = std::min(depth, kProbeReportedFrames);
		std::copy_n(frames, lastOtherWork.count, lastOtherWork.frames);
	}
	ProbeDecision.store(inEnumeration ? ProbeHold : ProbeRelease,
						std::memory_order_release);
	return inEnumeration ? StackSample::InSystemEnumeration
						 : StackSample::OtherWork;
}

class FontStartupSplash final : public QWindow {
  public:
	explicit FontStartupSplash(std::function<void()> onShown)
		: _backingStore(this), _onShown(std::move(onShown)),
		  _logo(Window::Logo()) {
		setFlags(Qt::SplashScreen | Qt::FramelessWindowHint);
		resize(_logo.size());
		if (const auto screen = QGuiApplication::primaryScreen()) {
			const auto geometry = screen->availableGeometry();
			setPosition(geometry.center() - QPoint(width() / 2, height() / 2));
		}
	}

  protected:
	void exposeEvent(QExposeEvent *event) override {
		QWindow::exposeEvent(event);
		if (!isExposed()) {
			return;
		}
		paint();
		if (qEnvironmentVariableIsSet(
				"TDESKTOP_FONT_STARTUP_REGRESSION_SKIP_EXPOSE_CALLBACK")) {
			return;
		}
		if (_onShown) {
			auto onShown = std::move(_onShown);
			_onShown = nullptr;
			onShown();
		}
	}

	void resizeEvent(QResizeEvent *event) override {
		QWindow::resizeEvent(event);
		paint();
	}

  private:
	void paint() {
		if (!isExposed()) {
			return;
		}
		const auto bounds = QRect(QPoint(), size());
		const auto region = QRegion(bounds);
		_backingStore.resize(size());
		_backingStore.beginPaint(region);
		{
			auto painter = QPainter(_backingStore.paintDevice());
			painter.fillRect(
				bounds, QGuiApplication::palette().color(QPalette::Window));
			painter.drawImage(bounds, _logo);
		}
		_backingStore.endPaint();
		_backingStore.flush(region, this);
	}

	QBackingStore _backingStore;
	std::function<void()> _onShown;
	const QImage &_logo;
};
#endif // Q_OS_MAC

} // namespace

const char kOptionDeadlockDetector[] = "deadlock-detector";

bool Sandbox::QuitOnStartRequested = false;
bool Sandbox::SystemShuttingDown = false;

Sandbox::Sandbox(int &argc, char **argv)
: QApplication(argc, argv)
, _mainThreadId(QThread::currentThreadId()) {
#ifdef Q_OS_MAC
	_fontStartupTimer.start();
#endif // Q_OS_MAC
}

int Sandbox::start() {
	const auto drainShutdown = [this] {
		const auto pending = [this] {
			return _closingApplication
#ifdef Q_OS_MAC
				   || (_fontDatabaseWarmupThread.joinable()
					   && !_fontDatabaseWarmupDone)
#endif // Q_OS_MAC
				;
		};
		if (pending()) {
			// Keep the GUI event loop available while shutdown integrations
			// drain.
			_shutdownLoop.run(pending);
		}
	};
	auto localServerNameError = QString();
	{
		const auto d = QFile::encodeName(QDir(cWorkingDir()).absolutePath());
		char h[33] = {0};
		hashMd5Hex(d.constData(), d.size(), h);
		_localServerName
			= Platform::SingleInstanceLocalServerName(h, &localServerNameError);
	}

#if !defined Q_OS_WIN && !defined Q_OS_MAC
	if (_localServerName.isEmpty()) {
		const auto message
			= localServerNameError.isEmpty()
				  ? u"Could not determine a private local socket path."_q
				  : localServerNameError;
		LOG(("%1").arg(message));
		ShowSingleInstanceStartupError(message);
		return 1;
	}
#endif // !Q_OS_WIN && !Q_OS_MAC

#ifdef Q_OS_MAC
	if (_localServerName.isEmpty()) {
		LOG(("Mac local socket directory is unavailable."));
		return 1;
	}
	const auto encodedSocketName = QFile::encodeName(_localServerName);
	auto socketAddress = sockaddr_un();
	const auto socketPathLimit = sizeof(socketAddress.sun_path) - 1;
	if (size_t(encodedSocketName.size()) > socketPathLimit) {
		LOG(("Mac local socket path too long: %1 bytes exceeds %2: %3")
				.arg(encodedSocketName.size())
				.arg(socketPathLimit)
				.arg(_localServerName));
		return 1;
	}
	if (MacProtectedPath::IsActive()) {
		const auto socketDirectory = QFileInfo(_localServerName).absolutePath();
		if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Open,
										 _localServerName, Q_FUNC_INFO)
			|| !MacProtectedPath::CheckPath(
				MacProtectedPath::Operation::OpenDir, socketDirectory,
				Q_FUNC_INFO)
			|| !MacProtectedPath::CheckPath(MacProtectedPath::Operation::Mkdir,
											socketDirectory, Q_FUNC_INFO)
			|| !QDir().mkpath(socketDirectory)) {
			LOG(("Could not prepare single-instance IPC path."));
			return 1;
		}
	}
#endif // Q_OS_MAC

	if (cLaunchMode() == LaunchModeCleanup) {
		const auto result = stopRunningInstance();
		psCleanup();
		closeApplication();
		return result;
	}
	{
		auto lockIdentity = _localServerName;
#ifdef Q_OS_LINUX
		// The endpoint can differ if session runtime creation races startup.
		lockIdentity = Platform::Linux::SingleInstanceProfileLockIdentity(
			_localServerName, KSandbox::isSnap());
#endif // Q_OS_LINUX
		const auto encoded = QFile::encodeName(lockIdentity);
		char hash[33] = {0};
		hashMd5Hex(encoded.constData(), encoded.size(), hash);
		const auto lockDirectory = QDir(cWorkingDir()).absolutePath();
		auto lockPath = lockDirectory + u"/Teagram-profile-"_q
						+ QString::fromLatin1(hash) + u".lock"_q;
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
		if (qEnvironmentVariableIsSet("TDESKTOP_PROFILE_LOCK_REGRESSION")) {
			lockPath = lockDirectory;
		}
		if (qEnvironmentVariableIsSet(
				"TDESKTOP_PROFILE_LOCK_REFUSAL_REGRESSION")
			&& MacProtectedPath::IntegrationTestActive()) {
			lockPath
				= qEnvironmentVariable("TDESKTOP_MAC_PROFILE_TEST_HOME")
				  + u"/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/refused-profile.lock"_q;
		}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
#ifdef Q_OS_MAC
		auto refusedOperation = QString();
		if (lockDirectory.isEmpty()) {
			refusedOperation = u"directory"_q;
		} else {
			const auto operations = std::array{
				std::pair{MacProtectedPath::Operation::Stat, "stat"},
				std::pair{MacProtectedPath::Operation::Open, "open"},
				std::pair{MacProtectedPath::Operation::Write, "write"},
				std::pair{MacProtectedPath::Operation::Lock, "lock"},
			};
			for (const auto &[operation, name] : operations) {
				if (!MacProtectedPath::CheckPath(operation, lockPath,
												 Q_FUNC_INFO)) {
					refusedOperation = QString::fromLatin1(name);
					break;
				}
			}
		}
		if (!refusedOperation.isEmpty()) {
			const auto diagnostic
				= u"Protected profile lock path refused (%1): %2"_q.arg(
					refusedOperation, lockPath);
			LOG((diagnostic));
			if (QuitOnStartRequested) {
				closeApplication();
				return 1;
			}
			QTimer::singleShot(0, this, [this, diagnostic, refusedOperation] {
				showSingleInstanceError(diagnostic, refusedOperation);
			});
			_started = true;
			exec();
			closeApplication();
			return 1;
		}
#endif // Q_OS_MAC
		_singleInstanceLock = std::make_unique<SingleInstanceLock>(lockPath);
	}

	if (!Core::UpdaterDisabled()) {
		_updateChecker = std::make_unique<Core::UpdateChecker>();
	}

	{
		const auto d = QFile::encodeName(cExeDir() + cExeName());
		QByteArray h;
		h.resize(32);
		hashMd5Hex(d.constData(), d.size(), h.data());
#ifdef Q_OS_MAC
		const auto ipcDirectory = MacProtectedPath::IpcDirectory();
		if (ipcDirectory.isEmpty()) {
			LOG(("Mac IPC directory is unavailable for the instance lock."));
			return 1;
		}
		const auto lockPath = MacProtectedPath::IsActive()
								  ? ipcDirectory + u"/Teagram-lock-"_q
										+ QString::fromLatin1(h.left(16))
								  : ipcDirectory + '/' + h + '-' + cGUIDStr();
		if (MacProtectedPath::IsActive()) {
			if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Stat,
											 lockPath, Q_FUNC_INFO)
				|| !MacProtectedPath::CheckPath(
					MacProtectedPath::Operation::Unlink, lockPath, Q_FUNC_INFO)
				|| !MacProtectedPath::CheckPath(
					MacProtectedPath::Operation::Lock, lockPath, Q_FUNC_INFO)) {
				return 1;
			}
		}
		_lockFile = std::make_unique<QLockFile>(lockPath);
#else  // Q_OS_MAC
		_lockFile = std::make_unique<QLockFile>(QDir::tempPath() + '/' + h + '-' + cGUIDStr());
#endif // !Q_OS_MAC
		_lockFile->setStaleLockTime(0);
		if (!_lockFile->tryLock()
			&& Launcher::Instance().customWorkingDir()) {
			// On Windows, QLockFile has problems detecting a stale lock
			// if the machine's hostname contains characters outside the US-ASCII character set.
			if constexpr (Platform::IsWindows()) {
				// QLockFile::removeStaleLockFile returns false on Windows,
				// when the application owning the lock is still running.
				if (!_lockFile->removeStaleLockFile()) {
					gManyInstance = true;
				}
			} else {
				gManyInstance = true;
			}
		}
	}

#ifdef Q_OS_LINUX
	_localServer.setSocketOptions(QLocalServer::UserAccessOption);
#endif // Q_OS_LINUX

	connect(
		&_localSocket,
		&QLocalSocket::connected,
		[=] { socketConnected(); });
	connect(
		&_localSocket,
		&QLocalSocket::disconnected,
		[=] { socketDisconnected(); });
	connect(
		&_localSocket,
		&QLocalSocket::errorOccurred,
		[=](QLocalSocket::LocalSocketError error) { socketError(error); });
	connect(
		&_localSocket,
		&QLocalSocket::bytesWritten,
		[=](qint64 bytes) { socketWritten(bytes); });
	connect(
		&_localSocket,
		&QLocalSocket::readyRead,
		[=] { socketReading(); });
	connect(
		&_localServer,
		&QLocalServer::newConnection,
		[=] { newInstanceConnected(); });

	crl::on_main(this, [=] { checkForQuit(); });
	connect(this, &QCoreApplication::aboutToQuit, [=] {
		// On Windows, Qt emits aboutToQuit synchronously from its
		// WM_ENDSESSION handler (QWindowsContext::windowsProc). Running
		// closeApplication() there destroys QWindows mid-dispatch and
		// later WM_ENDSESSION messages delivered to other top-level
		// HWNDs crash on virtual dispatch through stale QWindow*. Detect
		// that path and defer cleanup to the next main-loop tick so Qt
		// finishes delivering shutdown messages on still-live windows.
		// On a normal quit, start draining here and finish teardown in the
		// shutdown event loop after exec() returns.
		if (SystemShuttingDown) {
			QMetaObject::invokeMethod(this, [=] {
				customEnterFromEventLoop([&] {
					closeApplication();
				});
			}, Qt::QueuedConnection);
		} else {
			customEnterFromEventLoop([&] {
				closeApplication();
			});
		}
	});

	// https://github.com/telegramdesktop/tdesktop/issues/948
	// and https://github.com/telegramdesktop/tdesktop/issues/5022
	connect(this, &QGuiApplication::saveStateRequest, [](auto &manager) {
		manager.setRestartHint(QSessionManager::RestartNever);
	});

	LOG(("Connecting local socket to %1...").arg(_localServerName));
#ifdef Q_OS_MAC
	if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Open,
									 _localServerName, Q_FUNC_INFO)) {
		return 1;
	}
#endif // Q_OS_MAC
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
	if (qEnvironmentVariableIsSet("TDESKTOP_SOCKET_ERROR_REGRESSION")) {
		QTimer::singleShot(0, this, [=] {
			fprintf(stderr, "Socket error regression: injected=1\n");
			const auto mode
				= qEnvironmentVariable("TDESKTOP_SOCKET_ERROR_REGRESSION");
			if (mode == u"ipc-collision"_q) {
				auto entry = QFile(_localServerName);
				const auto contents
					= QByteArray(kSocketCollisionFixtureContents);
				if (!entry.open(QIODevice::WriteOnly | QIODevice::NewOnly)
					|| entry.write(contents) != contents.size()) {
					fprintf(
						stderr,
						"Socket error regression: collision_setup=failed\n");
					QCoreApplication::exit(1);
					return;
				}
			}
			socketError(
				(mode == u"unknown"_q)		? QLocalSocket::UnknownSocketError
				: (mode == u"persistent"_q) ? QLocalSocket::SocketAccessError
				: (mode == u"ipc-collision"_q)
					? QLocalSocket::ServerNotFoundError
					: QLocalSocket::SocketTimeoutError);
		});
	} else
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
	{
		_localSocket.connectToServer(_localServerName);
	}

	if (QuitOnStartRequested) {
		closeApplication();
		drainShutdown();
		return 0;
	}
	_started = true;
	const auto result = exec();
	if (_application && CurrentLaunchState() != LaunchState::QuitProcessed) {
		closeApplication();
	}
	drainShutdown();
#ifdef Q_OS_MAC
	// closeApplication() has run from aboutToQuit by now. Wait for the font
	// warmup worker so it never outlives the Qt and style state it touches.
	if (_fontDatabaseWarmupThread.joinable()) {
		_fontDatabaseWarmupThread.join();
	}
	if (qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_QUIT_REGRESSION")) {
		fprintf(stderr,
				"Font shutdown regression: worker_joined=1 "
				"application_created=%d teardown=complete\n",
				_applicationCreated ? 1 : 0);
	}
#endif // Q_OS_MAC
	return result;
}

int Sandbox::stopRunningInstance() {
	LOG(("Cleanup: connecting to %1...").arg(_localServerName));
#ifdef Q_OS_MAC
	if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Open,
									 _localServerName, Q_FUNC_INFO)) {
		return 1;
	}
#endif // Q_OS_MAC
	_localSocket.connectToServer(_localServerName);
	if (!_localSocket.waitForConnected(int(kCleanupIpcTimeout))) {
		if (_localSocket.error() == QLocalSocket::ServerNotFoundError) {
			LOG(("Cleanup: no running instance found."));
			return 0;
		}
		LOG(("Cleanup: connect error %1.").arg(_localSocket.error()));
		return 1;
	}
	_localSocket.write("CMD:quit;");
	if (!_localSocket.waitForBytesWritten(int(kCleanupIpcTimeout))) {
		LOG(("Cleanup: could not send the quit command."));
		return 1;
	}
	auto response = QByteArray();
	const auto deadline = crl::now() + kCleanupIpcTimeout;
	while (!response.contains(';')) {
		const auto timeout = deadline - crl::now();
		if (timeout <= 0 || !_localSocket.waitForReadyRead(int(timeout))) {
			LOG(("Cleanup: no response to the quit command."));
			return 1;
		}
		response.append(_localSocket.readAll());
	}
	const auto match = QRegularExpression(u"RES:(\\d+)_(\\d+);"_q).match(
		QString::fromLatin1(response));
	if (!match.hasMatch()) {
		LOG(("Cleanup: bad response to the quit command."));
		return 1;
	}
#ifdef Q_OS_MAC
	if (MacProtectedPath::IntegrationTestActive()) {
		fprintf(stderr, "Mac profile IPC cleanup response: %s\n",
				response.constData());
	}
#endif // Q_OS_MAC
	const auto processId = match.capturedView(1).toULongLong();
	LOG(("Cleanup: waiting for process %1 to quit...").arg(processId));
	if (!Platform::WaitForProcessExit(processId, kCleanupQuitTimeout)) {
		LOG(("Cleanup: the process did not quit in time."));
		return 1;
	}
	LOG(("Cleanup: the running instance quit."));
	return 0;
}

void Sandbox::NotifySystemShuttingDown() {
	SystemShuttingDown = true;
}

void Sandbox::QuitWhenStarted() {
	if (CurrentLaunchState() == LaunchState::QuitProcessed) {
		return;
	}
	if (!QApplication::instance() || !Instance()._started) {
		QuitOnStartRequested = true;
	} else {
		// Use exit(0) instead of quit() to avoid recursive
		// [NSApp terminate:] on macOS. Since Qt 6.0, quit() routes
		// through QCocoaIntegration::quit() -> [NSApp terminate:],
		// which when called from within applicationShouldTerminate:
		// causes a nested terminate that leads to exit() being called
		// directly, bypassing normal cleanup. exit(0) properly exits
		// event loops without going through the platform plugin.
		QCoreApplication::exit(0);
	}
}

void Sandbox::launchApplication() {
	InvokeQueued(this, [=] {
		if (Quitting()) {
			quit();
		} else if (_application) {
			return;
		}
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
		if (!_uiStallWatchdog) {
			_uiStallWatchdog = std::make_unique<UiStallWatchdog>(
				[](const QString &line, bool update) {
					Logs::writeMainUiStall(line, update);
				},
				UiStallWatchdog::Settings{
					.captureShortStalls = qEnvironmentVariableIsSet(
						"TDESKTOP_UI_STALL_WATCHDOG_CACHE_REGRESSION"),
					.disableRateLimit = qEnvironmentVariableIsSet(
						"TDESKTOP_FONT_STARTUP_REGRESSION"),
					.captureSchedulerTrace = qEnvironmentVariableIsSet(
						"TDESKTOP_UI_STALL_WATCHDOG_CACHE_REGRESSION"),
				});
		}
#endif // Q_OS_LINUX || Q_OS_MAC
#ifdef Q_OS_MAC
		if (!_fontStartupWindow) {
			_launchAfterFontDatabaseWarmup = true;
			_fontStartupWindow = std::make_unique<FontStartupSplash>([this] {
				if (qEnvironmentVariableIsSet(
						"TDESKTOP_FONT_STARTUP_REGRESSION")) {
					const auto visible = _fontStartupWindow->isVisible()
										 && _fontStartupWindow->isExposed();
					fprintf(stderr,
							"Font startup splash "
							"regression: visible=%d "
							"wall_ns=%lld\n",
							visible ? 1 : 0,
							static_cast<long long>(
								_fontStartupTimer.nsecsElapsed()));
				}
				startFontDatabaseWarmup();
			});
			_fontStartupWindow->show();
			QTimer::singleShot(kFontStartupSplashExposureTimeout, this, [this] {
				if (_fontDatabaseWarmupStarted || Quitting()) {
					return;
				}
				if (qEnvironmentVariableIsSet(
						"TDESKTOP_FONT_STARTUP_REGRESSION")) {
					fprintf(stderr,
							"Font startup exposure timeout regression: "
							"wall_ns=%lld\n",
							static_cast<long long>(
								_fontStartupTimer.nsecsElapsed()));
				}
				startFontDatabaseWarmup();
			});
			return;
		}
		if (!_fontDatabaseWarmupDone) {
			_launchAfterFontDatabaseWarmup = true;
			return;
		}
		if (qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_REGRESSION")) {
			fprintf(stderr, "Font database warmup regression: ready=%d\n",
					_fontDatabaseWarmupSucceeded ? 1 : 0);
		}
		Platform::CreateGlobalMenu();
#endif // Q_OS_MAC
		setupScreenScale();

		rpl::single(
			rpl::empty
		) | rpl::then(
			OptionDeadlockDetector.changes()
		) | rpl::on_next([=] {
			using DeadlockDetector::PingThread;
			// The test agent always wants a stuck main thread to crash with a
			// report instead of hanging silently, so force it on for -testagent.
			_deadlockDetector = (OptionDeadlockDetector.value() || cTestAgent())
				? std::make_unique<PingThread>(this)
				: nullptr;
		}, _lifetime);

#ifdef Q_OS_MAC
		_applicationCreated = true;
#endif // Q_OS_MAC
		_application = std::make_unique<Application>();

		// Ideally this should go to constructor.
		// But we want to catch all native events and Application installs
		// its own filter that can filter out some of them. So we install
		// our filter after the Application constructor installs his.
		installNativeEventFilter(this);

		_application->run();
#ifdef Q_OS_MAC
		const auto trayRegression
			= MacProtectedPath::IntegrationTestActive()
			  && qEnvironmentVariableIsSet(
				  "TDESKTOP_MAC_START_IN_TRAY_REGRESSION");
		const auto splashVisible = trayRegression && _fontStartupWindow
								   && _fontStartupWindow->isVisible();
		_fontStartupWindow.reset();
		if (cStartInTray()) {
			using WorkMode = Core::Settings::WorkMode;
			const auto workMode = _application->settings().workMode();
			if ((workMode == WorkMode::TrayOnly
				 || workMode == WorkMode::WindowAndTray)
				&& _application->activePrimaryWindow()) {
				// Delayed startup can activate the app after we hide it.
				// macOS reopens hidden windows on activation.
				Platform::IgnoreNextApplicationActivation();
				_application->activePrimaryWindow()->widget()->hide();
			}
		}
		if (trayRegression) {
			QTimer::singleShot(500, this, [=] {
				const auto window = _application
										? _application->activePrimaryWindow()
										: nullptr;
				fprintf(stderr,
						"Mac start-in-tray regression: startup=complete "
						"start_intray=%d window_and_tray=%d hidden=%d "
						"splash_visible=%d "
						"splash_destroyed=%d\n",
						cStartInTray() ? 1 : 0,
						(_application
						 && _application->settings().workMode()
								== Core::Settings::WorkMode::WindowAndTray)
							? 1
							: 0,
						(window && !window->widget()->isVisible()) ? 1 : 0,
						splashVisible ? 1 : 0, _fontStartupWindow ? 0 : 1);
			});
			QTimer::singleShot(1000, this, [=] {
				const auto window = _application
										? _application->activePrimaryWindow()
										: nullptr;
				fprintf(stderr,
						"Mac start-in-tray regression: "
						"delayed_activation_check hidden=%d\n",
						(window && !window->widget()->isVisible()) ? 1 : 0);
			});
		}
#endif // Q_OS_MAC
	});
}

#ifdef Q_OS_MAC
void Sandbox::startFontDatabaseWarmup() {
	if (_fontDatabaseWarmupStarted) {
		return;
	}
	_fontDatabaseWarmupStarted = true;
	style::internal::PrepareFontSubstitutions();
	const auto quitRegression
		= qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_QUIT_REGRESSION");
	const auto testFailure = MacProtectedPath::IntegrationTestActive()
								 ? qgetenv("TDESKTOP_FONT_STARTUP_TEST_FAILURE")
								 : QByteArray();
	const auto ignoreFirstEnumeration
		= quitRegression ? std::make_shared<std::atomic_bool>(
							   testFailure == "probe-missed-first")
						 : nullptr;
	const auto workerFinished
		= quitRegression ? std::make_shared<std::atomic_bool>(false) : nullptr;
	const auto shutdownRemovalPending
		= (quitRegression && testFailure == "shutdown-remove-pending")
			  ? std::make_shared<std::atomic_bool>(false)
			  : nullptr;
	const auto shutdownRemovalPendingAtQuit
		= shutdownRemovalPending ? std::make_shared<std::atomic_bool>(false)
								 : nullptr;
	const auto shutdownRemovalCompleted
		= shutdownRemovalPending ? std::make_shared<std::atomic_bool>(false)
								 : nullptr;
	const auto retryCheckpoint
		= (quitRegression && testFailure == "probe-timeout-retry")
			  ? std::make_shared<std::atomic_bool>(false)
			  : nullptr;
	if (quitRegression) {
		// The main thread samples the worker stack until it catches the
		// system font enumeration executing, holds the worker there, and only
		// then requests the real quit. The worker is released after
		// aboutToQuit, so teardown runs with the enumeration still in flight.
		if (!InstallProbeSignal(testFailure == "signal-install")) {
			fprintf(stderr, "Font shutdown regression: probe=unavailable\n");
			QCoreApplication::exit(1);
			return;
		}
		connect(this, &QCoreApplication::aboutToQuit, [=] {
			_fontStartupQuitObserved = true;
			if (shutdownRemovalPendingAtQuit) {
				shutdownRemovalPendingAtQuit->store(
					shutdownRemovalPending->load(std::memory_order_acquire),
					std::memory_order_release);
			}
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
			Core::Tests::ReleaseFontShutdownProbe();
#endif
			ProbeFinalRelease.store(true, std::memory_order_release);
		});
	}
	try {
		if (testFailure == "thread-start"
			&& qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_REGRESSION")) {
			throw std::system_error(std::make_error_code(
				std::errc::resource_unavailable_try_again));
		}
		_fontDatabaseWarmupThread = std::thread([this, quitRegression,
												 testFailure,
												 ignoreFirstEnumeration,
												 workerFinished,
												 shutdownRemovalPending,
												 shutdownRemovalPendingAtQuit,
												 shutdownRemovalCompleted,
												 retryCheckpoint] {
			const auto finished = gsl::finally([=] {
				if (retryCheckpoint) {
					fprintf(stderr,
							"Font shutdown regression: retry_worker_done=1\n");
				}
				if (workerFinished) {
					workerFinished->store(true, std::memory_order_release);
				}
				QMetaObject::invokeMethod(
					this,
					[this] {
						_fontDatabaseWarmupDone = true;
						_shutdownLoop.quit();
					},
					Qt::QueuedConnection);
			});
			if (quitRegression && testFailure == "probe-timeout") {
				// Keep the test worker outside enumeration until sampled.
				while (!ProbeFinalRelease.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				return;
			}
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
			if (quitRegression && testFailure == "probe-release-before-arm") {
				while (!ProbeFinalRelease.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
			}
			if (quitRegression
				&& (testFailure.isEmpty() || testFailure == "probe-missed-first"
					|| testFailure == "probe-release-before-arm")) {
				const auto skippedPopulations
					= (ignoreFirstEnumeration
					   && ignoreFirstEnumeration->load(
						   std::memory_order_acquire))
						  ? 1
						  : 0;
				Core::Tests::ArmFontShutdownProbe(pthread_self(),
												  skippedPopulations);
				if (testFailure == "probe-release-before-arm"
					&& Core::Tests::IsFontShutdownProbeReleased()) {
					fprintf(stderr, "Font shutdown regression: "
									"probe_release_preserved=1\n");
				}
			}
#endif
			const auto openSansLoaded = style::internal::PreloadCustomFonts();
			const auto populateFontDatabase = [] {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
				const auto families = QFontDatabase().families();
#else  // Qt < 6.0.0
				const auto families = QFontDatabase::families();
#endif // Qt < 6.0.0
				Q_UNUSED(families);
			};
			populateFontDatabase();
			if (ignoreFirstEnumeration) {
				ignoreFirstEnumeration->store(false, std::memory_order_release);
			}
			auto retryReported = false;
			while (quitRegression
				   && !ProbeFinalRelease.load(std::memory_order_acquire)) {
				if (!retryReported && testFailure == "probe-missed-first") {
					fprintf(stderr,
							"Font shutdown regression: enumeration_retry=1\n");
					retryReported = true;
				}
				const auto fontId = QFontDatabase::addApplicationFont(
					u":/gui/fonts/OpenSans-Regular.ttf"_q);
				if (fontId >= 0 && retryCheckpoint) {
					fprintf(stderr,
							"Font shutdown regression: retry_checkpoint=1\n");
					retryCheckpoint->store(true, std::memory_order_release);
					while (!ProbeFinalRelease.load(std::memory_order_acquire)) {
						std::this_thread::yield();
					}
					if (!QMetaObject::invokeMethod(
							this,
							[] {
								fprintf(stderr, "Font shutdown regression: "
												"retry_gui_event=1\n");
							},
							Qt::BlockingQueuedConnection)) {
						fprintf(stderr, "Font shutdown regression: "
										"probe_error=retry_gui_event\n");
						return;
					}
				}
				if (fontId >= 0 && shutdownRemovalPending) {
					shutdownRemovalPending->store(true,
												  std::memory_order_release);
					while (!ProbeFinalRelease.load(std::memory_order_acquire)) {
						std::this_thread::yield();
					}
				}
				if (fontId < 0
					|| !QFontDatabase::removeApplicationFont(fontId)) {
					fprintf(stderr, "Font shutdown regression: "
									"probe_error=font_database_invalidation\n");
					QMetaObject::invokeMethod(
						this, [] { QCoreApplication::exit(1); },
						Qt::QueuedConnection);
					return;
				}
				if (shutdownRemovalCompleted) {
					shutdownRemovalCompleted->store(true,
													std::memory_order_release);
				}
				// Removing the application font invalidates Qt's database; this
				// query retries the native population while Qt holds its mutex.
				populateFontDatabase();
			}
			const auto prewarmFontMetrics = [](QString family) {
				auto font = QFont(std::move(family));
				const auto sample
					= u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
					  u"0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"_q;
				static constexpr auto pixelSizes = std::array{
					12, 13, 14, 16, 18, 20,
				};
				static constexpr auto weights
					= std::array{QFont::Normal, QFont::DemiBold};
				auto image
					= QImage(2048, 384, QImage::Format_ARGB32_Premultiplied);
				image.fill(Qt::transparent);
				auto painter = QPainter(&image);
				auto row = 0;
				for (const auto pixelSize : pixelSizes) {
					for (const auto weight : weights) {
						font.setPixelSize(pixelSize);
						font.setWeight(weight);
						const auto metrics = QFontMetricsF(font);
						static_cast<void>(metrics.ascent());
						static_cast<void>(metrics.height());
						// Prime the text-width queries used by style::FontData.
						static_cast<void>(
							metrics.horizontalAdvance(QLatin1Char(' ')));
						static_cast<void>(metrics.horizontalAdvance(u"..."_q));
						static_cast<void>(metrics.tightBoundingRect(sample));
						painter.setFont(font);
						painter.drawText(QPoint(0, 24 + row * 32), sample);
						++row;
					}
				}
			};
			prewarmFontMetrics(QFont().family());
			prewarmFontMetrics(u"Open Sans"_q);
			if (quitRegression) {
				if (shutdownRemovalPending) {
					fprintf(stderr,
							"Font shutdown regression: quit_observed=%d "
							"font_work=active "
							"remove_pending_at_quit=%d removal_completed=%d\n",
							_fontStartupQuitObserved ? 1 : 0,
							shutdownRemovalPendingAtQuit->load(
								std::memory_order_acquire)
								? 1
								: 0,
							shutdownRemovalCompleted->load(
								std::memory_order_acquire)
								? 1
								: 0);
				} else {
					fprintf(stderr,
							"Font shutdown regression: quit_observed=%d "
							"font_work=active enumeration_overlapped=%d\n",
							_fontStartupQuitObserved ? 1 : 0,
							_fontEnumerationOverlapped ? 1 : 0);
				}
			}
			if (qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_REGRESSION")) {
				fprintf(stderr,
						"Font database metrics warmup regression: ready=1\n");
			}
			QMetaObject::invokeMethod(
				this,
				[this, openSansLoaded] {
					style::internal::SetPreloadedCustomFonts(openSansLoaded);
					_fontDatabaseWarmupDone = true;
					_fontDatabaseWarmupSucceeded = true;
					if (qEnvironmentVariableIsSet(
							"TDESKTOP_FONT_STARTUP_REGRESSION")) {
						fprintf(stderr,
								"Font database warmup regression: ready=1 "
								"custom-fonts=1\n");
					}
					if (_launchAfterFontDatabaseWarmup && !Quitting()) {
						_launchAfterFontDatabaseWarmup = false;
						launchApplication();
					}
					_shutdownLoop.quit();
				},
				Qt::QueuedConnection);
		});
		if (qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_REGRESSION")) {
			fprintf(stderr, "Font database warmup regression: started=1\n");
		}
		if (quitRegression) {
			const auto timer = Ui::CreateChild<QTimer>(this);
			const auto started = std::make_shared<QElapsedTimer>();
			const auto lastOtherWork = std::make_shared<ProbeFrameSnapshot>();
			const auto pendingExit
				= std::make_shared<ProbeExit>(ProbeExit::None);
			const auto stopWorker = [=](ProbeExit exitMode) {
				_launchAfterFontDatabaseWarmup = false;
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
				Core::Tests::ReleaseFontShutdownProbe();
#endif
				ProbeFinalRelease.store(true, std::memory_order_release);
				*pendingExit = exitMode;
			};
			started->start();
			connect(timer, &QTimer::timeout, this, [=] {
				if (*pendingExit != ProbeExit::None) {
					if (!workerFinished->load(std::memory_order_acquire)) {
						return;
					}
					timer->stop();
					if (*pendingExit == ProbeExit::Quit) {
						Core::Quit();
					} else {
						QCoreApplication::exit(1);
					}
					return;
				}
				if (testFailure == "probe-release-before-arm") {
					stopWorker(ProbeExit::Quit);
					return;
				}
				if (retryCheckpoint) {
					if (retryCheckpoint->load(std::memory_order_acquire)) {
						fprintf(stderr,
								"Font shutdown regression: retry_timeout=1\n");
						stopWorker(ProbeExit::Quit);
					} else if (started->elapsed() >= 10000) {
						fprintf(stderr,
								"Font shutdown regression: "
								"probe_error=retry_checkpoint_timeout\n");
						stopWorker(ProbeExit::Failure);
					}
					return;
				}
				if (shutdownRemovalPending) {
					if (shutdownRemovalPending->load(
							std::memory_order_acquire)) {
						_launchAfterFontDatabaseWarmup = false;
						timer->stop();
						Core::Quit();
					} else if (started->elapsed() >= 10000) {
						fprintf(stderr, "Font shutdown regression: "
										"remove_checkpoint_timeout=1\n");
						stopWorker(ProbeExit::Failure);
					}
					return;
				}
				if (ignoreFirstEnumeration->load(std::memory_order_acquire)) {
					if (started->elapsed() >= 10000) {
						fprintf(stderr,
								"Font shutdown regression: "
								"probe_error=first_enumeration_timeout\n");
						stopWorker(ProbeExit::Failure);
					}
					return;
				}
#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
				if (testFailure != "probe-timeout"
					&& !Core::Tests::IsFontShutdownProbeReady()) {
					if (started->elapsed() >= 10000) {
						fprintf(stderr,
								"Font shutdown regression: "
								"probe_error=interposer_ready_timeout\n");
						stopWorker(ProbeExit::Failure);
					}
					return;
				}
#endif // TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST
				const auto sample = SampleWorkerStack(
					_fontDatabaseWarmupThread.native_handle(), *lastOtherWork);
				if (sample == StackSample::Failed) {
					stopWorker(ProbeExit::Failure);
					return;
				}
				const auto sampled
					= (sample == StackSample::InSystemEnumeration);
				if (!sampled && testFailure != "probe-timeout"
					&& started->elapsed() < 10000) {
					return;
				}
				_fontEnumerationOverlapped = sampled;
				if (!sampled) {
					PrintProbeFrameSnapshot(*lastOtherWork);
					stopWorker(ProbeExit::Quit);
					return;
				}
				fprintf(stderr,
						"Font shutdown regression: qt_font_database_hold=1\n");
				timer->stop();
				Core::Quit();
			});
			timer->start(0);
		}
	} catch (const std::system_error &error) {
		_fontDatabaseWarmupDone = true;
		if (qEnvironmentVariableIsSet("TDESKTOP_FONT_STARTUP_REGRESSION")) {
			fprintf(stderr,
					"Font database warmup regression: started=0 "
					"error=%s\n",
					error.what());
		}
		LOG(("Font database warmup could not start: %1")
				.arg(QString::fromLocal8Bit(error.what())));
		if (_launchAfterFontDatabaseWarmup && !Quitting()) {
			_launchAfterFontDatabaseWarmup = false;
			launchApplication();
		}
	}
}
#endif // Q_OS_MAC

void Sandbox::setupScreenScale() {
	const auto ratio = devicePixelRatio();
	LOG(("Global devicePixelRatio: %1").arg(ratio));
	const auto logEnv = [](const char *name) {
		const auto value = qEnvironmentVariable(name);
		if (!value.isEmpty()) {
			LOG(("%1: %2").arg(name, value));
		}
	};
	logEnv("QT_DEVICE_PIXEL_RATIO");
	logEnv("QT_AUTO_SCREEN_SCALE_FACTOR");
	logEnv("QT_ENABLE_HIGHDPI_SCALING");
	logEnv("QT_SCALE_FACTOR");
	logEnv("QT_SCREEN_SCALE_FACTORS");
	logEnv("QT_SCALE_FACTOR_ROUNDING_POLICY");
	logEnv("QT_DPI_ADJUSTMENT_POLICY");
	logEnv("QT_USE_PHYSICAL_DPI");
	logEnv("QT_FONT_DPI");

	const auto useRatio = std::clamp(qCeil(ratio), 1, 3);
	style::SetDevicePixelRatio(useRatio);

	const auto screen = Sandbox::primaryScreen();
	const auto dpi = screen->logicalDotsPerInch();
	const auto basePair = screen->handle()->logicalBaseDpi();
	const auto base = (basePair.first + basePair.second) * 0.5;
	const auto screenScaleExact = dpi / base;
	const auto screenScale = int(base::SafeRound(screenScaleExact * 20)) * 5;
	LOG(("Primary screen DPI: %1, Base: %2.").arg(dpi).arg(base));
	LOG(("Computed screen scale: %1").arg(screenScale));
	if (Platform::IsMac()) {
		// 110% for Retina screens by default.
		cSetScreenScale((useRatio == 2) ? 110 : style::kScaleDefault);
	} else {
		cSetScreenScale(std::clamp(
			screenScale,
			style::kScaleMin,
			style::MaxScaleForRatio(useRatio)));
	}
	LOG(("DevicePixelRatio: %1").arg(useRatio));
	LOG(("ScreenScale: %1").arg(cScreenScale()));
}

Sandbox::~Sandbox() {
#ifdef Q_OS_MAC
	if (_fontDatabaseWarmupThread.joinable()) {
		_fontDatabaseWarmupThread.join();
	}
	Platform::DestroyGlobalMenu();
#endif // Q_OS_MAC
}

bool Sandbox::event(QEvent *e) {
	if (e->type() == QEvent::Quit) {
		if (CurrentLaunchState() == LaunchState::QuitProcessed) {
			e->accept();
			return true;
		}
		if (Quitting()) {
			return QCoreApplication::event(e);
		}
		Quit(QuitReason::QtQuitEvent);
		e->ignore();
		return false;
	} else if (e->type() == QEvent::Close) {
		Quit();
	} else if (e->type() == DeadlockDetector::PingPongEvent::Type()) {
		postEvent(
			static_cast<DeadlockDetector::PingPongEvent*>(e)->sender(),
			new DeadlockDetector::PingPongEvent(this));
	}
	return QApplication::event(e);
}

void Sandbox::socketConnected() {
	LOG(("Socket connected, this is not the first application instance, sending show command..."));
	_unexpectedSingleInstanceErrors = 0;
	if (_singleInstanceLock && _singleInstanceLock->isAcquired()) {
		_singleInstanceLock->release();
	}
	_secondInstance = true;

	QString commands;
	if (qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN")) {
		commands += u"XDG_ACTIVATION_TOKEN:"_q + qgetenv("XDG_ACTIVATION_TOKEN").toBase64() + ';';
	}
	for (const auto &url : cRefStartUrls()) {
		commands += u"OPEN:"_q + url.toString(QUrl::FullyEncoded) + ';';
	}
	if (cQuit()) {
		commands += u"CMD:quit;"_q;
	} else if (cRefStartUrls().isEmpty()) {
		commands += u"CMD:show;"_q;
	}

	DEBUG_LOG(("Sandbox Info: writing commands %1").arg(commands));
	_localSocket.write(commands.toLatin1());
}

void Sandbox::socketWritten(qint64/* bytes*/) {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	if (_localSocket.bytesToWrite()) {
		return;
	}
	LOG(("Show command written, waiting response..."));
}

void Sandbox::socketReading() {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	_localSocketReadData.append(_localSocket.readAll());
	const auto m = QRegularExpression(u"RES:(\\d+)_(\\d+);"_q).match(
		_localSocketReadData);
	if (!m.hasMatch()) {
		return;
	}
	const auto processId = m.capturedView(1).toULongLong();
	const auto windowId = m.capturedView(2).toULongLong();
	if (windowId) {
		Platform::ActivateOtherProcess(processId, windowId);
	}
	LOG(("Show command response received, processId = %1, windowId = %2, "
		"activating and quitting..."
		).arg(processId
		).arg(windowId));
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
	if (qEnvironmentVariableIsSet("TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE")) {
		fprintf(stderr, "Emoji cache relaunch: forwarded=1\n");
	}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
	return Quit();
}

void Sandbox::socketError(QLocalSocket::LocalSocketError e) {
	if (Quitting()) return;

	if (_secondInstance) {
		if (e == QLocalSocket::PeerClosedError) {
			return;
		}
		LOG(("Could not write show command, error %1, quitting...").arg(e));
		return Quit();
	}
	if (e != QLocalSocket::ServerNotFoundError
		&& e != QLocalSocket::ConnectionRefusedError) {
		LOG(("Single instance connection failed: %1 (%2)")
				.arg(e)
				.arg(_localSocket.errorString()));
		_localSocket.close();
		if (++_unexpectedSingleInstanceErrors
			>= kUnexpectedSingleInstanceErrorLimit) {
			return showSingleInstanceError();
		}
		return retrySingleInstance();
	}
	_unexpectedSingleInstanceErrors = 0;

	if (e == QLocalSocket::ServerNotFoundError) {
		LOG(("This is the only instance of Telegram, starting server and app..."));
	} else {
		LOG(("Socket connect error %1, starting server and app...").arg(e));
	}
	_localSocket.close();
#ifndef Q_OS_WINRT
	if (!_singleInstanceLock->isAcquired()) {
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
		const auto holdFile
			= qEnvironmentVariable("TDESKTOP_INSTANCE_RECHECK_HOLD_FILE");
		if (!holdFile.isEmpty() && !QFile::exists(holdFile)) {
			const auto name = QFile::encodeName(_localServerName);
			fprintf(stderr,
					"Single instance recheck regression: "
					"initial_failure=1 socket=%s\n",
					name.constData());
			const auto timer = new QTimer(this);
			connect(timer, &QTimer::timeout, this, [=] {
				if (QFile::exists(holdFile)) {
					timer->stop();
					timer->deleteLater();
					socketError(e);
				}
			});
			timer->start(10);
			return;
		}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
		const auto result = _singleInstanceLock->tryAcquire();
		if (result == SingleInstanceLock::Result::Error) {
			const auto diagnostic
				= u"Could not lock the single instance profile: %1"_q.arg(
					_singleInstanceLock->errorString());
			LOG((diagnostic));
			return showSingleInstanceError(diagnostic);
		}
		// Another process may have started its server after our previous
		// connection attempt. A successful acquire allows an immediate recheck.
#ifdef Q_OS_LINUX
		if (result == SingleInstanceLock::Result::Acquired) {
			if (!_singleInstanceLock->setContents(_localServerName)) {
				const auto diagnostic
					= u"Could not record the single instance IPC path: %1"_q
						  .arg(_singleInstanceLock->errorString());
				LOG((diagnostic));
				return showSingleInstanceError(diagnostic);
			}
		} else {
			const auto selectedPath
				= Platform::Linux::SingleInstanceServerPathFromLockContents(
					_singleInstanceLock->contents(), _localServerName,
					geteuid(), KSandbox::isSnap());
			if (!selectedPath.isEmpty()) {
				_localServerName = selectedPath;
			}
		}
#endif // Q_OS_LINUX
		_retryingSingleInstance = true;
		return retrySingleInstance(
			result == Core::SingleInstanceLock::Result::Acquired);
	}
#endif // !Q_OS_WINRT

	// Local server does not work in WinRT build.
#ifndef Q_OS_WINRT
#ifdef Q_OS_MAC
	if (!MacProtectedPath::CheckPath(MacProtectedPath::Operation::Stat,
									 _localServerName, Q_FUNC_INFO)
		|| !MacProtectedPath::CheckPath(MacProtectedPath::Operation::Unlink,
										_localServerName, Q_FUNC_INFO)
		|| !MacProtectedPath::CheckPath(MacProtectedPath::Operation::Open,
										_localServerName, Q_FUNC_INFO)) {
		return Quit();
	}
#endif // Q_OS_MAC
#if !defined Q_OS_WIN && !defined Q_OS_MAC
	Platform::Linux::StartSingleInstanceServer(
		_localServer, _localServerName,
		[](const QString &serverName) { psCheckLocalSocket(serverName); },
		[this](Platform::Linux::SingleInstanceServerResult result,
			   const QString &error) {
			if (Quitting()) {
				return;
			}
			if (result
				== Platform::Linux::SingleInstanceServerResult::Secondary) {
				Platform::Linux::QueueSingleInstanceSocketReconnect(
					this, &_localSocket, _localServerName);
				return;
			} else if (result
					   == Platform::Linux::SingleInstanceServerResult::Failed) {
				LOG(("Failed to start listening to %1 server: %2")
						.arg(_localServerName, error));
				ShowSingleInstanceStartupError(error);
				return Quit();
			}
			if (_retryingSingleInstance && _lockFile && !_lockFile->isLocked()
				&& !_lockFile->tryLock()) {
				LOG(("Executable lock remains held by another profile."));
			}
			_retryingSingleInstance = false;
			afterSingleInstanceStarted();
		});
	return;
#else
	if (!RemoveStaleLocalServer(_localServerName)) {
		const auto diagnostic
			= u"Could not use the single-instance IPC path: %1"_q.arg(
				_localServerName);
		LOG((diagnostic));
		return showSingleInstanceError(diagnostic);
	}

	if (!_localServer.listen(_localServerName)) {
		LOG(("Failed to start listening to %1 server: %2")
				.arg(_localServerName, _localServer.errorString()));
		return Quit();
	}
	if (_retryingSingleInstance && _lockFile && !_lockFile->isLocked()
		&& !_lockFile->tryLock()) {
		LOG(("Executable lock remains held by another profile."));
	}
	_retryingSingleInstance = false;
#endif // !Q_OS_WIN && !Q_OS_MAC
#ifdef Q_OS_MAC
	if (MacProtectedPath::IntegrationTestActive()) {
		const auto fullServerName = _localServer.fullServerName().toUtf8();
		fprintf(stderr,
				"Mac profile IPC ready: listening=%d full_server_name=%s\n",
				_localServer.isListening() ? 1 : 0, fullServerName.constData());
	}
#endif // Q_OS_MAC
#endif // !Q_OS_WINRT
	afterSingleInstanceStarted();
}

void Sandbox::afterSingleInstanceStarted() {
	if (!Core::UpdaterDisabled() && !cNoStartUpdate()
		&& Core::checkReadyUpdate()) {
		cSetRestartingUpdate(true);
		DEBUG_LOG(
			("Sandbox Info: installing update instead of starting app..."));
		return Quit();
	}

	if (cQuit()) {
		return Quit();
	}

	singleInstanceChecked();
}

void Sandbox::singleInstanceChecked() {
	if (cManyInstance()) {
		LOG(("App Info: Detected another instance"));
	}

	refreshGlobalProxy();
	if (!Logs::started() || !Logs::instanceChecked()) {
		new NotStartedWindow();
		return;
	}
	const auto result = CrashReports::Start();
	v::match(result, [&](CrashReports::Status status) {
		if (status == CrashReports::CantOpen) {
			new NotStartedWindow();
		} else {
			launchApplication();
		}
	}, [&](const QByteArray &crashdump) {
		// If crash dump is empty with that status it means that we
		// didn't close the application properly. Just ignore for now.
		if (crashdump.isEmpty()) {
			if (CrashReports::Restart() == CrashReports::CantOpen) {
				new NotStartedWindow();
			} else {
				launchApplication();
			}
			return;
		}
		_lastCrashDump = crashdump;
		auto window = new LastCrashedWindow(
			_lastCrashDump,
			[=] { launchApplication(); });
		window->proxyChanges(
		) | rpl::on_next([=](MTP::ProxyData &&proxy) {
			_sandboxProxy = std::move(proxy);
			refreshGlobalProxy();
		}, window->lifetime());
	});
}

void Sandbox::socketDisconnected() {
	if (_secondInstance && !Quitting()) {
		if (cQuit()) {
			return Quit();
		}
		_secondInstance = false;
		_localSocketReadData.clear();
		_retryingSingleInstance = true;
		retrySingleInstance();
	}
}

void Sandbox::retrySingleInstance(bool immediate) {
	if (_singleInstanceRetryScheduled || Quitting()) {
		return;
	}
	if (++_singleInstanceRetries > 600) {
		LOG(("Single instance retry timed out."));
		return showSingleInstanceError();
	}
	_singleInstanceRetryScheduled = true;
	const auto delay = immediate ? 0 : 50;
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
	if (qEnvironmentVariableIsSet("TDESKTOP_INSTANCE_RECHECK_HOLD_FILE")
		|| qEnvironmentVariableIsSet("TDESKTOP_SOCKET_ERROR_REGRESSION")) {
		fprintf(stderr, "Single instance retry regression: delay_ms=%d\n",
				delay);
	}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
	QTimer::singleShot(delay, this, [=] {
		_singleInstanceRetryScheduled = false;
		if (!Quitting()) {
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
			if (qEnvironmentVariable("TDESKTOP_SOCKET_ERROR_REGRESSION")
				== u"persistent"_q) {
				return socketError(QLocalSocket::SocketAccessError);
			}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
			_localSocket.connectToServer(_localServerName);
		}
	});
}

void Sandbox::showSingleInstanceError(
	QString diagnostic, [[maybe_unused]] QString refusedOperation) {
	if (_singleInstanceLock && _singleInstanceLock->isAcquired()) {
		_singleInstanceLock->release();
	}
	if (cQuit()) {
		return Quit();
	}
	const auto window = new NotStartedWindow();
	const auto text = window->findChild<QTextEdit *>();
	if (text && !diagnostic.isEmpty()) {
		text->setPlainText(diagnostic + u"\n\n"_q + text->toPlainText());
	}
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
	if (qEnvironmentVariableIsSet("TDESKTOP_PROFILE_LOCK_REFUSAL_REGRESSION")) {
		const auto path
			= qEnvironmentVariable("TDESKTOP_MAC_PROFILE_TEST_HOME")
			  + u"/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/refused-profile.lock"_q;
		const auto expected
			= u"Protected profile lock path refused (%1): %2"_q.arg(
				refusedOperation, path);
		const auto visible
			= !refusedOperation.isEmpty() && text
			  && text->toPlainText().startsWith(expected + u"\n\n"_q);
		const auto encoded = path.toUtf8();
		const auto operation = refusedOperation.toUtf8();
		fprintf(stderr,
				"Profile lock refusal regression: failure_window=%d "
				"window_contains_error=%d operation=%s path=%s\n",
				window->isVisible() ? 1 : 0, visible ? 1 : 0,
				operation.constData(), encoded.constData());
		QTimer::singleShot(0, this, [] { QCoreApplication::exit(1); });
	}
	if (qEnvironmentVariableIsSet("TDESKTOP_PROFILE_LOCK_REGRESSION")) {
		const auto reason = _singleInstanceLock->errorString();
		const auto encoded = reason.toUtf8();
		fprintf(stderr,
				"Profile lock regression: failure_window=%d "
				"window_contains_error=%d reason=%s\n",
				window->isVisible() ? 1 : 0,
				(text && text->toPlainText().contains(reason)) ? 1 : 0,
				encoded.constData());
		QTimer::singleShot(0, this, [] { QCoreApplication::exit(0); });
	}
	if (qEnvironmentVariable("TDESKTOP_SOCKET_ERROR_REGRESSION")
		== u"persistent"_q) {
		fprintf(stderr, "Socket error regression: failure_window=%d\n",
				window->isVisible() ? 1 : 0);
		QTimer::singleShot(0, this, [] { QCoreApplication::exit(0); });
	}
	if (qEnvironmentVariable("TDESKTOP_SOCKET_ERROR_REGRESSION")
		== u"ipc-collision"_q) {
		auto entry = QFile(_localServerName);
		const auto entryPreserved
			= entry.open(QIODevice::ReadOnly)
			  && (entry.readAll()
				  == QByteArray(kSocketCollisionFixtureContents));
		const auto pathVisible
			= text && text->toPlainText().contains(_localServerName);
		const auto visible = window->isVisible();
		const auto encoded = QFile::encodeName(_localServerName);
		fprintf(stderr,
				"Socket error regression: collision_window=%d "
				"path_visible=%d entry_preserved=%d path=%s\n",
				visible ? 1 : 0, pathVisible ? 1 : 0, entryPreserved ? 1 : 0,
				encoded.constData());
		const auto passed = visible && pathVisible && entryPreserved;
		QTimer::singleShot(
			0, this, [passed] { QCoreApplication::exit(passed ? 0 : 1); });
	}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
}

void Sandbox::newInstanceConnected() {
	DEBUG_LOG(("Sandbox Info: new local socket connected"));
	for (auto client = _localServer.nextPendingConnection(); client; client = _localServer.nextPendingConnection()) {
		if (CurrentLaunchState() == LaunchState::QuitProcessed
			&& !_closingApplication) {
			client->setParent(this);
			continue;
		}
		_localClients.push_back(LocalClient(client, QByteArray()));
		connect(
			client,
			&QLocalSocket::readyRead,
			[=] { readClients(); });
		connect(
			client,
			&QLocalSocket::disconnected,
			[=] { removeClients(); });
	}
}

void Sandbox::readClients() {
	if (Quitting()) {
		for (auto &client : _localClients) {
			const auto received = client.first->readAll();
			client.second.append(received);
#if defined(Q_OS_MAC) && defined(TDESKTOP_LIFECYCLE_REGRESSION)
			if (!received.isEmpty()
				&& qEnvironmentVariableIsSet(
					"TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE")) {
				fprintf(stderr,
						"Emoji cache shutdown regression: peer_waiting=1\n");
			}
#endif // Q_OS_MAC && TDESKTOP_LIFECYCLE_REGRESSION
			if (client.second == QByteArray("CMD:quit;")) {
				client.first->write(
					u"RES:%1_0;"_q.arg(QApplication::applicationPid())
						.toLatin1());
				client.second.clear();
			} else if (client.second.size() > 9) {
				client.second.clear();
			}
		}
		return;
	}
	// This method can be called before Application is constructed.
	QList<QUrl> startUrls;
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e; ++i) {
		i->second.append(i->first->readAll());
		if (i->second.size()) {
			bool activationRequired = false;
			bool quitAfterResponse = false;
			QString cmds(QString::fromLatin1(i->second));
			int32 from = 0, l = cmds.length();
			for (int32 to = cmds.indexOf(QChar(';'), from); to >= from; to = (from < l) ? cmds.indexOf(QChar(';'), from) : -1) {
				auto cmd = base::StringViewMid(cmds, from, to - from);
				if (cmd.startsWith(u"CMD:"_q)) {
					const auto processId = QApplication::applicationPid();
					const auto command = cmds.mid(from + 4, to - from - 4);
					auto windowId = uint64(0);
					if (command == u"quit"_q) {
						quitAfterResponse = true;
					} else {
						windowId = execExternal(command);
					}
					const auto response = u"RES:%1_%2;"_q.arg(processId).arg(windowId).toLatin1();
					i->first->write(response.data(), response.size());
				} else if (cmd.startsWith(u"XDG_ACTIVATION_TOKEN:"_q)) {
					qputenv("XDG_ACTIVATION_TOKEN", QByteArray::fromBase64(cmds.mid(from + 21, to - from - 21).toLatin1()));
				} else if (cmd.startsWith(u"OPEN:"_q)) {
					const auto raw
						= cmds.mid(from + 5, to - from - 5).mid(0, 8192);
					const auto url = QUrl(raw);
#ifdef Q_OS_MAC
					if ((url.scheme() == u"file"_q && !url.isLocalFile()
						 && MacProtectedPath::IsActive())
						|| (url.isLocalFile()
							&& !MacProtectedPath::CheckExternalPath(
								MacProtectedPath::Operation::Open,
								url.toLocalFile(), "sandbox.open"))
						|| (url.scheme().isEmpty()
							&& !MacProtectedPath::CheckExternalPath(
								MacProtectedPath::Operation::Open, raw,
								"sandbox.open"))) {
						from = to + 1;
						continue;
					}
#endif // Q_OS_MAC
					startUrls.append(url);
					if (!activationRequired) {
						activationRequired = StartUrlRequiresActivate(startUrls.back().toString());
					}
				} else if (cmd.startsWith(u"CTRL:"_q)) {
					const auto payload = HandleExternalControl(
						cmds.mid(from + 5, to - from - 5));
					const auto response = QByteArray("DATA:")
						+ payload.toBase64()
						+ ';';
					i->first->write(response);
				} else {
					LOG(("Sandbox Error: unknown command %1 passed in local socket").arg(cmd.toString()));
				}
				from = to + 1;
			}
			if (from > 0) {
				i->second = i->second.mid(from);
			}
			const auto processId = QApplication::applicationPid();
			const auto windowId = activationRequired
				? execExternal("show")
				: 0;
			const auto response = u"RES:%1_%2;"_q.arg(processId).arg(windowId).toLatin1();
			i->first->write(response.data(), response.size());
			if (quitAfterResponse) {
				const auto client = i->first;
				const auto quitWhenFlushed = [=] {
					if (!client->bytesToWrite()) {
						Quit();
					}
				};
				if (client->bytesToWrite()) {
					connect(client, &QLocalSocket::bytesWritten, this,
							[=](qint64) { quitWhenFlushed(); });
					connect(client, &QLocalSocket::disconnected, this,
							[=] { Quit(); });
				} else {
					QMetaObject::invokeMethod(this, quitWhenFlushed,
											  Qt::QueuedConnection);
				}
			}
		}
	}
	cRefStartUrls() << base::take(startUrls);
	if (_application) {
		_application->checkStartUrls();
	}
}

void Sandbox::removeClients() {
	DEBUG_LOG(("Sandbox Info: remove clients slot called, clients %1"
		).arg(_localClients.size()));
	for (auto i = _localClients.begin(), e = _localClients.end(); i != e;) {
		if (i->first->state() != QLocalSocket::ConnectedState) {
			DEBUG_LOG(("Sandbox Info: removing client"));
			i = _localClients.erase(i);
			e = _localClients.end();
		} else {
			++i;
		}
	}
}

void Sandbox::checkForQuit() {
	if (Quitting()) {
		quit();
	}
}

void Sandbox::refreshGlobalProxy() {
	const auto proxy = !Core::IsAppLaunched()
		? _sandboxProxy
		: Core::App().settings().proxy().isEnabled()
		? Core::App().settings().proxy().selected()
		: MTP::ProxyData();
	if (proxy.type == MTP::ProxyData::Type::Socks5
		|| proxy.type == MTP::ProxyData::Type::Http) {
		QNetworkProxy::setApplicationProxy(
			MTP::ToNetworkProxy(MTP::ToDirectIpProxy(proxy)));
	} else if (!Core::IsAppLaunched()
		|| Core::App().settings().proxy().isSystem()) {
		QNetworkProxyFactory::setUseSystemConfiguration(true);
	} else {
		QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
	}
}

void Sandbox::checkForEmptyLoopNestingLevel() {
	// _loopNestingLevel == _eventNestingLevel means that we had a
	// native event in a nesting loop that didn't get a notify() call
	// after. That means we already have exited the nesting loop and
	// there must not be any postponed calls with that nesting level.
	if (_loopNestingLevel == _eventNestingLevel) {
		Assert(_postponedCallsDeferred
			|| _postponedCalls.empty()
			|| _postponedCalls.back().loopNestingLevel < _loopNestingLevel);
		Assert(!_previousLoopNestingLevels.empty());

		_loopNestingLevel = _previousLoopNestingLevels.back();
		_previousLoopNestingLevels.pop_back();
	}
}

void Sandbox::postponeCall(FnMut<void()> &&callable) {
	Expects(callable != nullptr);
	Expects(_eventNestingLevel >= _loopNestingLevel);

	checkForEmptyLoopNestingLevel();
	_postponedCalls.push_back({
		_loopNestingLevel,
		std::move(callable)
	});
}

void Sandbox::incrementEventNestingLevel() {
	++_eventNestingLevel;
}

void Sandbox::decrementEventNestingLevel() {
	Expects(_eventNestingLevel >= _loopNestingLevel);

	if (_eventNestingLevel == _loopNestingLevel) {
		_loopNestingLevel = _previousLoopNestingLevels.back();
		_previousLoopNestingLevels.pop_back();
	}
	const auto processTillLevel = _eventNestingLevel - 1;
	processPostponedCalls(processTillLevel);
	checkForEmptyLoopNestingLevel();
	_eventNestingLevel = processTillLevel;

	Ensures(_eventNestingLevel >= _loopNestingLevel);
}

void Sandbox::registerEnterFromEventLoop() {
	Expects(_eventNestingLevel >= _loopNestingLevel);

	if (_eventNestingLevel > _loopNestingLevel) {
		_previousLoopNestingLevels.push_back(_loopNestingLevel);
		_loopNestingLevel = _eventNestingLevel;
	}
}

bool Sandbox::notify(QObject *receiver, QEvent *e) {
	if (QThread::currentThreadId() != _mainThreadId) {
		return QApplication::notify(receiver, e);
	}

	const auto wrap = createEventNestingLevel();
	if (e->type() == QEvent::UpdateRequest) {
		const auto weak = QPointer<QObject>(receiver);
		_widgetUpdateRequests.fire({});
		if (!weak) {
			return true;
		}
	}
	return QApplication::notify(receiver, e);
}

void Sandbox::processPostponedCalls(int level) {
	if (_postponedCallsDeferred) {
		return;
	}
	while (!_postponedCalls.empty()) {
		auto &last = _postponedCalls.back();
		if (last.loopNestingLevel != level) {
			break;
		}
		auto taken = std::move(last);
		_postponedCalls.pop_back();
		taken.callable();
	}
}

void Sandbox::drainPostponedCalls() {
	Expects(QThread::currentThreadId() == _mainThreadId);

	if (!cTestAgent()) {
		return;
	}
	const auto wasDeferred = std::exchange(_postponedCallsDeferred, true);
	const auto guard = gsl::finally([&] {
		_postponedCallsDeferred = wasDeferred;
	});
	while (!_postponedCalls.empty()) {
		auto taken = std::move(_postponedCalls.back());
		_postponedCalls.pop_back();
		taken.callable();
	}
}

void Sandbox::setPostponedCallsDeferred(bool deferred) {
	Expects(QThread::currentThreadId() == _mainThreadId);

	if (!cTestAgent()) {
		return;
	}
	_postponedCallsDeferred = deferred;
}

bool Sandbox::nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		native_event_filter_result *result) {
	registerEnterFromEventLoop();
	return false;
}

rpl::producer<> Sandbox::widgetUpdateRequests() const {
	return _widgetUpdateRequests.events();
}

MTP::ProxyData Sandbox::sandboxProxy() const {
	return _sandboxProxy;
}

void Sandbox::closeApplication() {
	if (CurrentLaunchState() == LaunchState::QuitProcessed) {
		return;
	}
	SetLaunchState(LaunchState::QuitProcessed);
	if (!_application) {
		finishCloseApplication();
		return;
	}
	_closingApplication = true;
	Ui::Emoji::StopAndDrain([this] {
		QMetaObject::invokeMethod(
			this,
			[this] {
				customEnterFromEventLoop([&] { finishCloseApplication(); });
			},
			Qt::QueuedConnection);
	});
}

void Sandbox::finishCloseApplication() {
	_application = nullptr;

	// Keep peers waiting until this Sandbox releases its instance lock and
	// listener. QObject destroys these children after the Sandbox members.
	for (const auto &localClient : base::take(_localClients)) {
		QObject::disconnect(localClient.first, nullptr, nullptr, nullptr);
		localClient.first->setParent(this);
	}

	_localSocket.close();

	_updateChecker = nullptr;
	_closingApplication = false;
	_shutdownLoop.quit();
}

uint64 Sandbox::execExternal(const QString &cmd) {
	DEBUG_LOG(("Sandbox Info: executing external command '%1'").arg(cmd));
	if (cmd == "show") {
		if (Core::IsAppLaunched() && Core::App().activePrimaryWindow()) {
			const auto window = Core::App().activePrimaryWindow();
			window->activate();
			return Platform::ActivationWindowId(window->widget());
		} else if (const auto window = PreLaunchWindow::instance()) {
			window->activate();
			return Platform::ActivationWindowId(window);
		}
	} else if (cmd == "quit") {
		Quit();
	}
	return 0;
}

} // namespace Core

namespace crl {

rpl::producer<> on_main_update_requests() {
	return Core::Sandbox::Instance().widgetUpdateRequests();
}

} // namespace crl
