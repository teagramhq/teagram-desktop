/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "intro/intro_step.h"
#include "intro/intro_server_discovery.h"
#include "mtproto/mtproto_server_discovery.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

class QNetworkAccessManager;
class QNetworkReply;
class QHostInfo;
class QTimer;

namespace Ui {
class FlatLabel;
class InputField;
class LinkButton;
class RoundButton;
class ScrollArea;
class VerticalLayout;
} // namespace Ui

namespace Intro {
namespace details {

class ServerWidgetDiscovery;

inline void SubmitSavedServer(bool blocked, bool hasSession,
							  bool browserOnlyServerRefused,
							  Fn<void()> showBrowserOnlyError,
							  Fn<void()> continueToUsername) {
	if (blocked || hasSession) {
		return;
	}
	if (browserOnlyServerRefused) {
		showBrowserOnlyError();
		return;
	}
	continueToUsername();
}

struct ServerStepState {
	bool blocked = false;
	bool hasSession = false;
	bool hasCustomServer = false;
	bool browserOnlyServerRefused = false;
};

[[nodiscard]] inline bool ServerStepIsReadOnly(const ServerStepState &state) {
	return state.blocked
		   || (state.hasCustomServer
			   && (state.hasSession || !state.browserOnlyServerRefused));
}

[[nodiscard]] inline bool
CanCommitServerSelection(const ServerStepState &state) {
	return !state.blocked && !state.hasSession
		   && (!state.hasCustomServer || state.browserOnlyServerRefused);
}

class ServerWidget final : public Step {
public:
	ServerWidget(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);

	bool hasBack() const override {
		return true;
	}

	[[nodiscard]] int nextButtonTop() const override;

	void setInnerFocus() override;
	void activate() override;
	void cancelled() override;
	void submit() override;

	[[nodiscard]] rpl::producer<QString> nextButtonText() const override;
	[[nodiscard]] QWidget *firstTabWidget() const override;
	[[nodiscard]] QWidget *lastTabWidget() const override;
	[[nodiscard]] QWidget *nextButtonFocusWidget() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void layoutContent();
	void refreshMode();
	[[nodiscard]] ServerStepState serverStepState() const;
	void setupSelection();
	void setupBound();
	void switchToBound();
	void switchToSelection();
	void selectionChanged();
	void submitSelection();
	void beginPublicDiscovery();
	void beginLocalDiscovery();
	void startLocalDiscovery(const QList<QHostAddress> &addresses);
	void discoveryTimeout();
	void discoveryFinished(MTP::ServerDiscoveryResult result);
	void resolvePublicEndpoint(MTP::ServerDiscoveryResult result);
	void publicEndpointResolved(
		MTP::ServerDiscoveryResult result,
		const QHostInfo &info);
	void resolvePublicWebSocketEndpoint(MTP::ServerDiscoveryResult result);
	void publicWebSocketEndpointResolved(MTP::ServerDiscoveryResult result,
										 const QHostInfo &info);
	void discoveryFailed(bool connectionFailure);
	void resetAfterDiscoveryFailure(bool connectionFailure);
	void
	resetAfterDiscoveryFailure(ServerDiscoveryFailurePresentation presentation);
	void cancelDiscovery();
	void showStatus(const QString &text, bool error);
	void clearStatus();
	void announceStatus();
	void commitBinding(const MTP::ServerDiscoveryResult &result);
	[[nodiscard]] QString selectionError(
		MTP::ServerSelectionStatus status) const;
	[[nodiscard]] bool readOnly() const;

	object_ptr<Ui::ScrollArea> _scroll;
	Ui::VerticalLayout *_content = nullptr;
	Ui::FlatLabel *_addressLabel = nullptr;
	Ui::InputField *_address = nullptr;
	Ui::FlatLabel *_status = nullptr;
	Ui::RoundButton *_continue = nullptr;
	Ui::FlatLabel *_savedAddressLabel = nullptr;
	Ui::FlatLabel *_savedAddress = nullptr;
	Ui::FlatLabel *_savedStatus = nullptr;
	Ui::RoundButton *_savedContinue = nullptr;
	Ui::LinkButton *_addAccount = nullptr;

	QNetworkAccessManager *_network = nullptr;
	QNetworkReply *_reply = nullptr;
	QByteArray _publicResponse;
	int _hostLookupId = -1;
	ServerWidgetDiscovery *_localDiscovery = nullptr;
	ServerDiscoveryFlow _discoveryFlow;
	QTimer *_deadline = nullptr;
	QByteArray _localNonce;
	std::optional<MTP::ServerDiscoveryAttempt> _discoveryAttempt;

	MTP::ServerSelectionCheck _selection;
	bool _readOnly = false;
	bool _connecting = false;
	bool _suppressChanges = false;
	uint64 _attempt = 0;
};

} // namespace details
} // namespace Intro
