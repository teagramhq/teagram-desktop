/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_account.h"

#include "base/platform/base_platform_info.h"
#include "intro/intro_server_discovery.h"
#include "core/application.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h" // Storage::StartResult.
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "data/data_download_manager.h"
#include "window/window_controller.h"
#include "media/audio/media_audio.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_server_enrollment.h"
#include "mainwidget.h"
#include "api/api_updates.h"
#include "ui/ui_utility.h"
#include "boxes/abstract_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include "main/main_app_config.h"
#include "main/main_account_persistence.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"

#include <algorithm>
#include <QtCore/QTimer>

namespace Main {
namespace {

constexpr auto kWideIdsTag = ~uint64(0);

// Why an account that must be pinned to a custom server could not
// start on it. All end in the same blocked state — no endpoint,
// no key, no connection — and differ only in what the user is told.
enum class PinFailure {
	None,
	AuthorizationWriteFailed, // A prior auth snapshot did not reach disk.
	ForgetTransitionFailed,
	CacheBindingWriteFailed,
	ConfigUnreadable, // Marker set, config blob missing or corrupt.
	PinMissing,       // Marker set, config parses but carries no pin.
	MarkerUnreadable, // Prefs unreadable, so pinned-unknown.
};

[[nodiscard]] const char *PinFailureLog(PinFailure failure) {
	switch (failure) {
	case PinFailure::AuthorizationWriteFailed:
		return "previous authorization snapshot was not durable";
	case PinFailure::ForgetTransitionFailed:
		return "server forget transition could not be completed";
	case PinFailure::CacheBindingWriteFailed:
		return "cached message identity could not be recorded";
	case PinFailure::ConfigUnreadable: return "config could not be read";
	case PinFailure::PinMissing: return "config carries no pin";
	case PinFailure::MarkerUnreadable: return "prefs could not be read";
	case PinFailure::None: break;
	}
	Unexpected("PinFailure value in PinFailureLog.");
}

[[nodiscard]] QString PinFailureText(PinFailure failure) {
	// A user who never pinned a server must not be told they did, so
	// the unreadable-prefs case gets its own cause. Not knowing which
	// server this account uses is the state that case is reporting, so
	// the rest of the text must not imply the app knows either.
	const auto cause = (failure == PinFailure::AuthorizationWriteFailed)
		? u"The last authorization save did not complete, so this account "
			u"stays blocked rather than risk using a stale key."_q
		: (failure == PinFailure::ForgetTransitionFailed)
		? u"The server reset could not be completed, so this account stays "
			u"blocked until its local state can be cleared safely."_q
		: (failure == PinFailure::CacheBindingWriteFailed)
		? u"This account's cached messages could not be bound to a server "
			u"identity, so it stays blocked until that state can be recorded."_q
		: (failure == PinFailure::MarkerUnreadable)
		? u"This account's local data could not be read, so there is "
			u"no way to tell which server it belongs to."_q
		: u"The saved server settings for this account could not be "
			u"loaded. The account is pinned to a custom server, so it "
			u"will not connect to Telegram's servers instead."_q;
	return cause + u"\n\n"
		u"Until you decide, it connects to nothing.\n\n"
		u"Forgetting the server clears that setting on this device and "
		u"restarts the app. Nothing else is removed — your messages and "
		u"local data stay. If this account used a private server, its "
		u"address and key have to be entered again; until they are, "
		u"this app connects to no server.\n\n"
		u"See 'log.txt' for details."_q;
}

[[nodiscard]] object_ptr<Ui::GenericBox> MakePinFailureBox(
		PinFailure failure,
		Fn<bool()> forget) {
	// Deliberately not MakeConfirmBox. That binds Enter and Return to
	// the confirm button unconditionally, and this modal appears
	// unbidden at startup, where Enter is the ordinary reflex for
	// dismissing one — on a genuinely pinned account that reflex would
	// forget the server and land on a Telegram login screen with
	// nothing saying what changed.
	//
	// So every reflex dismissal leaves the account blocked: no key is
	// bound, and Escape, clicking outside and the close button all
	// just close. Forgetting the server has to be aimed at, and it is
	// the left attention button rather than the primary one.
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				PinFailureText(failure),
				st::boxLabel),
			st::boxPadding);
		box->addButton(
			rpl::single(u"Keep it blocked"_q),
			[=] { box->closeBox(); });
		box->addLeftButton(
			rpl::single(u"Forget server"_q),
			[=] {
				if (!forget()) {
					box->uiShow()->showToast(
						u"Could not safely forget this server. The account remains blocked."_q);
				}
			},
			st::attentionBoxButton);
	});
}

[[nodiscard]] QString ComposeDataString(const QString &dataName, int index) {
	auto result = dataName;
	result.replace('#', QString());
	if (index > 0) {
		result += '#' + QString::number(index + 1);
	}
	return result;
}

} // namespace

Account::Account(not_null<Domain *> domain, const QString &dataName, int index)
	: _domain(domain), _local(std::make_unique<Storage::Account>(
						   this, ComposeDataString(dataName, index))),
	  _serverEndpointRefreshRetryTimer(std::make_unique<QTimer>()),
	  _serverEndpointVerificationTimer(std::make_unique<QTimer>()) {
	_serverEndpointRefreshRetryTimer->setSingleShot(true);
	_serverEndpointVerificationTimer->setSingleShot(true);
	QObject::connect(
		_serverEndpointRefreshRetryTimer.get(), &QTimer::timeout,
		[=] { requestServerEndpointRefresh(_serverEndpointRefreshDcId); });
	QObject::connect(
		_serverEndpointVerificationTimer.get(), &QTimer::timeout, [=] {
			LOG(("MTP Error: endpoint verification timed out for dc %1.")
					.arg(_serverEndpointRefreshDcId));
			serverEndpointVerificationFinished(false);
		});
}

Account::~Account() {
	cancelServerEndpointRefresh();
	// Auth keys are normally persisted from a postponed write request. During
	// shutdown the event loop can finish before that callback runs, so take a
	// final durable authorization snapshot while the MTP instance still owns
	// the current key and pin.
	if (_mtp
		&& !_mtp->dcOptions().blocked()
		&& !_mtp->dcOptions().unenrolled()
		&& _mtp->isServerEnrollmentNetworkAllowed()
		&& !_serverCacheBindingMismatchPending) {
		static_cast<void>(details::CommitTeardownMtpAuthorization(
			_local.get(),
			[] {},
			[=] {
				// Keep the last recoverable on-disk binding and key snapshot;
				// never leave a live instance looking usable after its final
				// authorization commit failed.
				if (!_local->writeMtpAuthorizationFailure()) {
					LOG(("MTP Error: could not persist the authorization failure "
						"marker; keeping the account blocked for this shutdown."));
				} else {
					LOG(("MTP Error: final authorization snapshot failed; "
						"keeping the account blocked for restart."));
				}
				_mtp->dcOptions().constructBlocked();
			}));
	}
	if (const auto session = maybeSession()) {
		session->saveSettingsNowIfNeeded();
		_local->writeSearchSuggestionsIfNeeded();
	}
	destroySession(DestroyReason::Quitting);
}

