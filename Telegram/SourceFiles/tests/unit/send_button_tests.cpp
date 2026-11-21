/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "lang/lang_keys.h"
#include "lottie/lottie_common.h"
#include "lottie/lottie_icon.h"
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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <pthread.h>
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
constexpr auto kUiBudget = std::chrono::milliseconds(50);

struct ReadGate {
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool released = false;
	bool timedOut = false;
};

std::atomic<std::shared_ptr<ReadGate>> ActiveReadGate;

struct ThreadStartProbe {
	int attempts = 0;
	int rejected = 0;
};

thread_local ThreadStartProbe *ActiveThreadStartProbe = nullptr;

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

SendButtonIntegration &SetupSendButtonTests() {
	static auto integration = SendButtonIntegration();
	Ui::Integration::Set(&integration);
	static auto mainQueue = Ui::MainQueueProcessor();
	style::internal::StartFonts();
	style::StartManager(style::kScaleDefault);
	return integration;
}

void WarmSendButton() {
	auto icon = Lottie::MakeIcon({
		.path = kVoiceToVideo,
		.sizeOverride = st::historySend.recordSize,
		.colorizeUsingAlpha = true,
	});
	CHECK(icon->framesCount() > 1);
	auto button = Ui::SendButton(nullptr, st::historySend);
	CHECK(HasVisiblePixel(Ui::GrabWidgetToImage(&button)));
}

} // namespace

// Only the test thread can arm this failure, after the Lottie executor is
// already running. That makes the rejected creation the button's waiter,
// while thread starts in the rest of the application keep using pthreads
// normally. The next waiter start is allowed so recovery can be observed.
extern "C" int pthread_create(pthread_t *thread,
							  const pthread_attr_t *attributes,
							  void *(*start)(void *), void *argument) noexcept {
	if (const auto probe = ActiveThreadStartProbe) {
		++probe->attempts;
		if (!probe->rejected) {
			++probe->rejected;
			return EAGAIN;
		}
	}
	using Function = int (*)(pthread_t *, const pthread_attr_t *,
							 void *(*)(void *), void *);
	static const auto original
		= reinterpret_cast<Function>(dlsym(RTLD_NEXT, "pthread_create"));
	if (!original) {
		std::abort();
	}
	return original(thread, attributes, start, argument);
}

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

	auto &integration = SetupSendButtonTests();
	auto animations = Ui::Animations::Manager();
	WarmSendButton();

	const auto gate = std::make_shared<ReadGate>();
	ActiveReadGate.store(gate);
	{
		auto button = Ui::SendButton(nullptr, st::historySend);
		const auto stateStart = std::chrono::steady_clock::now();
		button.setState({.type = Ui::SendButton::Type::Record});
		const auto stateElapsed = std::chrono::steady_clock::now() - stateStart;
		CHECK(stateElapsed < kUiBudget);

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
			const auto grabStart = std::chrono::steady_clock::now();
			const auto pending = Ui::GrabWidgetToImage(&button);
			const auto grabElapsed
				= std::chrono::steady_clock::now() - grabStart;
			CHECK(!pending.isNull());
			CHECK(grabElapsed < kUiBudget);
			CHECK(!GateTimedOut(gate));
			if (!GateTimedOut(gate)) {
				CHECK(pending == Ui::GrabWidgetToImage(&button));
				const auto roundStart = std::chrono::steady_clock::now();
				button.setState({.type = Ui::SendButton::Type::Round});
				CHECK(std::chrono::steady_clock::now() - roundStart
					  < kUiBudget);
				CHECK(!GateTimedOut(gate));
				const auto roundGrabStart = std::chrono::steady_clock::now();
				const auto pendingRound = Ui::GrabWidgetToImage(&button);
				CHECK(!pendingRound.isNull());
				CHECK(std::chrono::steady_clock::now() - roundGrabStart
					  < kUiBudget);
				CHECK(!GateTimedOut(gate));
				button.setState({.type = Ui::SendButton::Type::Record});
				button.setState({.type = Ui::SendButton::Type::Round});
				CHECK(!GateTimedOut(gate));
			}
		}

		const auto originalMultiplier = anim::SlowMultiplier();
		anim::SetSlowMultiplier(10);
		const auto restoreMultiplier = gsl::finally(
			[=] { anim::SetSlowMultiplier(originalMultiplier); });
		{
			const auto lock = std::lock_guard(gate->mutex);
			gate->released = true;
		}
		gate->changed.notify_all();
		ActiveReadGate.store(nullptr);

		auto visible = false;
		auto animated = false;
		auto firstFrame = QImage();
		const auto until = std::chrono::steady_clock::now() + kWaitLimit;
		while (!animated && std::chrono::steady_clock::now() < until) {
			QCoreApplication::processEvents();
			integration.processPostponedCalls();
			const auto frame = Ui::GrabWidgetToImage(&button);
			if (HasVisiblePixel(frame)) {
				if (!visible) {
					firstFrame = frame;
				} else {
					animated = (frame != firstFrame);
				}
				visible = true;
			}
			if (!animated) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		CHECK(visible);
		CHECK(animated);
		CHECK(button.type() == Ui::SendButton::Type::Round);
		for (const auto type : {
				 Ui::SendButton::Type::Send,
				 Ui::SendButton::Type::Schedule,
				 Ui::SendButton::Type::Save,
				 Ui::SendButton::Type::Record,
			 }) {
			button.setState({.type = type});
			button.finishAnimating();
			CHECK(button.type() == type);
			CHECK(HasVisiblePixel(Ui::GrabWidgetToImage(&button)));
		}
	}

	const auto destroyedGate = std::make_shared<ReadGate>();
	ActiveReadGate.store(destroyedGate);
	{
		auto button
			= std::make_unique<Ui::SendButton>(nullptr, st::historySend);
		button->setState({.type = Ui::SendButton::Type::Record});
		auto entered = false;
		{
			auto lock = std::unique_lock(destroyedGate->mutex);
			entered = destroyedGate->changed.wait_for(
				lock, kWaitLimit, [&] { return destroyedGate->entered; });
		}
		CHECK(entered);
		CHECK(!GateTimedOut(destroyedGate));
		button->finishAnimating();
		button->setState({.type = Ui::SendButton::Type::Send});
		CHECK(!GateTimedOut(destroyedGate));
		button.reset();
		CHECK(!GateTimedOut(destroyedGate));
	}
	{
		const auto lock = std::lock_guard(destroyedGate->mutex);
		destroyedGate->released = true;
	}
	destroyedGate->changed.notify_all();
	ActiveReadGate.store(nullptr);
	QCoreApplication::processEvents();
	integration.processPostponedCalls();
}

