/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_turnstile_panel.h"

#include "config.h"
#include "intro/intro_turnstile.h"
#include "ui/chat/attach/attach_bot_webview.h"
#include "ui/boxes/abstract_box.h"
#include "ui/layers/box_content.h"
#include "lang/lang_keys.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"
#include "webview/webview_embed.h"
#if defined Q_OS_WIN
#include "webview/platform/win/webview_windows_edge_chromium.h"
#endif // Q_OS_WIN

#include <QtCore/QObject>
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

[[nodiscard]] QString UnavailableText(
		const Webview::Available &available) {
	auto result = tr::lng_bot_no_webview(tr::now);
	result += u"\n\n";
	result += Ui::BotWebView::ErrorText(available).text;
	return result;
}

[[nodiscard]] Webview::StorageId TurnstileStorageId() {
	static const auto result = [] {
		return Webview::StorageId{
			.path = cWorkingDir() + u"tdata/webview-turnstile"_q,
			.token = QByteArray::fromStdString(
				Webview::GenerateStorageToken()),
		};
	}();
	return result;
}

class TurnstileBox final : public Ui::BoxContent {
public:
	TurnstileBox(
		QWidget*,
		TurnstileChallenge challenge,
		Fn<void(QString)> token,
		Fn<void()> failed)
	: _challenge(std::move(challenge))
	, _token(std::move(token))
	, _failed(std::move(failed)) {
	}

	~TurnstileBox() override {
		_finished = true;
		_pending = false;
		_window.reset();
	}

	void closeSilently() {
		if (_finished) {
			return;
		}
		_finished = true;
		_pending = false;
		_window.reset();
		closeBox();
	}

	[[nodiscard]] QString error() const {
		return _error;
	}

protected:
	void prepare() override {
		setNoContentMargin(true);
		addButton(tr::lng_cancel(), [=] {
			closeBox();
		});
		setDimensions(st::boxWideWidth, st::boxMaxListHeight);

		boxClosing() | rpl::on_next([=] {
			if (!_finished) {
				finishFailed(false);
			}
		}, lifetime());

		const auto available = TurnstileAvailability();
		if (available.error != Webview::Available::Error::None) {
			_error = UnavailableText(available);
			return;
		}

		_window = std::make_unique<Webview::Window>(
			this,
			Webview::WindowConfig{
				.opaqueBg = st::windowBg->c,
				.storageId = TurnstileStorageId(),
			});
		const auto raw = _window.get();
		if (!raw->widget()) {
			_window.reset();
			_error = tr::lng_bot_webview_failed(tr::now);
			return;
		}
		QObject::connect(
			raw->widget(),
			&QObject::destroyed,
			this,
			[=] {
				if (!_finished) {
					finishFailed();
				}
			});

		const auto initialUrl = TurnstileUrl(_challenge);
		raw->setNavigationStartHandler([=](
				const QString &url,
				bool newWindow) {
			const auto decision = CheckTurnstileNavigation(
				url,
				initialUrl,
				_initialNavigationPending,
				newWindow,
#if !defined Q_OS_WIN && !defined Q_OS_MAC
				true
#else
				false
#endif
			);
			if (decision.initialNavigation) {
				_initialNavigationPending = false;
			}
			if (!decision.allowed) {
				LOG(("Intro Turnstile navigation rejected: %1")
					.arg(TurnstileNavigationRejectionText(
						decision.rejection)));
			}
			return decision.allowed;
		});
		raw->setNavigationDoneHandler([=](bool success) {
			_navigationDone = success;
			if (!success && !_finished) {
				finishFailed();
			}
		});
		raw->navigationHistoryState(
		) | rpl::on_next([=](Webview::NavigationHistoryState state) {
			_mainFrameUrl = QString::fromStdString(state.url);
		}, lifetime());
		raw->setMessageHandler([=](Webview::Message message) {
			if (_finished) {
				return;
			}
			const auto parsed = ParseTurnstileMessage(
				message.text,
				_challenge.action,
				_pending,
				_navigationDone,
				_mainFrameUrl);
			if (!parsed.message) {
				if (parsed.rejection != TurnstileMessageRejection::None) {
					LOG(("Intro Turnstile message rejected: %1 (%2 bytes)")
						.arg(TurnstileMessageRejectionText(
							parsed.rejection))
						.arg(quint64(message.text.size())));
				}
				return;
			} else if (parsed.message->type == TurnstileMessageType::Error) {
				finishFailed();
				return;
			}

			auto token = std::move(parsed.message->token);
			auto callback = std::move(_token);
			_finished = true;
			_pending = false;
			_window.reset();
			closeBox();
			if (callback) {
				callback(std::move(token));
			}
		});
		raw->setExternalWindowCloseHandler([=] {
			if (!_finished) {
				finishFailed();
			}
		});

		raw->init(R"(
window.TelegramWebviewProxy = {
postEvent: function(eventType, eventData) {
	if (window.external && window.external.invoke) {
		window.external.invoke(JSON.stringify([eventType, eventData]));
	}
}
};)");
		if (!_window) {
			_error = tr::lng_bot_webview_failed(tr::now);
			return;
		}

		raw->widget()->setGeometry(rect());
		raw->widget()->show();
		raw->navigate(initialUrl);
	}

	void resizeEvent(QResizeEvent *e) override {
		BoxContent::resizeEvent(e);
		if (_window && _window->widget()) {
			_window->widget()->setGeometry(rect());
		}
	}

private:
	void finishFailed(bool close = true) {
		if (_finished) {
			return;
		}
		_finished = true;
		_pending = false;
		auto callback = std::move(_failed);
		_window.reset();
		if (close) {
			closeBox();
		}
		if (callback) {
			callback();
		}
	}

	const TurnstileChallenge _challenge;
	Fn<void(QString)> _token;
	Fn<void()> _failed;
	std::unique_ptr<Webview::Window> _window;
	QString _mainFrameUrl;
	QString _error;
	bool _initialNavigationPending = true;
	bool _navigationDone = false;
	bool _pending = true;
	bool _finished = false;
};

} // namespace

TurnstilePanelResult OpenTurnstilePanel(
		TurnstileChallenge challenge,
		Fn<void(QString)> token,
		Fn<void()> failed) {
	const auto available = TurnstileAvailability();
	if (available.error != Webview::Available::Error::None) {
		return { .error = UnavailableText(available) };
	}

	auto weak = Ui::show(Ui::Box<TurnstileBox>(
		std::move(challenge),
		std::move(token),
		std::move(failed)));
	if (weak && !weak->error().isEmpty()) {
		const auto error = weak->error();
		weak->closeSilently();
		return { .error = error };
	}
	return {
		.closeSilently = [weak] {
			if (weak) {
				weak->closeSilently();
			}
		},
	};
}

} // namespace Intro::details