Storage::Domain &Account::domainLocal() const {
	return _domain->local();
}

[[nodiscard]] Storage::StartResult Account::legacyStart(
		const QByteArray &passcode) {
	Expects(!_appConfig);

	return _local->legacyStart(passcode);
}

std::unique_ptr<MTP::Config> Account::prepareToStart(
		std::shared_ptr<MTP::AuthKey> localKey) {
	return _local->start(std::move(localKey));
}

void Account::start(std::unique_ptr<MTP::Config> config) {
	if (!details::ShouldRestoreStoredSession(
			bool(_sessionUserId), !_sessionUserSerialized.isEmpty())) {
		_sessionUserId = 0;
		_sessionUserSerialized = {};
		_sessionUserStreamVersion = 0;
		_storedSessionSettings.reset();
	}
	_appConfig = std::make_unique<AppConfig>(this);

	// An account pinned to a user-entered endpoint and RSA key must
	// never fall through to Telegram's built-in table and keys, which
	// would put a normal login screen in front of the user with
	// Telegram's own servers behind it. A config that holds the pin
	// settles it whatever the marker says; otherwise the marker
	// decides, and an unreadable marker counts as pinned.
	const auto originalPin = config
		? config->customServer()
		: MTP::CustomServer();
	const auto forgetBinding = Storage::ServerCacheBinding{
		.fingerprintKnown = bool(originalPin.key),
		.fingerprint = originalPin.key
			? originalPin.key->fingerprint()
			: uint64(0),
		.userIdKnown = bool(_sessionUserId),
		.userId = _sessionUserId.bare,
	};
	auto failure = [&] {
		if (_local->serverForgetBlocked()) {
			return PinFailure::ForgetTransitionFailed;
		} else if (_local->mtpAuthorizationWriteFailed()) {
			return PinFailure::AuthorizationWriteFailed;
		} else if (config && config->hasCustomServer()) {
			return PinFailure::None;
		} else if (_local->customServerPinUnknown()) {
			return PinFailure::MarkerUnreadable;
		} else if (details::ShouldBlockRestoredSessionWithoutServerPin(
				bool(_sessionUserId),
				config && config->hasCustomServer())) {
			return PinFailure::PinMissing;
		} else if (!_local->hasStoredCustomServer()) {
			return PinFailure::None;
		} else if (!config) {
			return PinFailure::ConfigUnreadable;
		}
		// The blob parsed and carries no pin: the crash window between
		// the marker flush and the config write leaves exactly this on
		// disk, as does a rollback to a binary that drops the block.
		return PinFailure::PinMissing;
	}();
	if (failure == PinFailure::None
		&& _sessionUserId
		&& config
		&& config->customServer().key
		&& _local->checkServerCacheBinding(
			config->customServer().key->fingerprint(),
			_sessionUserId.bare) == Storage::ServerCacheBindingStatus::None
		&& !_local->ensureServerCacheBinding(Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = config->customServer().key->fingerprint(),
			.userIdKnown = true,
			.userId = _sessionUserId.bare,
		})) {
		failure = PinFailure::CacheBindingWriteFailed;
	}
	if (failure == PinFailure::None
		&& !_sessionUserId
		&& (!config || !config->hasCustomServer())
		&& (!_mtpFields.keys.empty() || !_mtpKeysToDestroy.empty())
		&& !_local->ensureServerCacheBinding(Storage::ServerCacheBinding())) {
		failure = PinFailure::CacheBindingWriteFailed;
	}
	if (failure != PinFailure::None) {
		LOG(("MTP Error: custom server pin could not be honoured (%1), "
			"refusing to fall back to production."
			).arg(QString::fromUtf8(PinFailureLog(failure))));
		// A blocked account must not construct a Session from a restored
		// identity or carry its old keys into a later server enrollment.
		_sessionUserId = 0;
		_sessionUserSerialized = {};
		_sessionUserStreamVersion = 0;
		_storedSessionSettings.reset();
		_mtpFields.keys.clear();
		_mtpKeysToDestroy.clear();
		_mtpKeysToDestroyPin.reset();
		config = std::make_unique<MTP::Config>(MTP::Environment::Production);
		config->dcOptions().constructBlocked();
		// The authorization-write marker is already durable and has its own
		// reason. Other startup failures need the custom-server marker because
		// the blocked config is never written back.
		if (failure != PinFailure::AuthorizationWriteFailed) {
			_local->writeCustomServerBlocked(
				failure == PinFailure::MarkerUnreadable);
		}
		// Only a config write clears the markers, and a blocked account
		// never performs one, so without a way out the block is
		// terminal — including for an account that never had a custom
		// server and whose config reads perfectly. The box is that way
		// out; see MakePinFailureBox() for why it is built by hand.
		crl::on_main(this, [=] {
			Ui::show(MakePinFailureBox(failure, crl::guard(this, [=] {
				LOG(("MTP Info: forgetting the custom server pin for "
					"this account on the user's request."));
				return details::CommitServerForget(
					_local.get(),
					forgetBinding,
					[] { Core::Restart(); });
			})));
		});
	} else if (!config) {
		config = std::make_unique<MTP::Config>(
			Core::App().fallbackProductionConfig());
	}
	if (failure == PinFailure::None
		&& !_sessionUserId
		&& !config->blocked()
		&& !config->hasCustomServer()) {
		// A fresh account must not carry Telegram's built-in endpoints or
		// RSA keys while the enrollment form is waiting for a user-selected
		// server. Keep this state editable; blocked() is reserved for an
		// unreadable persisted pin and intentionally disables the form.
		config->dcOptions().constructUnenrolled();
	}
	const auto customServer = config->customServer();
	const auto currentPinAuthorized = customServer.key
		&& config->dcOptions().isAuthorized(customServer.dcId);
	if (!_sessionUserId
		&& (!customServer.key
			|| !currentPinAuthorized
			|| config->dcOptions().blocked()
			|| config->dcOptions().unenrolled())) {
		// An unenrolled account cannot carry authorization state from the
		// previous pin. Drop both the live keys and deferred destruction list
		// before the new enrollment can resume the instance.
		_mtpFields.keys.clear();
		discardStaleAuthorizationState();
	} else if (!_mtpKeysToDestroy.empty()) {
		if (!currentPinAuthorized
			|| !_mtpKeysToDestroyPin
			|| !MTP::SameCustomServerPin(
				*_mtpKeysToDestroyPin,
				customServer)) {
			discardStaleAuthorizationState();
		} else {
			_mtpKeysToDestroyPin = customServer;
		}
	}
	const auto startPaused = !_sessionUserId
		&& (!customServer.key
			|| !config->dcOptions().isAuthorized(customServer.dcId));
	if (!startMtp(std::move(config), startPaused)) {
		return;
	}
	if (!startPaused) {
		_appConfig->start();
	}
	watchProxyChanges();
	watchSessionChanges();
}

