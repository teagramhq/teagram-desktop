/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"
#include "core/shutdown_loop.h"

#include "ui/emoji_config.h"
#include "ui/integration.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_font.h"

#include <QtCore/QFile>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#include <atomic>
#include <memory>
#include <utility>

namespace {

class CacheShutdownIntegration final : public Ui::Integration {
  public:
	explicit CacheShutdownIntegration(QString folder = QString())
		: _folder(std::move(folder)) {}
	void postponeCall(FnMut<void()> &&) override {}
	void registerLeaveSubscription(not_null<QWidget *>) override {}
	void unregisterLeaveSubscription(not_null<QWidget *>) override {}
	QString openglCheckFilePath() override { return {}; }
	QString angleBackendFilePath() override { return {}; }
	void touchCounterIncrement() override {}
	int touchCounterNow() override { return 0; }

	QString emojiCacheFolder() override {
		if (_armed.exchange(false)) {
			_entered.release();
			if (!_release.tryAcquire(1, 5000)) {
				timedOut = true;
			}
		}
		return _enabled ? _folder : QString();
	}

	void arm() {
		_enabled = true;
		_armed = true;
	}
	bool waitUntilEntered() { return _entered.tryAcquire(1, 5000); }
	void release() { _release.release(); }

	std::atomic_bool timedOut = false;

  private:
	const QString _folder;
	std::atomic_bool _enabled = false;
	std::atomic_bool _armed = false;
	QSemaphore _entered;
	QSemaphore _release;
};

} // namespace

TEST_CASE(EmojiCacheShutdownDrainsActualCleanupWithoutBlockingGui) {
	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	const auto stale = directory.filePath(u"obsolete"_q);
	auto file = QFile(stale);
	CHECK(file.open(QIODevice::WriteOnly));
	file.close();

	auto integration
		= std::make_unique<CacheShutdownIntegration>(directory.path());
	Ui::Integration::Set(integration.get());
	style::SetDevicePixelRatio(1);
	style::internal::StartFonts();
	style::StartManager(style::kScaleDefault);
	Ui::Emoji::Init();

	integration->arm();
	Ui::Emoji::ClearIrrelevantCache();
	const auto entered = integration->waitUntilEntered();
	auto drained = std::atomic_bool(false);
	Ui::Emoji::StopAndDrain([&] { drained = true; });
	const auto completedWhileBlocked = drained.load();

	auto loop = Core::ShutdownLoop();
	auto heartbeatBeforeRelease = false;
	auto interrupted = false;
	auto timedOut = false;
	auto poll = QTimer();
	QTimer::singleShot(0, &poll, [&] {
		interrupted = true;
		QTimer::singleShot(0, &poll, [&] {
			heartbeatBeforeRelease = !drained.load();
			integration->release();
		});
		loop.quit();
	});
	QObject::connect(&poll, &QTimer::timeout, [&] {
		if (drained.load() && !QFile::exists(stale)) {
			loop.quit();
		}
	});
	auto deadline = QTimer();
	deadline.setSingleShot(true);
	QObject::connect(&deadline, &QTimer::timeout, [&] {
		timedOut = true;
		loop.quit();
	});
	poll.start(1);
	deadline.start(5000);
	loop.run(
		[&] { return !timedOut && (!drained.load() || QFile::exists(stale)); });
	poll.stop();
	deadline.stop();

	CHECK(entered);
	CHECK(!completedWhileBlocked);
	CHECK(interrupted);
	CHECK(heartbeatBeforeRelease);
	CHECK(!integration->timedOut.load());
	CHECK(!timedOut);
	CHECK(drained.load());
	CHECK(!QFile::exists(stale));

	Ui::Emoji::Clear();
	static auto fallback = CacheShutdownIntegration();
	Ui::Integration::Set(&fallback);
	integration.reset();
	style::StopManager();
}
