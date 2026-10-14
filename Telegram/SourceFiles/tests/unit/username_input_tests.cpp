/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "styles/style_widgets.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_font.h"
#include "ui/widgets/fields/special_fields.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QResource>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>

#include <rpl/event_stream.h>

#include <memory>
#include <utility>
#include <vector>

namespace crl {

rpl::producer<> on_main_update_requests() {
	static auto requests = rpl::event_stream<>();
	return requests.events();
}

} // namespace crl

namespace {

class UsernameInputIntegration final : public Ui::Integration {
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

class UsernameInputFixture final {
  public:
	UsernameInputFixture() {
		Ui::Integration::Set(&Integration());
		style::internal::StartFonts();
		style::StartManager(style::kScaleDefault);
		_animations = std::make_unique<Ui::Animations::Manager>();
	}

	~UsernameInputFixture() { _animations.reset(); }

	void processPostponedCalls() {
		Integration().processPostponedCalls();
		QCoreApplication::processEvents();
	}

  private:
	static UsernameInputIntegration &Integration() {
		static auto result = UsernameInputIntegration();
		return result;
	}

	std::unique_ptr<Ui::Animations::Manager> _animations;
};

} // namespace

TEST_CASE(UsernameInputStandaloneEventDispatch) {
	const auto application
		= qobject_cast<QApplication *>(QCoreApplication::instance());
	CHECK(application != nullptr);
	if (!application) {
		return;
	}

#ifdef Q_OS_LINUX
	CHECK_EQ(QApplication::platformName(), u"offscreen"_q);
#endif // Q_OS_LINUX

	const auto shortcuts
		= QKeySequence::keyBindings(QKeySequence::DeleteStartOfWord);
	CHECK(!shortcuts.isEmpty());
	if (shortcuts.isEmpty()) {
		return;
	}

	auto fixture = UsernameInputFixture();
	const auto fontResource = QResource(u":/gui/fonts/OpenSans-Regular.ttf"_q);
	CHECK(fontResource.isValid());
	if (!fontResource.isValid()) {
		return;
	}

	const auto hasFont = bool(st::defaultInputField.style.font);
	CHECK(hasFont);
	if (!hasFont) {
		return;
	}

	const auto key = shortcuts.front()[0];
	for (auto lifetime = 0; lifetime != 2; ++lifetime) {
		{
			auto input = Ui::UsernameInput(nullptr, st::defaultInputField,
										   rpl::single(u"@username"_q),
										   u"operator"_q, QString());
			input.setReadOnly(false);
			CHECK(!input.isVisible());
			input.setSelection(int(input.text().size()) - 2, 2);

			auto event = QKeyEvent(QEvent::KeyPress, key.key(),
								   key.keyboardModifiers());
			QCoreApplication::sendEvent(&input, &event);

			CHECK_EQ(input.text(), u"operat"_q);
			CHECK_EQ(input.getLastText(), u"operat"_q);
			input.setText(QString());
		}
		fixture.processPostponedCalls();
	}
}