void Account::prepareToStartAdded(
		std::shared_ptr<MTP::AuthKey> localKey) {
	_local->startAdded(std::move(localKey));
}

void Account::watchProxyChanges() {
	using ProxyChange = Core::Application::ProxyChange;

	Core::App().proxyChanges(
	) | rpl::on_next([=](const ProxyChange &change) {
		const auto key = [&](const MTP::ProxyData &proxy) {
			return (proxy.type == MTP::ProxyData::Type::Mtproto)
				? std::make_pair(proxy.host, proxy.port)
				: std::make_pair(QString(), uint32(0));
		};
		if (_mtp) {
			_mtp->restart();
			if (key(change.was) != key(change.now)) {
				_mtp->reInitConnection(_mtp->mainDcId());
			}
		}
		if (_mtpForKeysDestroy) {
			if (!authorizationStateMatchesCurrentPin(_mtpForKeysDestroyPin)) {
				discardStaleAuthorizationState();
			} else {
				_mtpForKeysDestroy->restart();
			}
		}
	}, _lifetime);
}

bool Account::authorizationStateMatchesCurrentPin(
		const std::optional<MTP::CustomServer> &pin) const {
	if (!_mtp || !pin || !pin->key || !_mtp->dcOptions().hasCustomServer()) {
		return false;
	}
	const auto currentPin = _mtp->dcOptions().customServer();
	return currentPin.key
		&& _mtp->dcOptions().isAuthorized(currentPin.dcId)
		&& MTP::SameCustomServerPin(*pin, currentPin);
}

void Account::watchSessionChanges() {
	sessionChanges(
	) | rpl::on_next([=](Session *session) {
		if (!session && _mtp) {
			_mtp->setUserPhone(QString());
		}
	}, _lifetime);
}

uint64 Account::willHaveSessionUniqueId(MTP::Config *config) const {
	// See also Session::uniqueId.
	if (!_sessionUserId) {
		return 0;
	}
	return _sessionUserId.bare
		| (config && config->isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
}

bool Account::createSession(
		const MTPUser &user,
		std::unique_ptr<SessionSettings> settings) {
	return createSession(
		user,
		QByteArray(),
		0,
		settings ? std::move(settings) : std::make_unique<SessionSettings>());
}

bool Account::createSession(
		UserId id,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	DEBUG_LOG(("sessionUserSerialized.size: %1").arg(serialized.size()));
	QDataStream peekStream(serialized);
	const auto phone = Serialize::peekUserPhone(streamVersion, peekStream);
	const auto flags = MTPDuser::Flag::f_self | (phone.isEmpty()
		? MTPDuser::Flag()
		: MTPDuser::Flag::f_phone);
	const auto sessionUserId = _sessionUserId;

	return createSession(
		MTP_user(
			MTP_flags(flags),
			MTP_long(sessionUserId.bare),
			MTPlong(), // access_hash
			MTPstring(), // first_name
			MTPstring(), // last_name
			MTPstring(), // username
			MTP_string(phone),
			MTPUserProfilePhoto(),
			MTPUserStatus(),
			MTPint(), // bot_info_version
			MTPVector<MTPRestrictionReason>(),
			MTPstring(), // bot_inline_placeholder
			MTPstring(), // lang_code
			MTPEmojiStatus(),
			MTPVector<MTPUsername>(),
			MTPRecentStory(),
			MTPPeerColor(), // color
			MTPPeerColor(), // profile_color
			MTPint(), // bot_active_users
			MTPlong(), // bot_verification_icon
			MTPlong(), // send_paid_messages_stars
			MTPlong()), // linked_community_id
		serialized,
		streamVersion,
		std::move(settings));
}

bool Account::createSession(
		const MTPUser &user,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	Expects(_mtp != nullptr);
	Expects(_session == nullptr);
	Expects(_sessionValue.current() == nullptr);

	const auto userId = UserId(user.c_user().vid());
	const auto customServer = _mtp->dcOptions().customServer();
	const auto authorizedDcId = customServer.key
		? customServer.dcId
		: _mtp->mainDcId();
	const auto fingerprint = customServer.key
		? customServer.key->fingerprint()
		: uint64(0);
	const auto cacheBinding = _local->checkServerCacheBinding(
		fingerprint,
		userId.bare);
	if (cacheBinding == Storage::ServerCacheBindingStatus::Mismatch) {
		if (!_mtp->dcOptions().isAuthorized(authorizedDcId)
			&& !_mtp->dcOptions().markAuthorized(authorizedDcId)) {
			return false;
		}
		_serverCacheBindingMismatchPending = true;
		_mtp->stopForServerEnrollment();
		_serverCacheBindingMismatch.fire({});
		return false;
	}
	if (cacheBinding == Storage::ServerCacheBindingStatus::None
		&& customServer.key
		&& !_local->ensureServerCacheBinding(Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = fingerprint,
			.userIdKnown = true,
			.userId = userId.bare,
		})) {
		return false;
	}
	_serverCacheBindingMismatchPending = false;

	_session = std::make_unique<Session>(this, user, std::move(settings));
	if (!serialized.isEmpty()) {
		local().readSelf(_session.get(), serialized, streamVersion);
	}
	const auto previousOptions = _mtp->dcOptions().serialize();
	const auto markedAuthorized = _mtp->dcOptions().markAuthorized(
		authorizedDcId);
	const auto restoreOptions = [&] {
		if (!_mtp->dcOptions().constructFromSerialized(previousOptions)) {
			_mtp->dcOptions().constructBlocked();
		}
	};
	const auto committed = details::CommitPostAuthMtpAuthorization(
		_local.get(),
		[=] {
			if (!_mtpKeysToDestroy.empty()) {
				destroyMtpKeys(base::take(_mtpKeysToDestroy));
			}
			// Session construction can fail the durable authorization commit.
			// Register it only after that commit succeeds, while it is still
			// unpublished and before observers see the session.
			Core::App().downloadManager().trackSession(_session.get());
			_sessionValue = _session.get();
			_sessionUserId = 0;
		},
		[=] {
			if (!local().writeMtpAuthorizationFailure()) {
				LOG(("MTP Error: could not persist the authorization failure "
					"marker; keeping the account closed."));
			}
			LOG(("MTP Error: could not synchronously persist the authorization "
				"state; keeping the account closed."));
			if (markedAuthorized) {
				restoreOptions();
				if (!local().writeMtpConfig(true)) {
					_mtp->dcOptions().constructBlocked();
				}
			} else {
				_mtp->dcOptions().constructBlocked();
			}
			_session.reset();
		});
	if (!committed) {
		return false;
	}

	Ensures(_session != nullptr);
	local().writeSelf();
	return true;
}