TEST_CASE(SendButtonRetriesAfterWaiterThreadStartFails) {
	CHECK(qobject_cast<QApplication *>(QCoreApplication::instance())
		  != nullptr);
	if (!qobject_cast<QApplication *>(QCoreApplication::instance())) {
		return;
	}
	auto &integration = SetupSendButtonTests();
	auto animations = Ui::Animations::Manager();
	WarmSendButton();

	auto probe = ThreadStartProbe();
	ActiveThreadStartProbe = &probe;
	const auto clearProbe
		= gsl::finally([] { ActiveThreadStartProbe = nullptr; });
	auto button = Ui::SendButton(nullptr, st::historySend);
	button.setState({.type = Ui::SendButton::Type::Record});
	button.finishAnimating();
	CHECK_EQ(probe.rejected, 1);

	const auto until = std::chrono::steady_clock::now() + kWaitLimit;
	while (probe.attempts < 2 && std::chrono::steady_clock::now() < until) {
		QCoreApplication::processEvents();
		integration.processPostponedCalls();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto retried = (probe.attempts >= 2);
	CHECK(retried);
	ActiveThreadStartProbe = nullptr;

	auto visible = false;
	const auto readyUntil = std::chrono::steady_clock::now() + kWaitLimit;
	while (retried && !visible
		   && std::chrono::steady_clock::now() < readyUntil) {
		QCoreApplication::processEvents();
		integration.processPostponedCalls();
		visible = HasVisiblePixel(Ui::GrabWidgetToImage(&button));
		if (!visible) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(visible);
}
