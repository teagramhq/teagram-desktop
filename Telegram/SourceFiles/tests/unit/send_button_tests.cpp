/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "lang/lang_keys.h"
#include "lottie/lottie_common.h"
#include "styles/style_chat_helpers.h"
#include "ui/controls/send_button.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/main_queue_processor.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_font.h"
#include "ui/ui_utility.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QResource>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>

#include <rpl/never.h>
#include <rpl/range.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Lang {

QString Id() { return u"en"_q; }

rpl::producer<> Updated() { return rpl::never<>(); }

QString GetNonDefaultValue(const QByteArray &) { return QString(); }

namespace details {

QString Current(ushort key) { return GetOriginalValue(key); }

rpl::producer<QString> Value(ushort key) { return rpl::single(Current(key)); }

bool IsNonDefaultPlural(ushort) { return false; }

} // namespace details
} // namespace Lang

namespace {

const auto kVoiceToVideo = u":/animations/chat/voice_to_video.tgs"_q;
constexpr auto kWaitLimit = std::chrono::seconds(5);

struct ReadGate {
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool released = false;
	bool timedOut = false;
};

std::atomic<std::shared_ptr<ReadGate>> ActiveReadGate;

class SendButtonIntegration final : public Ui::Integration {
  public:
	void postponeCall(FnMut<void()> &&callable) override {
		_postponed.push_back(std::move(callable));
	}
	void registerLeaveSubscription(not_null<QWidget *>) override {}
	void unregisterLeaveSubscription(not_null<QWidget *>) override {}
	QString emojiCacheFolder() override { return QString(); }
	QString openglCheckFilePath() override { return QString(); }
	QString angleBackendFilePath() override { return QString(); }
	void touchCounterIncrement() override {}
	int touchCounterNow() override { return 0; }
	void processPostponedCalls() {
		while (!_postponed.empty()) {
			auto callable = std::move(_postponed.back());
			_postponed.pop_back();
			callable();
		}
	}

  private:
	std::vector<FnMut<void()>> _postponed;
};

[[nodiscard]] bool HasVisiblePixel(const QImage &image) {
	for (auto y = 0; y != image.height(); ++y) {
		for (auto x = 0; x != image.width(); ++x) {
			if (qAlpha(image.pixel(x, y)) != 0) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] bool GateTimedOut(const std::shared_ptr<ReadGate> &gate) {
	const auto lock = std::lock_guard(gate->mutex);
	return gate->timedOut;
}

} // namespace

extern "C" QByteArray
__real__ZN6Lottie11ReadContentERK10QByteArrayRK7QString(const QByteArray &data,
														const QString &path);

extern "C" QByteArray
__wrap__ZN6Lottie11ReadContentERK10QByteArrayRK7QString(const QByteArray &data,
														const QString &path) {
	if (const auto gate = ActiveReadGate.load();
		gate && path == kVoiceToVideo) {
		auto lock = std::unique_lock(gate->mutex);
		gate->entered = true;
		gate->changed.notify_all();
		if (!gate->changed.wait_for(lock, kWaitLimit,
									[&] { return gate->released; })) {
			gate->timedOut = true;
		}
	}
	return __real__ZN6Lottie11ReadContentERK10QByteArrayRK7QString(data, path);
}

TEST_CASE(SendButtonDoesNotWaitForPreparingVoiceIcon) {
	CHECK(qobject_cast<QApplication *>(QCoreApplication::instance())
		  != nullptr);
	if (!qobject_cast<QApplication *>(QCoreApplication::instance())) {
		return;
	}
	CHECK(QResource(kVoiceToVideo).isValid());
	if (!QResource(kVoiceToVideo).isValid()) {
		return;
	}

	static auto integration = SendButtonIntegration();
	Ui::Integration::Set(&integration);
	static auto mainQueue = Ui::MainQueueProcessor();
	style::internal::StartFonts();
	style::StartManager(style::kScaleDefault);
	auto animations = Ui::Animations::Manager();

	const auto gate = std::make_shared<ReadGate>();
	ActiveReadGate.store(gate);
	{
		auto button = Ui::SendButton(nullptr, st::historySend);
		button.setState({.type = Ui::SendButton::Type::Record});

		auto entered = false;
		{
			auto lock = std::unique_lock(gate->mutex);
			entered = gate->changed.wait_for(lock, kWaitLimit,
											 [&] { return gate->entered; });
		}
		CHECK(entered);
		CHECK(!GateTimedOut(gate));

		if (entered && !GateTimedOut(gate)) {
			button.finishAnimating();
			const auto pending = Ui::GrabWidgetToImage(&button);
			CHECK(!pending.isNull());
			CHECK(!GateTimedOut(gate));
			if (!GateTimedOut(gate)) {
				CHECK(pending == Ui::GrabWidgetToImage(&button));
				button.setState({.type = Ui::SendButton::Type::Round});
				CHECK(!GateTimedOut(gate));
			}
		}

		{
			const auto lock = std::lock_guard(gate->mutex);
			gate->released = true;
		}
		gate->changed.notify_all();
		ActiveReadGate.store(nullptr);

		auto visible = false;
		const auto until = std::chrono::steady_clock::now() + kWaitLimit;
		while (!visible && std::chrono::steady_clock::now() < until) {
			QCoreApplication::processEvents();
			integration.processPostponedCalls();
			visible = HasVisiblePixel(Ui::GrabWidgetToImage(&button));
			if (!visible) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		CHECK(visible);
	}
	integration.processPostponedCalls();
}
