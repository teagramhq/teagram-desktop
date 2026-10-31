/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_invite_friends.h"
#include "settings/sections/settings_invite_state.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_box.h"
#include "boxes/peers/edit_peer_invite_links.h"
#include "info/bot/starref/info_bot_starref_common.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/type_utils.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_calls.h"
#include "styles/style_boxes.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QLocale>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QShortcut>
#include <algorithm>
#include <optional>

namespace Settings {
namespace {

using InviteId = int64;

struct InviteData {
	InviteId id = 0;
	QString state;
	TimeId createdAt = 0;
	TimeId expiresAt = 0;
	bool showDuration = true;
};

[[nodiscard]] bool UserInvitesEnabled(not_null<Main::Session *> session) {
	return session->appConfig().get<bool>(u"user_invites_enabled"_q, false);
}

[[nodiscard]] QString ServerName(not_null<Main::Session *> session) {
	const auto &server = session->account().mtp().dcOptions().customServer();
	const auto host = QString::fromStdString(
		server.hostname.empty() ? server.ip : server.hostname);
	if (host.isEmpty()) {
		return u"this server"_q;
	} else if (server.hostname.empty() && server.ipv6) {
		return u"["_q + host + u"]"_q;
	}
	return host;
}

[[nodiscard]] QString InviteDate(TimeId timestamp) {
	const auto date = base::unixtime::parse(timestamp);
	return tr::lng_mediaview_date_time(
		tr::now, lt_date, langDayOfMonth(date.date()), lt_time,
		QLocale().toString(date.time(), QLocale::ShortFormat));
}

[[nodiscard]] QString InviteDuration(TimeId seconds) {
	if (seconds >= 86400) {
		return tr::lng_days(tr::now, lt_count, seconds / 86400);
	} else if (seconds >= 3600) {
		return tr::lng_hours(tr::now, lt_count, seconds / 3600);
	}
	return tr::lng_minutes(tr::now, lt_count, std::max(1, int(seconds / 60)));
}

[[nodiscard]] QString StateTitle(const InviteData &data) {
	if (data.state == u"used"_q) {
		return tr::lng_teagram_invite_used(tr::now);
	} else if (data.state == u"expired"_q) {
		return tr::lng_teagram_invite_expired(tr::now);
	} else if (data.state == u"revoked"_q) {
		return tr::lng_teagram_invite_revoked(tr::now);
	}
	return tr::lng_teagram_invite_pending(tr::now);
}

[[nodiscard]] QString StateStatus(const InviteData &data) {
	const auto created = tr::lng_settings_passkeys_created(
		tr::now, lt_date, InviteDate(data.createdAt));
	if (data.state != u"pending"_q || !data.showDuration) {
		return created;
	}
	const auto remaining
		= std::max<TimeId>(0, data.expiresAt - base::unixtime::now());
	return created + u" "_q + Ui::kQBullet + u" "_q
		   + tr::lng_teagram_invite_left(tr::now, lt_duration,
										 InviteDuration(remaining));
}

class InviteFriends;

class InviteRow final : public PeerListRow {
  public:
	InviteRow(not_null<InviteFriends *> delegate, InviteData data);

	[[nodiscard]] InviteData data() const { return _data; }

	QString generateName() override;
	QString generateShortName() override;
	PaintRoundImageCallback
	generatePaintUserpicCallback(bool forceRound) override;
	QSize rightActionSize() const override;
	QMargins rightActionMargins() const override;
	void rightActionPaint(Painter &p, int x, int y, int outerWidth,
						  bool selected, bool actionSelected) override;
	void setShowDuration(bool showDuration);

  private:
	const not_null<InviteFriends *> _delegate;
	InviteData _data;
};

class InviteListDelegate final : public PeerListContentDelegateShow {
  public:
	using PeerListContentDelegateShow::PeerListContentDelegateShow;

	void
	peerListSetDescription(object_ptr<Ui::FlatLabel> description) override {
		PeerListContentDelegate::peerListSetDescription(std::move(description));
	}
};

class InviteFriends final : public PeerListController,
							public Section<InviteFriends> {
  public:
	InviteFriends(QWidget *parent,
				  not_null<Window::SessionController *> controller);
	~InviteFriends();

	[[nodiscard]] rpl::producer<QString> title() override;

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow *> row) override;
	void rowRightActionClicked(not_null<PeerListRow *> row) override;
	base::unique_qptr<Ui::PopupMenu>
	rowContextMenu(QWidget *parent, not_null<PeerListRow *> row) override;