void Account::destroySession(DestroyReason reason) {
	_storedSessionSettings.reset();
	_sessionUserId = 0;
	_sessionUserSerialized = {};
	if (!sessionExists()) {
		return;
	}

	_sessionValue = nullptr;

	if (reason == DestroyReason::LoggedOut) {
		_session->finishLogout();
	}
	_session = nullptr;
}

bool Account::sessionExists() const {
	return (_sessionValue.current() != nullptr);
}

Session &Account::session() const {
	Expects(sessionExists());

	return *_sessionValue.current();
}

Session *Account::maybeSession() const {
	return _sessionValue.current();
}

rpl::producer<Session*> Account::sessionValue() const {
	return _sessionValue.value();
}

rpl::producer<Session*> Account::sessionChanges() const {
	return _sessionValue.changes();
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpValue() const {
	return _mtpValue.value() | rpl::map([](MTP::Instance *instance) {
		return not_null{ instance };
	});
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpMainSessionValue() const {
	return mtpValue() | rpl::map([=](not_null<MTP::Instance*> instance) {
		return instance->mainDcIdValue() | rpl::map_to(instance);
	}) | rpl::flatten_latest();
}

rpl::producer<MTPUpdates> Account::mtpUpdates() const {
	return _mtpUpdates.events();
}

rpl::producer<> Account::mtpNewSessionCreated() const {
	return _mtpNewSessionCreated.events();
}

rpl::producer<> Account::serverCacheBindingMismatch() const {
	return _serverCacheBindingMismatch.events();
}

void Account::setMtpMainDcId(MTP::DcId mainDcId) {
	Expects(!_mtp);

	_mtpFields.mainDcId = mainDcId;
}

void Account::setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key) {
	Expects(!_mtp);
	Expects(key != nullptr);

	_mtpFields.keys.push_back(std::move(key));
}

QByteArray Account::serializeMtpAuthorization() const {
	const auto serialize = [&](
			MTP::DcId mainDcId,
			const MTP::AuthKeysList &keys,
			const MTP::AuthKeysList &keysToDestroy) {
		const auto keysSize = [](auto &list) {
			const auto keyDataSize = MTP::AuthKey::Data().size();
			return sizeof(qint32)
				+ list.size() * (sizeof(qint32) + keyDataSize);
		};
		const auto writeKeys = [](
				QDataStream &stream,
				const MTP::AuthKeysList &keys) {
			stream << qint32(keys.size());
			for (const auto &key : keys) {
				stream << qint32(key->dcId());
				key->write(stream);
			}
		};

		auto result = QByteArray();
		// wide tag + userId + mainDcId
		auto size = 2 * sizeof(quint64) + sizeof(qint32);
		size += keysSize(keys) + keysSize(keysToDestroy);
		result.reserve(size);
		{
			QDataStream stream(&result, QIODevice::WriteOnly);
			stream.setVersion(QDataStream::Qt_5_1);

			const auto currentUserId = _session
				? _session->userId()
				: UserId();
			stream
				<< quint64(kWideIdsTag)
				<< quint64(currentUserId.bare)
				<< qint32(mainDcId);
			writeKeys(stream, keys);
			writeKeys(stream, keysToDestroy);

			DEBUG_LOG(("MTP Info: Keys written, userId: %1, dcId: %2"
				).arg(currentUserId.bare
				).arg(mainDcId));
		}
		return result;
	};
	if (_mtp) {
		auto keys = _mtp->getKeysForWrite();
		const auto currentPin = _mtp->dcOptions().customServer();
		const auto currentPinAuthorized = currentPin.key
			&& _mtp->dcOptions().isAuthorized(currentPin.dcId);
		if (!_sessionUserId && !sessionExists() && !currentPinAuthorized) {
			keys.clear();
		}
		auto keysToDestroy = MTP::AuthKeysList();
		if (_mtpForKeysDestroy
			&& authorizationStateMatchesCurrentPin(_mtpForKeysDestroyPin)) {
			keysToDestroy = _mtpForKeysDestroy->getKeysForWrite();
		}
		if (authorizationStateMatchesCurrentPin(_mtpKeysToDestroyPin)) {
			keysToDestroy.insert(
				keysToDestroy.end(),
				_mtpKeysToDestroy.begin(),
				_mtpKeysToDestroy.end());
		}
		return serialize(_mtp->mainDcId(), keys, keysToDestroy);
	}
	const auto &keys = _mtpFields.keys;
	// A deferred key list has no trusted pin identity until the live MTP
	// instance verifies it. Never carry it across a restart where that
	// association is unavailable.
	const auto keysToDestroy = MTP::AuthKeysList();
	return serialize(_mtpFields.mainDcId, keys, keysToDestroy);
}

void Account::setSessionUserId(UserId userId) {
	Expects(!sessionExists());

	_sessionUserId = userId;
}

void Account::setSessionFromStorage(
		std::unique_ptr<SessionSettings> data,
		QByteArray &&selfSerialized,
		int32 selfStreamVersion) {
	Expects(!sessionExists());

	DEBUG_LOG(("sessionUserSerialized set: %1"
		).arg(selfSerialized.size()));

	_storedSessionSettings = std::move(data);
	_sessionUserSerialized = std::move(selfSerialized);
	_sessionUserStreamVersion = selfStreamVersion;
}

SessionSettings *Account::getSessionSettings() {
	if (_sessionUserId) {
		return _storedSessionSettings
			? _storedSessionSettings.get()
			: nullptr;
	} else if (const auto session = maybeSession()) {
		return &session->settings();
	}
	return nullptr;
}

void Account::setMtpAuthorization(const QByteArray &serialized) {
	Expects(!_mtp);

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto legacyUserId = Serialize::read<qint32>(stream);
	auto legacyMainDcId = Serialize::read<qint32>(stream);
	auto userId = quint64();
	auto mainDcId = qint32();
	if (((uint64(legacyUserId) << 32) | uint64(legacyMainDcId))
		== kWideIdsTag) {
		userId = Serialize::read<quint64>(stream);
		mainDcId = Serialize::read<qint32>(stream);
	} else {
		userId = legacyUserId;
		mainDcId = legacyMainDcId;
	}
	if (stream.status() != QDataStream::Ok) {
		LOG(("MTP Error: "
			"Could not read main fields from mtp authorization."));
		return;
	}

	setSessionUserId(userId);
	_mtpFields.mainDcId = mainDcId;

	const auto readKeys = [&](auto &keys) {
		const auto count = Serialize::read<qint32>(stream);
		if (stream.status() != QDataStream::Ok) {
			LOG(("MTP Error: "
				"Could not read keys count from mtp authorization."));
			return;
		}
		keys.reserve(count);
		for (auto i = 0; i != count; ++i) {
			const auto dcId = Serialize::read<qint32>(stream);
			const auto keyData = Serialize::read<MTP::AuthKey::Data>(stream);
			if (stream.status() != QDataStream::Ok) {
				LOG(("MTP Error: "
					"Could not read key from mtp authorization."));
				return;
			}
			keys.push_back(std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile, dcId, keyData));
		}
	};
	readKeys(_mtpFields.keys);
	readKeys(_mtpKeysToDestroy);
	LOG(("MTP Info: "
		"read keys, current: %1, to destroy: %2"
		).arg(_mtpFields.keys.size()
		).arg(_mtpKeysToDestroy.size()));
}

