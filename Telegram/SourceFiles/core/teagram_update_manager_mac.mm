/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/teagram_update_manager.h"

#if defined(Q_OS_MAC) && defined(TDESKTOP_TEAGRAM)

#include "core/application.h"
#include "calls/calls_instance.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "storage/localstorage.h"
#include "settings.h"
#include "window/window_controller.h"

#include <QtCore/QOperatingSystemVersion>
#include <QtCore/QVersionNumber>

namespace Core {
namespace {

constexpr auto kUpdateCheckInterval = crl::time(24 * 60 * 60 * 1000);
constexpr auto kSafetyCheckInterval = crl::time(1000);

class InstallJob final : public TeagramUpdateInstallJob {
  public:
	void cancel() override { _installer.cancel(); }

	TeagramUpdateInstallResult
	install(const TeagramUpdateCandidate &candidate,
			TeagramUpdateInstallProgressHandler progress,
			TeagramUpdateBeforeRelaunch beforeRelaunch) override {
		return _installer.install(candidate, std::move(progress),
								  std::move(beforeRelaunch));
	}

  private:
	TeagramUpdateInstaller _installer;
};

class Environment final : public TeagramUpdateManagerEnvironment {
  public:
	quint64 runningBuild() const override { return kTeagramUpdateBuild; }

	TeagramUpdateChannel compiledChannel() const override {
		return (kTeagramUpdateChannel == "main") ? TeagramUpdateChannel::Main
												 : TeagramUpdateChannel::Dev;
	}

	TeagramUpdateChannel readChannel() override {
		return ReadTeagramUpdateChannel(Core::App().settings(),
										compiledChannel());
	}

	void writeChannel(TeagramUpdateChannel channel) override {
		WriteTeagramUpdateChannel(Core::App().settings(), channel);
		Local::writeSettings();
	}

	quint64 readHighWater() override {
		auto &settings = Core::App().settings();
		const auto pending = settings.readPref<QByteArray>(
			kTeagramUpdatePendingMainSwitchPreference);
		if (!pending.isEmpty()) {
			if (CommitTeagramUpdatePendingMainSwitch(settings, runningBuild(),
													 compiledChannel())) {
				settings.writePref<bool>(kTeagramUpdateToastPreference, true);
			}
			Local::writeSettings();
			Local::sync();
		}
		return ReadTeagramUpdateHighWater(settings);
	}

	void writeCommittedHighWater(quint64 build) override {
		WriteTeagramUpdateHighWater(Core::App().settings(), build);
		Core::App().settings().writePref<bool>(kTeagramUpdateToastPreference,
											   true);
		Local::writeSettings();
	}

	bool readUpdatedToastPending() override {
		return Core::App().settings().readPref<bool>(
			kTeagramUpdateToastPreference, false);
	}

	bool showUpdatedToast(quint64 build) override {
		const auto window = Core::App().activePrimaryWindow();
		if (!window) {
			return false;
		}
		window->showToast(tr::lng_teagram_update_toast(tr::now, lt_build,
													   QString::number(build)));
		Core::App().settings().writePref<bool>(kTeagramUpdateToastPreference,
											   false);
		Local::writeSettings();
		return true;
	}

	bool automatic() const override { return cAutoUpdate(); }

	void writeAutomatic(bool enabled) override {
		cSetAutoUpdate(enabled);
		Local::writeSettings();
	}

	TeagramUpdateActiveWork activeWork() const override {
		auto result = TeagramUpdateActiveWork();
		result.call
			= Core::App().calls().inCall() || Core::App().calls().inGroupCall();
		if (const auto capture = Media::Capture::instance()) {
			result.recording = capture->started();
		}
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto session = entry.account->maybeSession();
			if (session && session->uploadsInProgress()) {
				result.upload = true;
				break;
			}
		}
		return result;
	}

	crl::time now() const override { return crl::now(); }

	void restart() override { Core::Restart(); }

	TeagramUpdateInstallLocation installLocation() const override {
		return CheckTeagramUpdateInstallLocation();
	}

	bool supportsMinimumOs(const QString &minimum) const override {
		if (minimum.isEmpty()) {
			return true;
		}
		const auto required = QVersionNumber::fromString(minimum);
		if (required.isNull()) {
			return false;
		}
		const auto currentOs = QOperatingSystemVersion::current();
		const auto current
			= QVersionNumber(currentOs.majorVersion(), currentOs.minorVersion(),
							 currentOs.microVersion());
		return QVersionNumber::compare(current, required) >= 0;
	}

	bool previousBundleAvailable() const override {
		return HasPreviousTeagramUpdateBundle();
	}

	void showPreviousBundle() const override {
		ShowPreviousTeagramUpdateBundleInFinder();
	}

	bool deferOlderBuildUntilShutdown(
		std::shared_ptr<TeagramUpdateCandidate> candidate,
		const QString &name) override {
		const auto build = candidate->metadata().build();
		const auto failure = tr::lng_teagram_update_shutdown_failed(
			tr::now, lt_build, QString::number(build));
		if (!ScheduleOlderTeagramUpdateAfterShutdown(
				std::move(candidate), cWorkingDir(), name, failure)) {
			return false;
		}
		Core::App().settings().writePref<QByteArray>(
			kTeagramUpdatePendingMainSwitchPreference,
			QByteArray::number(build));
		Local::writeSettings();
		return true;
	}

	std::shared_ptr<TeagramUpdateDownloader> createDownloader() override {
		return std::make_shared<TeagramUpdateDownloader>();
	}

	std::shared_ptr<TeagramUpdateInstallJob> createInstaller() override {
		return std::make_shared<InstallJob>();
	}

	crl::time checkInterval() const override { return kUpdateCheckInterval; }

	crl::time safetyInterval() const override { return kSafetyCheckInterval; }
};

} // namespace

TeagramUpdateManager &TeagramUpdateManager::Instance() {
	static const auto result
		= new TeagramUpdateManager(std::make_unique<Environment>());
	return *result;
}

} // namespace Core

#endif // Q_OS_MAC && TDESKTOP_TEAGRAM