	void rowUpdateRow(not_null<InviteRow *> row);
	void rowPaintIcon(QPainter &p, int x, int y, int size,
					  const InviteData &data);

  private:
	void setupContent();
	[[nodiscard]] details::InviteCreateConditions createConditions() const;
	void updateAvailability();
	void updateSummary();
	void updateMessage();
	void loadInvites(Fn<void()> done = nullptr);
	void createInvite();
	void revokeInvite(InviteData data);
	void showInviteLink(QString link, TimeId expiresAt);
	void showCreateError(const MTP::Error &error);
	void setRows(const MTPDteagram_userInvites &data);

	const not_null<Window::SessionController *> _controller;
	const not_null<Main::Session *> _session;
	Ui::SettingsButton *_createButton = nullptr;
	Ui::SettingsButton *_retryButton = nullptr;
	Ui::FlatLabel *_quotaLabel = nullptr;
	Ui::FlatLabel *_messageLabel = nullptr;
	PeerListContent *_list = nullptr;
	mtpRequestId _listRequestId = 0;
	mtpRequestId _createRequestId = 0;
	mtpRequestId _revokeRequestId = 0;
	details::InviteCreateRequest _createRequest;
	InviteId _revokeId = 0;
	int64 _quota = 0;
	int64 _weeklyCount = 0;
	std::optional<TimeId> _nextAvailableAt;
	bool _available = false;
	bool _loaded = false;
	bool _loading = false;
	bool _listFailed = false;
	bool _unavailableShown = false;
	bool _serverDisabled = false;
	bool _createBlocked = false;
	QString _registrationMode;
	QString _createError;
	rpl::variable<bool> _creatingValue = false;
};

InviteRow::InviteRow(not_null<InviteFriends *> delegate, InviteData data)
	: PeerListRow(UniqueRowIdFromString(QString::number(data.id))),
	  _delegate(delegate), _data(std::move(data)) {
	setCustomStatus(StateStatus(_data));
}

QString InviteRow::generateName() { return StateTitle(_data); }

QString InviteRow::generateShortName() { return generateName(); }

PaintRoundImageCallback InviteRow::generatePaintUserpicCallback(bool) {
	return [=](Painter &p, int x, int y, int outerWidth, int size) {
		_delegate->rowPaintIcon(p, x, y, size, _data);
	};
}

QSize InviteRow::rightActionSize() const {
	return (_data.state == u"pending"_q) ? st::inviteLinkThreeDotsIcon.size()
										 : QSize();
}

QMargins InviteRow::rightActionMargins() const {
	if (_data.state != u"pending"_q) {
		return QMargins();
	}
	return QMargins(
		0, (st::inviteLinkList.item.height - rightActionSize().height()) / 2,
		st::inviteLinkThreeDotsSkip, 0);
}

void InviteRow::rightActionPaint(Painter &p, int x, int y, int outerWidth,
								 bool selected, bool actionSelected) {
	if (_data.state != u"pending"_q) {
		return;
	}
	(selected || actionSelected ? st::inviteLinkThreeDotsIconOver
								: st::inviteLinkThreeDotsIcon)
		.paint(p, x, y, outerWidth);
}

void InviteRow::setShowDuration(bool showDuration) {
	if (_data.showDuration == showDuration) {
		return;
	}
	_data.showDuration = showDuration;
	setCustomStatus(StateStatus(_data));
	_delegate->rowUpdateRow(this);
}

InviteFriends::InviteFriends(QWidget *parent,
							 not_null<Window::SessionController *> controller)
	: PeerListController(), Section(parent, controller),
	  _controller(controller), _session(&controller->session()) {
	setupContent();
}

InviteFriends::~InviteFriends() {
	session().api().request(_listRequestId).cancel();
	session().api().request(_createRequestId).cancel();
	session().api().request(_revokeRequestId).cancel();
}

rpl::producer<QString> InviteFriends::title() {
	return tr::lng_teagram_invite_title();
}

Main::Session &InviteFriends::session() const { return *_session; }

details::InviteCreateConditions InviteFriends::createConditions() const {
	return {
		.available = _available,
		.loaded = _loaded,
		.listLoading = bool(_listRequestId),
		.creating = _createRequest.active(),
		.serverDisabled = _serverDisabled,
		.createBlocked = _createBlocked,
		.quota = _quota,
		.weeklyCount = _weeklyCount,
	};
}

void InviteFriends::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	Ui::AddDivider(content);