bool Account::beginServerReenrollment(
		details::ServerReenrollmentPrompt prompt,
		bool accepted) {
	return details::CommitServerReenrollment(prompt, accepted, [=] {
		if (!_mtp) {
			return false;
		}
		const auto pin = _mtp->dcOptions().customServer();
		if (!pin.key || !_mtp->dcOptions().isAuthorized(pin.dcId)) {
			return false;
		}
		if (!_local->writeServerReenrollmentTombstone()) {
			return false;
		}
		cancelServerEndpointRefresh();
		_mtp->stopForServerEnrollment();
		return true;
	});
}

bool Account::startMtp(
		std::unique_ptr<MTP::Config> config,
		bool startPaused) {
	Expects(!_mtp);

	const auto restoringSession = bool(_sessionUserId);
	const auto pausedUntilSessionCommit = startPaused || restoringSession;
	auto fields = base::take(_mtpFields);
	fields.mainDcId = MTP::ServerEnrollmentMainDcId({
		.hasAuthenticatedSession = restoringSession,
		.storedMainDcId = fields.mainDcId,
		.pinnedServerDcId = config->customServer().dcId,
	});
	fields.config = std::move(config);
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	fields.startPaused = pausedUntilSessionCommit;
	_mtp = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::Normal,
		std::move(fields));

	const auto writingKeys = _mtp->lifetime().make_state<bool>(false);
	_mtp->writeKeysRequests(
	) | rpl::filter([=] {
		return !*writingKeys;
	}) | rpl::on_next([=] {
		*writingKeys = true;
		Ui::PostponeCall(_mtp.get(), [=] {
			local().writeMtpData();
			*writingKeys = false;
		});
	}, _mtp->lifetime());

	const auto writingConfig = _lifetime.make_state<bool>(false);
	rpl::merge(
		_mtp->config().updates(),
		_mtp->dcOptions().changed() | rpl::to_empty
	) | rpl::filter([=] {
		return !*writingConfig;
	}) | rpl::on_next([=] {
		*writingConfig = true;
		Ui::PostponeCall(_mtp.get(), [=] {
			local().writeMtpConfig();
			*writingConfig = false;
		});
	}, _lifetime);

	_mtpFields.mainDcId = _mtp->mainDcId();

	_mtp->setUpdatesHandler([=](const MTP::Response &message) {
		checkForUpdates(message) || checkForNewSession(message);
	});
	_mtp->setGlobalFailHandler([=](const MTP::Error &, const MTP::Response &) {
		if (const auto session = maybeSession()) {
			crl::on_main(session, [=] { logOut(); });
		}
	});
	const auto mainDcConnection
		= _mtp->lifetime().make_state<Api::details::UpdateConnectionState>(
			_mtp->dcstate(_mtp->mainDcId()) == MTP::ConnectedState);
	_mtp->setStateChangedHandler([=](MTP::ShiftedDcId dc, int32 state) {
		if (dc == _mtp->mainDcId()) {
			if (state == MTP::ConnectedState) {
				_serverEndpointRefreshFailures = 0;
				_serverEndpointRefreshIdentityFailure = false;
				const auto resumePausedRequests
					= _endpointRefresh.hasCandidate();
				cancelServerEndpointRefresh();
				if (resumePausedRequests) {
					_mtp->resume();
				}
			}
			Core::App().settings().proxy().connectionTypeChangesNotify();
			Core::App().checkProxyRotation(this, state);
			if (mainDcConnection->update(state == MTP::ConnectedState)) {
				if (const auto session = maybeSession()) {
					session->updates().getDifferenceAfterReconnect();
				}
			}
		}
	});
	_mtp->serverEndpointRefreshRequested()
		| rpl::on_next(
			[=](MTP::ShiftedDcId shiftedDcId) {
				requestServerEndpointRefresh(shiftedDcId);
			},
			_mtp->lifetime());
	_mtp->pinnedServerFailure()
		| rpl::on_next(
			[=](const std::optional<MTP::PinnedServerFailureReport> &report) {
				_serverEndpointRefreshIdentityFailure = report.has_value();
			},
			_mtp->lifetime());
	_mtp->setSessionResetHandler([=](MTP::ShiftedDcId shiftedDcId) {
		if (const auto session = maybeSession()) {
			if (shiftedDcId == _mtp->mainDcId()) {
				session->updates().getDifference();
			}
		}
	});

	// A paused enrollment account must not create the separate key-destroyer
	// instance either. Keep these keys until the account has authenticated,
	// when createSession() resumes their normal cleanup path.
	if (!pausedUntilSessionCommit && !_mtpKeysToDestroy.empty()) {
		destroyMtpKeys(base::take(_mtpKeysToDestroy));
	}

	if (restoringSession) {
		if (!createSession(
				_sessionUserId,
				_sessionUserSerialized,
				_sessionUserStreamVersion,
				(_storedSessionSettings
					? std::move(_storedSessionSettings)
					: std::make_unique<SessionSettings>()))) {
			// The instance was held behind the enrollment gate until the
			// authorization snapshot committed. Do not publish it, resume
			// network traffic, or consume the stored user id after failure.
			if (!_serverCacheBindingMismatchPending) {
				_mtp->dcOptions().constructBlocked();
			}
			LOG(("MTP Error: stored authorization could not be restored; "
				"keeping the account blocked."));
			return false;
		}
		_sessionUserSerialized = {};
		_sessionUserStreamVersion = 0;
		_restoredSession = true;
		_mtp->resume();
	}
	_storedSessionSettings = nullptr;

	if (const auto session = maybeSession()) {
		// Skip all pending self updates so that we won't local().writeSelf.
		session->changes().sendNotifications();
	}

	_mtpValue = _mtp.get();
	return true;
}

