/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_turnstile_panel.h"

#include "boxes/abstract_box.h"
#include "intro/intro_turnstile.h"
#include "ui/chat/attach/attach_bot_webview.h"
#include "ui/layers/box_content.h"
#include "webview/webview_embed.h"
#if defined Q_OS_WIN
#include "webview/platform/win/webview_windows_edge_chromium.h"
#endif // Q_OS_WIN

#include "lang/lang_keys.h"
#include "settings.h"

#include "styles/style_layers.h"
#include "styles/style_window.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QWidget>

#include <memory>
#include <utility>

namespace Intro::details {
namespace {

[[nodiscard]] Webview::Available TurnstileAvailability() {
	auto result = Webview::Availability();
#if defined Q_OS_WIN
	if (result.error == Webview::Available::Error::None
		&& !Webview::EdgeChromium::Supported()) {
		result.error = Webview::Available::Error::NoWebview2;
	}
#endif
	return result;
}

[[nodiscard]] QString UnavailableText(const Webview::Available &available) {
	auto result = tr::lng_bot_no_webview(tr::now);
	result += u"\n\n"_q;
	result += Ui::BotWebView::ErrorText(available).text;
	return result;
}

[[nodiscard]] Webview::StorageId TurnstileStorageId() {
	static const auto result = [] {
		return Webview::StorageId{
			.path = cWorkingDir() + u"tdata/webview-turnstile"_q,
			.token = QByteArray::fromStdString(Webview::GenerateStorageToken()),
		};
	}();
	return result;
}

enum class TurnstileFailureReason {
	WidgetDestroyed,
	NavigationFailed,
	MainFrameUrlChanged,
	ChallengeError,
	WindowClosed,
	Cancelled,
};

[[nodiscard]] QString
TurnstileFailureReasonText(TurnstileFailureReason reason) {
	switch (reason) {
	case TurnstileFailureReason::WidgetDestroyed:
		return u"web view widget destroyed"_q;
	case TurnstileFailureReason::NavigationFailed:
		return u"engine navigation failed"_q;
	case TurnstileFailureReason::MainFrameUrlChanged:
		return u"Linux main-frame URL changed"_q;
	case TurnstileFailureReason::ChallengeError:
		return u"challenge reported an error"_q;
	case TurnstileFailureReason::WindowClosed:
		return u"web view window closed"_q;
	case TurnstileFailureReason::Cancelled:
		return u"challenge cancelled"_q;
	}
	Unexpected("Unknown Turnstile failure reason.");
}

class TurnstileBox final : public Ui::BoxContent {
  public:
	TurnstileBox(QWidget *, TurnstileChallenge challenge,
				 Fn<void(QString)> token, Fn<void()> failed);
	~TurnstileBox() override;

	void closeSilently();
	[[nodiscard]] QString error() const;

  protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;

  private:
	[[nodiscard]] bool navigationStarted(const QString &url, bool newWindow);
	void navigationDone(bool success);
	void navigationStateUpdated(Webview::NavigationHistoryState state);
	void messageReceived(Webview::Message message);
	void widgetDestroyed();
	void externalWindowClosed();
	void finishToken(QString token);
	void deferWindowDestruction(bool close, Fn<void()> callback);
	void finishFailed(TurnstileFailureReason reason, bool close = true,
					  bool fromEngineCallback = false);