	const auto header = content->add(object_ptr<Ui::VerticalLayout>(content));
	header->add(Info::BotStarRef::CreateLinkHeaderIcon(header, _session),
				st::settingsFilterIconPadding, style::al_top);
	header
		->add(object_ptr<Ui::FlatLabel>(
				  header,
				  tr::lng_teagram_invite_about(
					  lt_server, rpl::single(tr::marked(ServerName(_session))),
					  tr::marked),
				  st::settingsFilterDividerLabel),
			  st::settingsFilterDividerLabelPadding, style::al_top)
		->setTryMakeSimilarLines(true);

	Ui::AddSkip(content);

	auto create = MakeCreateLinkButton(
		content, _creatingValue.value() | rpl::map([](bool creating) {
					 return creating ? tr::lng_teagram_invite_creating(tr::now)
									 : tr::lng_teagram_invite_create(tr::now);
				 }));
	_createButton = create.data();
	_createButton->setClickedCallback([=] { createInvite(); });
	content->add(std::move(create));
	Ui::AddSkip(content);

	const auto quota = Ui::AddDividerText(content, rpl::single(QString()));
	_quotaLabel = quota.get();
	_quotaLabel->hide();

	Ui::AddSubsectionTitle(content, tr::lng_teagram_invite_list());

	auto message
		= object_ptr<Ui::FlatLabel>(content, QString(), st::dialogsEmptyLabel);
	_messageLabel = message.data();
	_messageLabel->setTryMakeSimilarLines(true);
	content->add(std::move(message), st::peerAppearanceCoverLabelMargin);

	auto retry = object_ptr<Ui::SettingsButton>(
		content, tr::lng_teagram_invite_try_again(),
		st::settingsButtonLightNoIcon);
	_retryButton = retry.data();
	_retryButton->setClickedCallback([=] { loadInvites(); });
	content->add(std::move(retry));

	const auto delegate = content->lifetime().make_state<InviteListDelegate>(
		_controller->uiShow());
	setStyleOverrides(&st::inviteLinkList);
	const auto list = content->add(object_ptr<PeerListContent>(content, this));
	_list = list;
	delegate->setContent(list);
	_messageLabel->hide();
	_retryButton->hide();
	setDelegate(delegate);
	list->setHideEmpty(true);

	Ui::AddDividerText(content, tr::lng_teagram_invite_footer(tr::marked));

	updateAvailability();
	updateMessage();
	Ui::ResizeFitChild(this, content);

	session().appConfig().value()
		| rpl::on_next(
			[=] {
				const auto enabled = UserInvitesEnabled(_session);
				const auto mode = session().appConfig().get<QString>(
					u"registration_mode"_q, QString());
				if (enabled && !_available) {
					_available = true;
					_unavailableShown = false;
					_serverDisabled = false;
					_createBlocked = false;
					_createError.clear();
					_registrationMode = mode;
					loadInvites();
				} else if (_available && !enabled) {
					updateAvailability();
				} else if (enabled && mode != _registrationMode) {
					_registrationMode = mode;
					_serverDisabled = false;
					_createBlocked = false;
					_createError.clear();
					loadInvites();
				}
			},
			PeerListController::lifetime());
}

void InviteFriends::updateAvailability() {
	_available = UserInvitesEnabled(_session);
	_createButton->setDisabled(!details::CanCreateInvite(createConditions()));
	if (!_available && !_unavailableShown) {
		_unavailableShown = true;
		_controller->showToast(tr::lng_teagram_invite_off(tr::now));
		showOther(MainId());
		_messageLabel->setText(tr::lng_teagram_invite_off(tr::now));
		_messageLabel->show();
	}
}