bool Account::checkForUpdates(const MTP::Response &message) {
	auto updates = MTPUpdates();
	auto from = message.reply.constData();
	if (!updates.read(from, from + message.reply.size())) {
		return false;
	}
	_mtpUpdates.fire(std::move(updates));
	return true;
}

bool Account::checkForNewSession(const MTP::Response &message) {
	auto newSession = MTPNewSession();
	auto from = message.reply.constData();
	if (!newSession.read(from, from + message.reply.size())) {
		return false;
	}
	_mtpNewSessionCreated.fire({});
	return true;
}

void Account::logOut() {
	if (_loggingOut) {
		return;
	}
	_loggingOut = true;
	if (_mtp) {
		_mtp->logout([=] { loggedOut(); });
	} else {
		// We log out because we've forgotten passcode.
		loggedOut();
	}
}

bool Account::loggingOut() const {
	return _loggingOut;
}

void Account::forcedLogOut() {
	if (sessionExists()) {
		loggedOut();
	}
}

void Account::loggedOut() {
	_loggingOut = false;
	cancelServerEndpointRefresh();
	_restoredSession = false;
	if (!local().clearTelegramStickerBotToken()) {
		LOG(("Failed to remove saved Telegram sticker bot token at logout."));
	}
	Media::Player::mixer()->stopAndClear();
	destroySession(DestroyReason::LoggedOut);
	if (_mtp) {
		// Logging out returns the account to enrollment. Stop all queued
		// work and drop the independent key-destroyer before clearing
		// authorization, so a proxy callback cannot restart the old pin.
		_mtp->stopForServerEnrollment();
		discardStaleAuthorizationState();
		const auto clearedAuthorization = _mtp->dcOptions().clearAuthorized();
		const auto unbound = !_mtp->dcOptions().blocked()
			&& !_mtp->dcOptions().hasCustomServer();
		if (unbound) {
			_mtp->dcOptions().constructUnenrolled();
		}
		resetAuthorizationKeys();
		if (clearedAuthorization || unbound) {
			local().writeMtpConfig();
		}
	} else {
		discardStaleAuthorizationState();
		_mtpFields.keys.clear();
	}
	local().reset();
	cSetOtherOnline(0);
}

void Account::discardStaleAuthorizationState() {
	_mtpForKeysDestroy = nullptr;
	_mtpForKeysDestroyPin.reset();
	_mtpKeysToDestroy.clear();
	_mtpKeysToDestroyPin.reset();
}

void Account::destroyMtpKeys(MTP::AuthKeysList &&keys) {
	Expects(_mtp != nullptr);

	if (keys.empty()) {
		return;
	}
	const auto currentPin = _mtp->dcOptions().customServer();
	const auto currentPinAuthorized = currentPin.key
		&& _mtp->dcOptions().isAuthorized(currentPin.dcId);
	if (!currentPinAuthorized) {
		// There is no verified destination to which these old keys can be
		// sent. Drop both the pending list and any independent destroyer.
		discardStaleAuthorizationState();
		return;
	}
	if (_mtpForKeysDestroy
		&& !authorizationStateMatchesCurrentPin(_mtpForKeysDestroyPin)) {
		// A pin change invalidates the copied config before a proxy callback
		// or logout gets a chance to observe it.
		_mtpForKeysDestroy = nullptr;
		_mtpForKeysDestroyPin.reset();
	}
	if (!_mtpKeysToDestroyPin
		|| !MTP::SameCustomServerPin(
			*_mtpKeysToDestroyPin,
			currentPin)) {
		// Keys loaded without a persisted pin, or queued for another pin,
		// must not be reused against the newly authorized server.
		_mtpKeysToDestroyPin.reset();
		return;
	}
	_mtpKeysToDestroyPin.reset();
	if (_mtpForKeysDestroy) {
		_mtpForKeysDestroy->addKeysForDestroy(std::move(keys));
		local().writeMtpData();
		return;
	}
	auto destroyFields = MTP::Instance::Fields();

	destroyFields.mainDcId = MTP::Instance::Fields::kNoneMainDc;
	destroyFields.config = std::make_unique<MTP::Config>(_mtp->config());
	destroyFields.keys = std::move(keys);
	destroyFields.deviceModel = Platform::DeviceModelPretty();
	destroyFields.systemVersion = Platform::SystemVersionPretty();
	_mtpForKeysDestroyPin = currentPin;
	_mtpForKeysDestroy = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::KeysDestroyer,
		std::move(destroyFields));
	_mtpForKeysDestroy->writeKeysRequests(
	) | rpl::on_next([=] {
		local().writeMtpData();
	}, _mtpForKeysDestroy->lifetime());
	_mtpForKeysDestroy->allKeysDestroyed(
	) | rpl::on_next([=] {
		LOG(("MTP Info: all keys scheduled for destroy are destroyed."));
		crl::on_main(this, [=] {
			_mtpForKeysDestroy = nullptr;
			_mtpForKeysDestroyPin.reset();
			local().writeMtpData();
		});
	}, _mtpForKeysDestroy->lifetime());
}

void Account::suggestMainDcId(MTP::DcId mainDcId) {
	Expects(_mtp != nullptr);

	_mtp->suggestMainDcId(mainDcId);
	if (_mtpFields.mainDcId != MTP::Instance::Fields::kNotSetMainDc) {
		_mtpFields.mainDcId = mainDcId;
	}
}

void Account::destroyStaleAuthorizationKeys() {
	Expects(_mtp != nullptr);

	for (const auto &key : _mtp->getKeysForWrite()) {
		// Disable this for now.
		if (key->type() == MTP::AuthKey::Type::ReadFromFile) {
			_mtpKeysToDestroy = _mtp->getKeysForWrite();
			const auto currentPin = _mtp->dcOptions().customServer();
			if (currentPin.key
				&& _mtp->dcOptions().isAuthorized(currentPin.dcId)) {
				_mtpKeysToDestroyPin = currentPin;
			} else {
				_mtpKeysToDestroyPin.reset();
			}
			LOG(("MTP Info: destroying stale keys, count: %1"
				).arg(_mtpKeysToDestroy.size()));
			resetAuthorizationKeys();
			return;
		}
	}
}