	const TurnstileChallenge _challenge;
	const QString _initialUrl;
	Fn<void(QString)> _token;
	Fn<void()> _failed;
	std::unique_ptr<Webview::Window> _window;
	QString _mainFrameUrl;
	QString _error;
	TurnstileLinuxNavigationGuard _linuxNavigationGuard;
	TurnstileBoxState _state = TurnstileBoxState::Loading;
	bool _initialNavigationPending = true;
};

TurnstileBox::TurnstileBox(QWidget *, TurnstileChallenge challenge,
						   Fn<void(QString)> token, Fn<void()> failed)
	: _challenge(std::move(challenge)), _initialUrl(TurnstileUrl(_challenge)),
	  _token(std::move(token)), _failed(std::move(failed)) {}

TurnstileBox::~TurnstileBox() {
	_state = TurnstileBoxState::Finished;
	_window.reset();
}

void TurnstileBox::closeSilently() {
	if (_state == TurnstileBoxState::Finished) {
		return;
	}
	_state = TurnstileBoxState::Finished;
	_linuxNavigationGuard.finish();
	_window.reset();
	closeBox();
}

QString TurnstileBox::error() const { return _error; }

void TurnstileBox::prepare() {
	setNoContentMargin(true);
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	setDimensions(st::boxWideWidth, st::boxMaxListHeight);

	boxClosing()
		| rpl::on_next(
			[=] {
				if (_state != TurnstileBoxState::Finished) {
					finishFailed(TurnstileFailureReason::Cancelled, false);
				}
			},
			lifetime());

	const auto available = TurnstileAvailability();
	if (available.error != Webview::Available::Error::None) {
		LOG(("Intro Turnstile: web view unavailable, error %1")
				.arg(int(available.error)));
		_error = UnavailableText(available);
		return;
	}

	_window = std::make_unique<Webview::Window>(
		this, Webview::WindowConfig{
				  .opaqueBg = st::windowBg->c,
				  .storageId = TurnstileStorageId(),
			  });
	const auto raw = _window.get();
	if (!raw->widget()) {
		LOG(("Intro Turnstile: web view widget creation failed"));
		_window.reset();
		_error = tr::lng_bot_webview_failed(tr::now);
		return;
	}
	QObject::connect(raw->widget(), &QObject::destroyed, this,
					 [=] { widgetDestroyed(); });

	raw->setNavigationStartHandler([=](const QString &url, bool newWindow) {
		return navigationStarted(url, newWindow);
	});
	raw->setNavigationDoneHandler(
		[=](bool success) { navigationDone(success); });
	raw->navigationHistoryState()
		| rpl::on_next(
			[=](Webview::NavigationHistoryState state) {
				navigationStateUpdated(std::move(state));
			},
			lifetime());
	raw->setMessageHandler(
		[=](Webview::Message message) { messageReceived(std::move(message)); });
	raw->setExternalWindowCloseHandler([=] { externalWindowClosed(); });

	raw->init(R"(
window.TelegramWebviewProxy = {
postEvent: function(eventType, eventData) {
	if (window.external && window.external.invoke) {
		window.external.invoke(JSON.stringify([eventType, eventData]));
	}
}
};)");
	if (!_window) {
		LOG(("Intro Turnstile: web view initialization failed"));
		_error = tr::lng_bot_webview_failed(tr::now);
		return;
	}

	raw->widget()->setGeometry(rect());
	raw->widget()->show();
	raw->navigate(_initialUrl);
}

void TurnstileBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	if (_window && _window->widget()) {
		_window->widget()->setGeometry(rect());
	}
}

bool TurnstileBox::navigationStarted(const QString &url, bool newWindow) {
	if (_state == TurnstileBoxState::Finished) {
		return false;
	}
	const auto decision = CheckTurnstileNavigation(
		url, _initialUrl, _initialNavigationPending, newWindow,
#if !defined Q_OS_WIN && !defined Q_OS_MAC
		true
#else
		false
#endif
	);
	_state = NextStateOnNavigationStart(_state, decision);
	if (decision.initialNavigation) {
		_initialNavigationPending = false;
	}
	if (decision.rejection) {
		LOG(("Intro Turnstile navigation rejected: %1")
				.arg(TurnstileNavigationRejectionText(*decision.rejection)));
	}
	return !decision.rejection.has_value();
}

void TurnstileBox::navigationDone(bool success) {
	if (_state == TurnstileBoxState::Finished) {
		return;
	} else if (success) {
		_state = TurnstileBoxState::Pending;
		_linuxNavigationGuard.initialLoadCompleted();
	} else {
		finishFailed(TurnstileFailureReason::NavigationFailed, true, true);
	}
}

void TurnstileBox::navigationStateUpdated(
	Webview::NavigationHistoryState state) {
	if (_state == TurnstileBoxState::Finished) {
		return;
	}
	const auto url = QString::fromStdString(state.url);
#if !defined Q_OS_WIN && !defined Q_OS_MAC
	if (_linuxNavigationGuard.mainFrameUrlUpdated(url, _initialUrl)) {
		finishFailed(TurnstileFailureReason::MainFrameUrlChanged, true, true);
		return;
	}
#endif // !Q_OS_WIN && !Q_OS_MAC
	_mainFrameUrl = url;
}

void TurnstileBox::messageReceived(Webview::Message message) {
	if (_state == TurnstileBoxState::Finished) {
		return;
	}
	auto parsed = ParseTurnstileMessage(message.text, _challenge.action(),
										_state != TurnstileBoxState::Finished,
										_state == TurnstileBoxState::Pending,
										_mainFrameUrl);
	if (const auto rejection
		= std::get_if<TurnstileMessageRejection>(&parsed)) {
		LOG(("Intro Turnstile message rejected: %1 (%2 bytes)")
				.arg(TurnstileMessageRejectionText(*rejection))
				.arg(quint64(message.text.size())));
		return;
	}
	auto &parsedMessage = std::get<TurnstileMessage>(parsed);
	if (parsedMessage.type == TurnstileMessageType::Error) {
		finishFailed(TurnstileFailureReason::ChallengeError, true, true);
		return;
	}
	finishToken(std::move(parsedMessage.token));
}

void TurnstileBox::widgetDestroyed() {
	if (_state != TurnstileBoxState::Finished) {
		finishFailed(TurnstileFailureReason::WidgetDestroyed, true, true);
	}
}

void TurnstileBox::externalWindowClosed() {
	if (_state != TurnstileBoxState::Finished) {
		finishFailed(TurnstileFailureReason::WindowClosed, true, true);
	}
}

void TurnstileBox::finishToken(QString token) {
	_state = TurnstileBoxState::Finished;
	_linuxNavigationGuard.finish();
	auto callback = std::move(_token);
	deferWindowDestruction(true, [callback = std::move(callback),
								  token = std::move(token)]() mutable {
		if (callback) {
			callback(std::move(token));
		}
	});
}

void TurnstileBox::deferWindowDestruction(bool close, Fn<void()> callback) {
	auto window = std::shared_ptr<Webview::Window>(_window.release());
	const auto weak = QPointer<TurnstileBox>(this);
	DeferTurnstileCallback(
		[weak, window = std::move(window), close]() mutable {
			window.reset();
			if (weak && close) {
				weak->closeBox();
			}
		},
		std::move(callback));
}

void TurnstileBox::finishFailed(TurnstileFailureReason reason, bool close,
								bool fromEngineCallback) {
	if (_state == TurnstileBoxState::Finished) {
		return;
	}
	LOG(("Intro Turnstile failed: %1").arg(TurnstileFailureReasonText(reason)));
	_state = TurnstileBoxState::Finished;
	_linuxNavigationGuard.finish();
	auto callback = std::move(_failed);
	if (fromEngineCallback) {
		deferWindowDestruction(close, std::move(callback));
		return;
	}
	_window.reset();
	if (close) {
		closeBox();
	}
	if (callback) {
		callback();
	}
}

} // namespace

TurnstilePanelResult OpenTurnstilePanel(TurnstileChallenge challenge,
										Fn<void(QString)> token,
										Fn<void()> failed) {
	const auto available = TurnstileAvailability();
	if (available.error != Webview::Available::Error::None) {
		LOG(("Intro Turnstile: web view unavailable, error %1")
				.arg(int(available.error)));
		return {.error = UnavailableText(available)};
	}

	auto weak = Ui::show(Box<TurnstileBox>(
		std::move(challenge), std::move(token), std::move(failed)));
	if (weak && !weak->error().isEmpty()) {
		const auto error = weak->error();
		weak->closeSilently();
		return {.error = error};
	}
	return {
		.closeSilently =
			[weak] {
				if (weak) {
					weak->closeSilently();
				}
			},
	};
}

} // namespace Intro::details