void InviteFriends::updateSummary() {
	if (!_loaded) {
		_quotaLabel->hide();
		_createButton->setDisabled(true);
		return;
	}
	const auto left = details::InviteQuotaLeft(_quota, _weeklyCount);
	auto summary = QString();
	if (!_createError.isEmpty()) {
		summary = _createError;
	} else if (left > 0) {
		summary = tr::lng_teagram_invite_quota(
			tr::now, lt_count, left, lt_total, QString::number(_quota));
	} else if (_nextAvailableAt) {
		summary = tr::lng_teagram_invite_quota_reached(
			tr::now, lt_count, _quota, lt_date, InviteDate(*_nextAvailableAt));
	} else {
		summary = tr::lng_teagram_invite_quota(tr::now, lt_count, 0, lt_total,
											   QString::number(_quota));
	}
	_quotaLabel->setText(summary);
	_quotaLabel->setTextColorOverride(_createError.isEmpty()
										  ? st::windowSubTextFg->c
										  : st::boxTextFgError->c);
	_quotaLabel->show();
	_createButton->setAccessibleDescription(summary);
	_createButton->setDisabled(!details::CanCreateInvite(createConditions()));
}

void InviteFriends::updateMessage() {
	_retryButton->hide();
	if (!_available) {
		_messageLabel->setText(tr::lng_teagram_invite_off(tr::now));
		_messageLabel->show();
	} else if (_serverDisabled && !_loaded) {
		_messageLabel->setText(_createError.isEmpty()
								   ? tr::lng_teagram_invite_off(tr::now)
								   : _createError);
		_messageLabel->show();
	} else if (!_loaded && _loading) {
		_messageLabel->setText(tr::lng_teagram_invite_loading(tr::now));
		_messageLabel->show();
	} else if (_listFailed) {
		_messageLabel->setText(tr::lng_teagram_invite_list_failed(tr::now));
		_messageLabel->show();
		_retryButton->show();
	} else if (_loaded && !_list->fullRowsCount()) {
		_messageLabel->setText(tr::lng_teagram_invite_empty(tr::now));
		_messageLabel->show();
	} else {
		_messageLabel->hide();
	}
}

void InviteFriends::prepare() {
	_available = UserInvitesEnabled(_session);
	_registrationMode
		= session().appConfig().get<QString>(u"registration_mode"_q, QString());
	if (_serverDisabled) {
		updateAvailability();
		updateSummary();
		updateMessage();
		return;
	}
	if (_loaded && _createBlocked && !_serverDisabled) {
		_createBlocked = false;
		_createError.clear();
		updateSummary();
	}
	if (_available && !_loaded && !_listRequestId) {
		loadInvites();
	} else {
		updateAvailability();
	}
}

void InviteFriends::loadInvites(Fn<void()> done) {
	const auto requestId = details::RunInviteListRequest(
		_available, _listRequestId,
		[=] {
			_listFailed = false;
			_loading = !_loaded;
			_createError.clear();
			updateMessage();
		},
		[=](auto requestDone, auto requestFail) {
			return session()
				.api()
				.request(MTPteagram_ListUserInvites())
				.done(crl::guard(this, std::move(requestDone)))
				.fail(crl::guard(this, std::move(requestFail)))
				.handleAllErrors()
				.send();
		},
		[=](const MTPteagram_UserInvites &result) {
			_listRequestId = 0;
			result.match([&](const MTPDteagram_userInvites &data) {
				setRows(data);
				_loaded = true;
				_loading = false;
				_listFailed = false;
				updateSummary();
				updateMessage();
				if (done) {
					done();
				}
			});
		},
		[=](const MTP::Error &error) {
			_listRequestId = 0;
			_loading = false;
			if (details::ClassifyInviteListFailure(error.type())
				== details::InviteListFailure::Disabled) {
				_serverDisabled = true;
				_createBlocked = true;
				_createError = tr::lng_teagram_invite_off(tr::now);
				_listFailed = false;
				if (!_unavailableShown) {
					_unavailableShown = true;
					_controller->showToast(tr::lng_teagram_invite_off(tr::now));
					showOther(MainId());
				}
				for (auto i = 0; i != delegate()->peerListFullRowsCount();
					 ++i) {
					static_cast<InviteRow *>(delegate()->peerListRowAt(i).get())
						->setShowDuration(false);
				}
			} else {
				_listFailed = true;
			}
			updateSummary();
			updateMessage();
			if (done) {
				done();
			}
		});
	if (requestId) {
		_listRequestId = *requestId;
		_createButton->setDisabled(true);
	}
}

