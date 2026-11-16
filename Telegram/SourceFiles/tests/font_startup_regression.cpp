/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/font_startup_regression.h"

#include "base/debug_log.h"
#ifdef Q_OS_MAC
#include "ui/emoji_config.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#endif // Q_OS_MAC

#include <QtCore/QElapsedTimer>
#include <QtGui/QFont>
#include <QtGui/QRawFont>
#include <QtGui/QTextLayout>

namespace {

QString FontFamilyForCharacter(QString text, uint character) {
	auto layout = QTextLayout(text, QFont(u"Open Sans"_q, 16));
	layout.beginLayout();
	auto line = layout.createLine();
	if (!line.isValid()) {
		return QString();
	}
	line.setLineWidth(1024);
	layout.endLayout();
	for (const auto &run : layout.glyphRuns()) {
		const auto &font = run.rawFont();
		if (font.supportsCharacter(character)) {
			return font.familyName();
		}
	}
	return QString();
}

} // namespace

int RunFontFallbackRegression() {
	auto timer = QElapsedTimer();
	timer.start();
	const auto emoji = FontFamilyForCharacter(u"😀"_q, 0x1F600);
	const auto arabic = FontFamilyForCharacter(u"ش"_q, 0x0634);
	const auto wallNs = timer.nsecsElapsed();
	const auto emojiFamily = emoji.startsWith(u"."_q) ? emoji.mid(1) : emoji;
	const auto emojiFallback = (emojiFamily == u"Apple Color Emoji"_q)
							   || (emojiFamily == u"Apple Color Emoji UI"_q);
	const auto passed = emojiFallback && (arabic == u"Vazirmatn UI NL"_q);
	LOG(("Font fallback regression: emoji=%1 arabic=%2 wall_ns=%3 "
		 "result=%4")
			.arg(emoji, arabic)
			.arg(wallNs)
			.arg(passed ? u"PASS"_q : u"FAIL"_q));
	return passed ? 0 : 1;
}

#ifdef Q_OS_MAC
namespace {

struct EmojiShutdownState {
	std::atomic_bool enabled = false;
	std::atomic_bool claimed = false;
	std::atomic_bool blocked = false;
	std::atomic_bool destroying = false;
	std::atomic_bool workerReturned = false;
	std::atomic_bool workerSawAlive = false;
	std::atomic_bool guiResponsive = false;
	std::atomic_bool timedOut = false;
	std::mutex mutex;
	std::condition_variable wake;
	bool release = false;
};

EmojiShutdownState &ShutdownState() {
	static auto state = EmojiShutdownState();
	return state;
}

} // namespace

void PauseEmojiCacheForShutdownRegression() {
	auto &state = ShutdownState();
	if (!state.enabled.load()
		|| QThread::currentThread() == QCoreApplication::instance()->thread()
		|| state.claimed.exchange(true)) {
		return;
	}
	state.blocked = true;
	{
		auto lock = std::unique_lock(state.mutex);
		if (!state.wake.wait_for(lock, std::chrono::seconds(10),
								 [&] { return state.release; })) {
			state.timedOut = true;
		}
	}
	state.workerSawAlive = !state.destroying.load();
	state.workerReturned = true;
}

void RunEmojiCacheShutdownRegression(std::function<void(bool)> done) {
	auto &state = ShutdownState();
	state.enabled = true;
	Ui::Emoji::ClearIrrelevantCache();
	const auto timer = new QTimer(QCoreApplication::instance());
	const auto elapsed = std::make_shared<QElapsedTimer>();
	elapsed->start();
	QObject::connect(timer, &QTimer::timeout, [timer, elapsed, done] {
		auto &state = ShutdownState();
		if (!state.blocked && elapsed->elapsed() < 5000) {
			return;
		}
		timer->stop();
		timer->deleteLater();
		if (!state.blocked) {
			done(false);
			return;
		}
		std::fprintf(stderr,
					 "Emoji cache shutdown regression: worker_blocked=1\n");
		QTimer::singleShot(0, QCoreApplication::instance(), [] {
			auto &state = ShutdownState();
			state.guiResponsive
				= !state.destroying.load() && !state.workerReturned.load();
			{
				const auto lock = std::lock_guard(state.mutex);
				state.release = true;
			}
			state.wake.notify_all();
		});
		done(true);
	});
	timer->start(1);
}

void EmojiCacheShutdownApplicationDestroying() {
	auto &state = ShutdownState();
	if (!state.enabled.load()) {
		return;
	}
	state.destroying = true;
	std::fprintf(
		stderr,
		"Emoji cache shutdown regression: gui_responsive=%d "
		"integration_alive=%d worker_returned=%d wait_timed_out=%d\n",
		state.guiResponsive.load() ? 1 : 0, state.workerSawAlive.load() ? 1 : 0,
		state.workerReturned.load() ? 1 : 0, state.timedOut.load() ? 1 : 0);
}
#endif // Q_OS_MAC