void Account::setHandleLoginCode(Fn<void(QString)> callback) {
	_handleLoginCode = std::move(callback);
}

void Account::handleLoginCode(const QString &code) const {
	if (_handleLoginCode) {
		_handleLoginCode(code);
	}
}

MTP::ServerEndpointRefreshEligibility
Account::serverEndpointRefreshEligibility(const MTP::CustomServer &pin) const {
	if (!sessionExists() || !_mtp) {
		return {};
	}
	const auto &options = _mtp->dcOptions();
	const auto keys = _mtp->getKeysForWrite();
	const auto hasStoredAuthKey
		= ranges::any_of(keys, [&](const MTP::AuthKeyPtr &key) {
			  return key && key->dcId() == pin.dcId
					 && key->type() == MTP::AuthKey::Type::ReadFromFile;
		  });
	return {
		.restoredSession = _restoredSession,
		.pinAuthorized = pin.key && options.isAuthorized(pin.dcId),
		.canResume = MTP::CanResumeServerEnrollment(options),
		.cacheBindingMismatch = _serverCacheBindingMismatchPending,
		.hasStoredAuthKey = hasStoredAuthKey,
		.identityFailure = _serverEndpointRefreshIdentityFailure,
	};
}

bool Account::canRefreshServerEndpoint() const {
	if (!sessionExists() || !_mtp) {
		return false;
	}
	const auto pin = _mtp->dcOptions().customServer();
	return MTP::CanRefreshServerEndpoint(serverEndpointRefreshEligibility(pin),
										 pin);
}

void Account::requestServerEndpointRefresh(MTP::ShiftedDcId shiftedDcId) {
	if (!_mtp
		|| MTP::BareDcId(shiftedDcId) != _mtp->dcOptions().customServer().dcId
		|| _serverEndpointRefreshRetryTimer->isActive()
		|| (_serverEndpointRefreshDiscovery
			&& _serverEndpointRefreshDiscovery->running())
		|| _endpointRefresh.hasOriginal() || _mtpForEndpointVerification) {
		return;
	}
	_serverEndpointRefreshDcId = shiftedDcId;
	const auto pin = _mtp->dcOptions().customServer();
	if (!_endpointRefresh.begin(serverEndpointRefreshEligibility(pin), pin)) {
		return;
	}
	const auto selection = MTP::CheckServerSelection(
		QString::fromStdString(pin.serverSelection));
	if (!_serverEndpointRefreshDiscovery) {
		_serverEndpointRefreshDiscovery = std::make_unique<
			Intro::details::ServerEndpointRefreshDiscovery>();
	}
	const auto started = _serverEndpointRefreshDiscovery->start(
		selection, QString::fromStdString(pin.discoveryOrigin),
		{
			.finished =
				[=](MTP::ServerDiscoveryResult result) {
					serverEndpointRefreshDiscovered(std::move(result));
				},
			.failed =
				[=](Intro::details::ServerEndpointRefreshFailure failure) {
					const auto statusCode
						= failure.httpStatusCode
							  ? QString::number(failure.httpStatusCode)
							  : u"n/a"_q;
					LOG(("MTP Error: server endpoint discovery at %1 for dc %2 "
						 "failed: %3, HTTP status %4.")
							.arg(QString::fromStdString(pin.discoveryOrigin))
							.arg(pin.dcId)
							.arg(Intro::details::
									 ServerEndpointRefreshFailureName(
										 failure.status))
							.arg(statusCode));
					_endpointRefresh.reset();
					++_serverEndpointRefreshFailures;
					scheduleServerEndpointRefreshRetry();
				},
		});
	if (!started && _endpointRefresh.hasOriginal()) {
		_endpointRefresh.reset();
	}
}