void InviteFriends::setRows(const MTPDteagram_userInvites &data) {
	_quota = data.vquota().v;
	_weeklyCount = data.vweekly_count().v;
	_nextAvailableAt = data.vnext_available_at()
						   ? std::optional<TimeId>(data.vnext_available_at()->v)
						   : std::nullopt;
	while (delegate()->peerListFullRowsCount()) {
		delegate()->peerListRemoveRow(delegate()->peerListRowAt(0));
	}
	for (const auto &invite : data.vinvites().v) {
		invite.match([&](const MTPDteagram_userInvite &item) {
			auto row = std::make_unique<InviteRow>(
				this, InviteData{
						  .id = InviteId(item.vid().v),
						  .state = qs(item.vstate()),
						  .createdAt = item.vcreated_at().v,
						  .expiresAt = item.vexpires_at().v,
						  .showDuration = !_serverDisabled,
					  });
			delegate()->peerListAppendRow(std::move(row));
		});
	}
	delegate()->peerListRefreshRows();
}

void InviteFriends::createInvite() {
	const auto requestId = details::RunInviteCreateRequest(
		_createRequest, createConditions(),
		[=] {
			_creatingValue = true;
			_createError.clear();
			updateSummary();
			updateMessage();
		},
		[=](auto requestDone, auto requestFail) {
			return session()
				.api()
				.request(MTPteagram_CreateUserInvite())
				.done(crl::guard(this, std::move(requestDone)))
				.fail(crl::guard(this, std::move(requestFail)))
				.handleAllErrors()
				.send();
		},
		[=](const MTPteagram_UserInviteLink &result) {
			_createRequestId = 0;
			_creatingValue = false;
			result.match([&](const MTPDteagram_userInviteLink &data) {
				const auto createdAt = base::unixtime::now();
				auto row = std::make_unique<InviteRow>(
					this, InviteData{
							  .id = InviteId(data.vid().v),
							  .state = u"pending"_q,
							  .createdAt = createdAt,
							  .expiresAt = data.vexpires_at().v,
							  .showDuration = !_serverDisabled,
						  });
				delegate()->peerListPrependRow(std::move(row));
				delegate()->peerListRefreshRows();
				_nextAvailableAt = details::InviteNextAvailableAtAfterCreate(
					_quota, _weeklyCount, _nextAvailableAt, createdAt);
				++_weeklyCount;
				updateSummary();
				updateMessage();
				showInviteLink(qs(data.vlink()), data.vexpires_at().v);
				loadInvites();
			});
		},
		[=](const MTP::Error &error) {
			_createRequestId = 0;
			_creatingValue = false;
			showCreateError(error);
		});
	if (requestId) {
		_createRequestId = *requestId;
	}
}

void InviteFriends::showCreateError(const MTP::Error &error) {
	const auto failure = details::ClassifyInviteCreateFailure(
		error.type(),
		session().appConfig().get<QString>(u"registration_mode"_q, QString())
			== u"open"_q);
	switch (failure.kind) {
	case details::InviteCreateFailureKind::AccountTooNew:
		_createBlocked = true;
		if (failure.waitSeconds) {
			_createError = tr::lng_teagram_invite_too_new(
				tr::now, lt_date,
				InviteDate(base::unixtime::now() + *failure.waitSeconds));
		} else {
			_createError = tr::lng_teagram_invite_too_new_later(tr::now);
		}
		break;
	case details::InviteCreateFailureKind::RegistrationClosed:
		_serverDisabled = true;
		_createBlocked = true;
		_createError = failure.registrationOpen
						   ? tr::lng_teagram_invite_open(tr::now)
						   : tr::lng_teagram_invite_closed(tr::now);
		break;
	case details::InviteCreateFailureKind::InvitesDisabled:
		_serverDisabled = true;
		_createBlocked = true;
		_createError = tr::lng_teagram_invite_off(tr::now);
		break;
	case details::InviteCreateFailureKind::WeeklyLimitReached:
		_createError.clear();
		loadInvites();
		break;
	case details::InviteCreateFailureKind::Other:
		_createError = tr::lng_teagram_invite_create_failed(tr::now);
		break;
	}
	if (_serverDisabled) {
		for (auto i = 0; i != delegate()->peerListFullRowsCount(); ++i) {
			static_cast<InviteRow *>(delegate()->peerListRowAt(i).get())
				->setShowDuration(false);
		}
	}
	updateMessage();
	updateSummary();
}

void InviteFriends::revokeInvite(InviteData data) {
	if (!details::CanRevokeInvite(data.state, bool(_revokeRequestId))) {
		return;
	}
	_revokeId = data.id;
	const auto revokeError = std::make_shared<rpl::variable<QString>>();
	_controller->show(Ui::MakeConfirmBox({
		.text = rpl::combine(tr::lng_teagram_invite_revoke_text(),
							 revokeError->value())
				| rpl::map([](const QString &text, const QString &error) {
					  return error.isEmpty() ? text
											 : (text + u"\n\n"_q + error);
				  }),
		.confirmed = crl::guard(
			this,
			[=](Fn<void()> close) {
				const auto requestId = details::RunInviteRevokeRequest(
					data.state, bool(_revokeRequestId),
					[=] { *revokeError = QString(); },
					[=](auto requestDone, auto requestFail) {
						return session()
							.api()
							.request(
								MTPteagram_RevokeUserInvite(MTP_long(data.id)))
							.done(crl::guard(this, std::move(requestDone)))
							.fail(crl::guard(this, std::move(requestFail)))
							.handleAllErrors()
							.send();
					},
					[=](const MTPBool &result) {
						_revokeRequestId = 0;
						if (mtpIsFalse(result)) {
							*revokeError
								= tr::lng_teagram_invite_revoke_failed(tr::now);
							return;
						}
						loadInvites(crl::guard(this, [=] {
							const auto row = delegate()->peerListFindRow(
								UniqueRowIdFromString(
									QString::number(_revokeId)));
							const auto used
								= row
								  && static_cast<InviteRow *>(row)->data().state
										 == u"used"_q;
							_controller->showToast(
								used ? tr::lng_teagram_invite_revoke_used(
										   tr::now)
									 : tr::lng_teagram_invite_revoked_toast(
										   tr::now));
						}));
						close();
					},
					[=](const MTP::Error &) {
						_revokeRequestId = 0;
						*revokeError
							= tr::lng_teagram_invite_revoke_failed(tr::now);
					});
				if (requestId) {
					_revokeRequestId = *requestId;
				}
			}),
		.confirmText = tr::lng_teagram_invite_revoke(),
		.confirmStyle = &st::attentionBoxButton,
		.title = tr::lng_teagram_invite_revoke_title(),
	}));
}

void InviteFriends::showInviteLink(QString link, TimeId expiresAt) {
	const auto show = _controller->uiShow();
	show->showBox(Box([=](not_null<Ui::GenericBox *> box) {
		struct State {
			bool copied = false;
			bool confirming = false;
			QPointer<QWidget> copyButton;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto close = [=] {
			if (state->confirming) {
				return;
			} else if (state->copied) {
				box->closeBox();
				return;
			}
			state->confirming = true;
			const auto weak = base::make_weak(box.get());
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_teagram_invite_close_text(),
				.confirmed =
					[=](Fn<void()> closeConfirm) {
						state->confirming = false;
						if (const auto strong = weak.get()) {
							strong->closeBox();
						}
						closeConfirm();
					},
				.cancelled =
					[=] {
						state->confirming = false;
						if (state->copyButton) {
							state->copyButton->setFocus(Qt::OtherFocusReason);
						}
					},
				.confirmText = tr::lng_close(),
				.cancelText = tr::lng_cancel(),
				.confirmStyle = &st::attentionBoxButton,
				.title = tr::lng_teagram_invite_close_title(),
			}));
		};

		box->setStyle(st::confcallLinkBox);
		box->setWidth(st::boxWideWidth);
		box->setNoContentMargin(true);
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
		box->addTopButton(st::boxTitleClose, close);
		const auto escape = new QShortcut(QKeySequence(Qt::Key_Escape), box);
		escape->setContext(Qt::WindowShortcut);
		QObject::connect(escape, &QShortcut::activated, box.get(), close);

		box->addRow(Info::BotStarRef::CreateLinkHeaderIcon(box, _session),
					st::boxRowPadding + st::confcallLinkHeaderIconPadding);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box, tr::lng_teagram_invite_created_title(), st::boxTitle),
			st::boxRowPadding + st::confcallLinkTitlePadding, style::al_top);
		box->addRow(
			   object_ptr<Ui::FlatLabel>(
				   box,
				   tr::lng_teagram_invite_created_text(
					   lt_date, rpl::single(tr::marked(InviteDate(expiresAt))),
					   tr::marked),
				   st::confcallLinkCenteredText),
			   st::boxRowPadding + st::confcallLinkTitlePadding, style::al_top)
			->setTryMakeSimilarLines(true);

		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip * 2);
		const auto preview
			= box->addRow(Info::BotStarRef::MakeLinkLabel(box, link));
		preview->setAccessibleName(link);
		preview->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(link);
			state->copied = true;
			box->setCloseByOutsideClick(true);
			show->showToast(tr::lng_group_invite_copied(tr::now));
		});
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::FlatLabel>(
						box, tr::lng_teagram_invite_created_note(tr::marked),
						st::confcallLinkCenteredText),
					style::al_top)
			->setTryMakeSimilarLines(true);
		const auto copy = box->addButton(
			tr::lng_teagram_invite_copy(),
			[=] {
				QGuiApplication::clipboard()->setText(link);
				state->copied = true;
				box->setCloseByOutsideClick(true);
				show->showToast(tr::lng_group_invite_copied(tr::now));
			},
			st::confcallLinkCopyButton);
		state->copyButton = copy.get();
		copy->setFocus(Qt::OtherFocusReason);
	}));
}