void Account::serverEndpointRefreshDiscovered(
	MTP::ServerDiscoveryResult result) {
	if (!canRefreshServerEndpoint() || !_endpointRefresh.hasOriginal()
		|| !_mtp) {
		if (_serverEndpointRefreshDiscovery) {
			_serverEndpointRefreshDiscovery->cancel();
		}
		_endpointRefresh.reset();
		return;
	}
	const auto original = _endpointRefresh.original();
	const auto current = _mtp->dcOptions().customServer();
	if (!MTP::SameCustomServerPin(original, current)) {
		_serverEndpointRefreshDiscovery->cancel();
		_endpointRefresh.reset();
		return;
	}
	const auto selection = MTP::CheckServerSelection(
		QString::fromStdString(original.serverSelection));
	const auto candidate = MTP::BuildCustomServerFromDiscovery(
		selection, result, QString::fromStdString(original.discoveryOrigin));
	if (!candidate) {
		LOG(("MTP Error: server endpoint discovery returned an unusable "
			 "endpoint for dc %1.")
				.arg(original.dcId));
		_serverEndpointRefreshDiscovery->cancel();
		_endpointRefresh.reset();
		++_serverEndpointRefreshFailures;
		scheduleServerEndpointRefreshRetry();
		return;
	}
	const auto identityFailure = _endpointRefresh.setCandidate(*candidate);
	if (identityFailure
		!= MTP::CustomServerEndpointRefreshIdentityFailure::None) {
		if (identityFailure
			== MTP::CustomServerEndpointRefreshIdentityFailure::DcIdMismatch) {
			LOG(("MTP Error: endpoint refresh discovery changed dc id %1 to "
				 "%2.")
					.arg(original.dcId)
					.arg(candidate->dcId));
			_serverEndpointRefreshIdentityFailure = true;
			_mtp->onPinnedServerFailure(original.dcId,
										MTP::PinnedServerFailure::DcIdMismatch);
		} else if (identityFailure
				   == MTP::CustomServerEndpointRefreshIdentityFailure::
					   KeyMismatch) {
			LOG(("MTP Error: endpoint refresh discovery returned a different "
				 "key for dc %1.")
					.arg(original.dcId));
			_serverEndpointRefreshIdentityFailure = true;
			_mtp->onPinnedServerFailure(
				original.dcId, MTP::PinnedServerFailure::KeyMismatch,
				candidate->key ? candidate->key->fingerprint() : 0);
		}
		_serverEndpointRefreshDiscovery->cancel();
		_endpointRefresh.reset();
		if (!_serverEndpointRefreshIdentityFailure) {
			++_serverEndpointRefreshFailures;
			scheduleServerEndpointRefreshRetry();
		}
		return;
	}
	if (MTP::SameCustomServerEndpoint(current, *candidate)
		&& _mtp->isServerEnrollmentNetworkAllowed()) {
		_serverEndpointRefreshFailures = 0;
		cancelServerEndpointRefresh();
		return;
	}
	const auto key = [&] {
		for (const auto &stored : _mtp->getKeysForWrite()) {
			if (stored && stored->dcId() == original.dcId
				&& stored->type() == MTP::AuthKey::Type::ReadFromFile) {
				return stored;
			}
		}
		return MTP::AuthKeyPtr();
	}();
	if (!key) {
		LOG(("MTP Error: endpoint refresh has no stored auth key for dc %1.")
				.arg(original.dcId));
		_serverEndpointRefreshDiscovery->cancel();
		_endpointRefresh.reset();
		return;
	}
	_mtp->pauseForServerEndpointRefresh();

	auto fields = MTP::Instance::Fields();
	fields.mainDcId = original.dcId;
	fields.config = std::make_unique<MTP::Config>(_mtp->config());
	if (!fields.config->dcOptions().refreshCustomServerEndpoint(original,
																*candidate)) {
		LOG(("MTP Error: could not stage the refreshed endpoint for dc %1.")
				.arg(original.dcId));
		_serverEndpointRefreshDiscovery->cancel();
		_mtp->resume();
		_endpointRefresh.reset();
		++_serverEndpointRefreshFailures;
		scheduleServerEndpointRefreshRetry();
		return;
	}
	fields.keys.push_back(key);
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	fields.startPaused = true;
	_mtpForEndpointVerification = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::EndpointVerifier, std::move(fields));
	const auto verifier = _mtpForEndpointVerification.get();
	verifier->pinnedServerFailure()
		| rpl::on_next(
			[=](const std::optional<MTP::PinnedServerFailureReport> &report) {
				if (report && _mtp) {
					_serverEndpointRefreshIdentityFailure = true;
					_mtp->onPinnedServerFailure(
						report->shiftedDcId, report->failure,
						report->presentedFingerprint, report->pinnedHostname,
						report->dialledAddress);
				}
			},
			verifier->lifetime());
	verifier->endpointVerificationResult()
		| rpl::on_next(
			[=](bool authenticated) {
				crl::on_main(this, [=] {
					if (_mtpForEndpointVerification.get() == verifier) {
						serverEndpointVerificationFinished(authenticated);
					}
				});
			},
			verifier->lifetime());
	_serverEndpointVerificationTimer->start(15 * 1000);
	verifier->resume();
}

void Account::serverEndpointVerificationFinished(bool authenticated) {
	_serverEndpointVerificationTimer->stop();
	if (_serverEndpointRefreshDiscovery) {
		_serverEndpointRefreshDiscovery->cancel();
	}
	if (!_endpointRefresh.hasCandidate() || !_mtp) {
		_mtpForEndpointVerification = nullptr;
		return;
	}
	auto verifier = std::move(_mtpForEndpointVerification);
	Expects(verifier != nullptr);
	const auto original = _endpointRefresh.original();
	const auto candidate = _endpointRefresh.candidate();
	const auto current = _mtp->dcOptions().customServer();
	auto committed = false;
	if (authenticated && MTP::SameCustomServerPin(original, current)) {
		const auto committedRefresh = _endpointRefresh.commit(
			authenticated,
			[&] {
				const auto refreshed
					= _mtp->dcOptions().refreshCustomServerEndpoint(original,
																	candidate);
				if (!refreshed) {
					LOG(("MTP Error: could not update the active endpoint for "
						 "dc %1.")
							.arg(original.dcId));
				}
				return refreshed;
			},
			[&] {
				const auto persisted = local().writeMtpConfig(true);
				if (!persisted) {
					LOG(("MTP Error: could not persist the refreshed endpoint "
						 "for dc %1.")
							.arg(original.dcId));
				}
				return persisted;
			},
			[&] {
				if (_mtpKeysToDestroyPin
					&& MTP::SameCustomServerPin(*_mtpKeysToDestroyPin,
												original)) {
					_mtpKeysToDestroyPin = candidate;
				}
				_mtp->resume();
			},
			[&] {
				if (!_mtp->dcOptions().refreshCustomServerEndpoint(candidate,
																   original)) {
					LOG(("MTP Error: could not roll back the unpersisted "
						 "endpoint for dc %1.")
							.arg(original.dcId));
				}
			});
		committed = committedRefresh;
	} else if (!authenticated) {
		LOG(("MTP Error: refreshed endpoint failed auth-key proof for dc %1.")
				.arg(_endpointRefresh.original().dcId));
	} else {
		LOG(("MTP Error: pin changed before endpoint verification committed "
			 "for dc %1.")
				.arg(original.dcId));
	}
	_endpointRefresh.reset();
	if (committed) {
		_serverEndpointRefreshFailures = 0;
		_serverEndpointRefreshRetryTimer->stop();
		return;
	}
	++_serverEndpointRefreshFailures;
	scheduleServerEndpointRefreshRetry();
}

void Account::scheduleServerEndpointRefreshRetry() {
	if (!MTP::ShouldScheduleServerEndpointRefreshRetry(
			canRefreshServerEndpoint(),
			_serverEndpointRefreshRetryTimer->isActive())) {
		return;
	}
	_serverEndpointRefreshRetryTimer->start(
		MTP::ServerEndpointRefreshRetryDelayMs(_serverEndpointRefreshFailures));
}

void Account::cancelServerEndpointRefresh() {
	if (_serverEndpointRefreshDiscovery) {
		_serverEndpointRefreshDiscovery->cancel();
	}
	_serverEndpointRefreshRetryTimer->stop();
	_serverEndpointVerificationTimer->stop();
	_mtpForEndpointVerification = nullptr;
	_endpointRefresh.reset();
}

void Account::resetAuthorizationKeys() {
	Expects(_mtp != nullptr);
	cancelServerEndpointRefresh();

	{
		const auto old = base::take(_mtp);
		auto config = std::make_unique<MTP::Config>(old->config());
		const auto customServer = config->customServer();
		if (!_sessionUserId
			&& !config->blocked()
			&& !customServer.key) {
			config->dcOptions().constructUnenrolled();
		}
		const auto startPaused = !_sessionUserId
			&& (!customServer.key
				|| !config->dcOptions().isAuthorized(customServer.dcId));
		startMtp(std::move(config), startPaused);
	}
	local().writeMtpData();
}

} // namespace Main