void InviteFriends::rowClicked(not_null<PeerListRow *> row) {
	if (static_cast<InviteRow *>(row.get())->data().state == u"pending"_q) {
		delegate()->peerListShowRowMenu(row, true);
	}
}

void InviteFriends::rowRightActionClicked(not_null<PeerListRow *> row) {
	if (static_cast<InviteRow *>(row.get())->data().state == u"pending"_q) {
		delegate()->peerListShowRowMenu(row, true);
	}
}

base::unique_qptr<Ui::PopupMenu>
InviteFriends::rowContextMenu(QWidget *parent, not_null<PeerListRow *> row) {
	const auto data = static_cast<InviteRow *>(row.get())->data();
	if (data.state != u"pending"_q) {
		return nullptr;
	}
	auto result
		= base::make_unique_q<Ui::PopupMenu>(parent, st::popupMenuWithIcons);
	Ui::Menu::CreateAddActionCallback(result.get())({
		.text = tr::lng_teagram_invite_revoke(tr::now),
		.handler = [=] { revokeInvite(data); },
		.icon = &st::menuIconRemoveAttention,
		.isAttention = true,
	});
	return result;
}

void InviteFriends::rowUpdateRow(not_null<InviteRow *> row) {
	delegate()->peerListUpdateRow(row);
}

void InviteFriends::rowPaintIcon(QPainter &p, int x, int y, int size,
								 const InviteData &data) {
	const auto skip = st::inviteLinkIconSkip;
	const auto inner = size - 2 * skip;
	const auto stroke = st::inviteLinkIconStroke;
	const auto revoked = (data.state == u"revoked"_q);
	const auto pending = (data.state == u"pending"_q);
	const auto total = data.expiresAt - data.createdAt;
	const auto now = base::unixtime::now();
	const auto progress
		= total > 0 ? std::clamp((now - data.createdAt) / double(total), 0., 1.)
					: 0.;
	const auto expiringSoon = pending && progress >= .75;
	const auto bg = pending ? (expiringSoon ? &st::msgFile4Bg : &st::msgFile2Bg)
					: data.state == u"used"_q ? &st::msgFile1Bg
					: revoked				  ? &st::windowSubTextFg
											  : &st::msgFile3Bg;
	p.setPen(Qt::NoPen);
	p.setBrush(*bg);
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto rect
			= QRect(x + skip, y + skip, inner, inner)
			  - ((pending && data.expiresAt > data.createdAt) ? Margins(stroke)
															  : Margins(0));
		p.drawEllipse(rect);
	}
	(revoked ? st::inviteLinkRevokedIcon : st::inviteLinkIcon)
		.paintInCenter(p, Rect(x + skip, y + skip, Size(inner)));
	if (pending && total > 0) {
		auto hq = PainterHighQualityEnabler(p);
		auto pen = QPen((*bg)->c);
		pen.setWidth(stroke);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto margins = .5 * stroke;
		p.drawArc(QRectF(x + skip, y + skip, inner, inner)
					  .marginsAdded({
						  margins,
						  margins,
						  margins,
						  margins,
					  }),
				  arc::kQuarterLength, arc::kFullLength * (1. - progress));
	}
}

} // namespace

Type InviteFriendsId() { return InviteFriends::Id(); }

const auto kMeta = Builder::BuildHelper(
	{
		.id = InviteFriends::Id(),
		.parentId = nullptr,
		.title = &tr::lng_teagram_invite_title,
		.icon = &st::menuIconInvite,
	},
	[](Builder::SectionBuilder &) {});

} // namespace Settings
