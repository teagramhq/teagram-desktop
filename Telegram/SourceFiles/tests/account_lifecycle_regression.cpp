/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/account_lifecycle_regression.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/mac_protected_path_runtime.h"
#include "core/teagram_icon_choice.h"
#include "crl/crl_on_main.h"
#include "crl/crl_semaphore.h"
#include "data/data_chat.h"
#include "data/data_download_manager.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_account_persistence.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/sender.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"
#include "storage/storage_encryption.h"
#include "storage/streamed_file_downloader.h"
#include "ui/image/image_location.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace Tests {
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
namespace {

auto gLifecycleWriteCounts = LifecycleWriteCountsForRegressionTest();

} // namespace

void RecordLifecycleWriteForRegressionTest(
		LifecycleWriteForRegressionTest operation) {
	switch (operation) {
	case LifecycleWriteForRegressionTest::AuthorizationSnapshot:
		++gLifecycleWriteCounts.authorizationSnapshot;
		break;
	case LifecycleWriteForRegressionTest::AuthorizationFailureMarker:
		++gLifecycleWriteCounts.authorizationFailureMarker;
		break;
	case LifecycleWriteForRegressionTest::CustomServerBlockMarker:
		++gLifecycleWriteCounts.customServerBlockMarker;
		break;
	}
}

void ResetLifecycleWriteCountsForRegressionTest() {
	gLifecycleWriteCounts = LifecycleWriteCountsForRegressionTest();
}

LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest() {
	return gLifecycleWriteCounts;
}
#endif

namespace {

const char kRegressionServerKey[] = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g
5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO
62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/
+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9
t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs
5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB
-----END RSA PUBLIC KEY-----)";

const char kRegressionOtherServerKey[] = R"(-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAyMEdY1aR+sCR3ZSJrtztKTKqigvO/vBfqACJLZtS7QMgCGXJ6XIR
yy7mx66W0/sOFa7/1mAZtEoIokDP3ShoqF4fVNb6XeqgQfaUHd8wJpDWHcR2OFwv
plUUI1PLTktZ9uW2WE23b+ixNwJjJGwBDJPQEQFBE+vfmH0JP503wr5INS1poWg/
j25sIWeYPHYeOrFp/eXaqhISP6G+q2IeTaWTXpwZj4LzXq5YOpk4bYEQ6mvRq7D1
aHWfYmlEGepfaYR8Q0YqvvhYtMte3ITnuSJs171+GDqpdKcSwHnd6FudwGO4pcCO
j4WcDuXc2CTHgH8gFTNhp/Y8/SpDOhvn9QIDAQAB
-----END RSA PUBLIC KEY-----)";

[[nodiscard]] std::shared_ptr<MTP::details::RSAPublicKey>
RegressionServerKey() {
	return std::make_shared<MTP::details::RSAPublicKey>(bytes::make_span(
		kRegressionServerKey,
		sizeof(kRegressionServerKey) - 1));
}

struct ProtectedCacheFixtures {
	QString root;
	QString cacheRoot;
	QString mediaCacheRoot;
	QString cacheLeaf;
	QString mediaCacheLeaf;
	QString openedCache;
	QString openedMediaCache;
	QString cleanupRoot;
};

[[nodiscard]] bool WriteFixtureFile(const QString &path,
									const QByteArray &bytes) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
		   && file.flush();
}

[[nodiscard]] QByteArray SerializeRefusedDownloadHistory(const QString &path,
														 FullMsgId itemId) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << quint64(7) << qint32(Data::DownloadType::Document)
		   << qint64(1) << quint32(1) << quint64(itemId.peer.value)
		   << qint64(itemId.msg.bare) << quint64(0) << path;
	stream.device()->close();
	return result;
}

[[nodiscard]] bool
StoreRefusedDownloadHistory(not_null<Main::Account *> account,
							const QString &path, FullMsgId itemId) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	if (Core::MacProtectedPath::CheckExternalPath(
			Core::MacProtectedPath::Operation::Stat, path,
			"Tests::DownloadHistory::fixture")) {
		return false;
	}
	const auto serialized = SerializeRefusedDownloadHistory(path, itemId);
	const auto called = std::make_shared<bool>(false);
	account->local().updateDownloads([=] {
		*called = true;
		return serialized;
	});
	auto loop = QEventLoop();
	QTimer::singleShot(1500, &loop, &QEventLoop::quit);
	loop.exec();
	return *called && account->local().downloadsSerialized() == serialized;
}

[[nodiscard]] bool
RunRefusedDownloadHistoryRegression(not_null<Main::Session *> session,
									FullMsgId itemId) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	if (session->data().message(itemId)) {
		return false;
	}
	auto published = 0;
	auto lifetime = rpl::lifetime();
	Core::App().downloadManager().loadedAdded()
		| rpl::on_next(
			[&](not_null<const Data::DownloadedId *>) { ++published; },
			lifetime);
	auto loaded = 0;
	for (const auto entry : Core::App().downloadManager().loadedList()) {
		loaded += (entry->itemId == itemId);
	}
	return !published && !loaded;
}

[[nodiscard]] bool FixtureContainsOnlyMarker(const QString &path) {
	const auto entries = QDir(path).entryList(
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
		QDir::NoSort);
	if (entries != QStringList{"marker"}) {
		return false;
	}
	auto marker = QFile(path + "/marker");
	return marker.open(QIODevice::ReadOnly)
		   && marker.readAll() == "synthetic protected fixture";
}

[[nodiscard]] bool
PrepareProtectedCacheFixtures(ProtectedCacheFixtures *fixtures) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto home = qEnvironmentVariable("TDESKTOP_MAC_PROFILE_TEST_HOME");
	if (home.isEmpty()) {
		return false;
	}
	fixtures->root = QDir(home).filePath(
		"Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/"
		"SyntheticStorageFixtures");
	fixtures->cacheRoot = fixtures->root + "/cache-root";
	fixtures->mediaCacheRoot = fixtures->root + "/media-cache-root";
	fixtures->cacheLeaf = fixtures->root + "/cache-leaf";
	fixtures->mediaCacheLeaf = fixtures->root + "/media-cache-leaf";
	fixtures->openedCache = fixtures->root + "/opened-cache";
	fixtures->openedMediaCache = fixtures->root + "/opened-media-cache";
	fixtures->cleanupRoot = fixtures->root + "/legacy-cleanup";
	for (const auto &path : {
			 fixtures->cacheRoot,
			 fixtures->mediaCacheRoot,
			 fixtures->cacheLeaf,
			 fixtures->mediaCacheLeaf,
			 fixtures->openedCache,
			 fixtures->openedMediaCache,
			 fixtures->cleanupRoot,
		 }) {
		if (!QDir().mkpath(path)
			|| !WriteFixtureFile(path + "/marker",
								 "synthetic protected fixture")) {
			return false;
		}
	}
	return WriteFixtureFile(fixtures->cleanupRoot + "/unrecognized-legacy-file",
							"legacy bytes");
}

[[nodiscard]] bool FixtureContainsCleanupFiles(const QString &path) {
	const auto entries = QDir(path).entryList(
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
		QDir::Name);
	if (entries
		!= QStringList{
			"marker",
			"unrecognized-legacy-file",
		}) {
		return false;
	}
	auto marker = QFile(path + "/marker");
	auto legacy = QFile(path + "/unrecognized-legacy-file");
	return marker.open(QIODevice::ReadOnly)
		   && marker.readAll() == "synthetic protected fixture"
		   && legacy.open(QIODevice::ReadOnly)
		   && legacy.readAll() == "legacy bytes";
}

[[nodiscard]] bool
RunLegacyCleanupSymlinkRegression(const ProtectedCacheFixtures &fixtures) {
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto profile = Core::MacProtectedPath::ProfileRoot();
	const auto base = profile + "tdata/cleanup_regression";
	const auto original = base + ".original";
	const auto probeName = u"unrecognized-legacy-file"_q;
	const auto originalFile = original + '/' + probeName;
	const auto initialAlias = profile + "tdata/cleanup_initial";
	if (!QDir().mkpath(QFileInfo(initialAlias).absolutePath())
		|| !QFile::link(fixtures.cleanupRoot, initialAlias)) {
		return false;
	}
	struct InitialState {
		bool completed = false;
	};
	const auto initialState = std::make_shared<InitialState>();
	const auto initialLoop = std::make_shared<QEventLoop>();
	const auto initialTimer = std::make_shared<QTimer>();
	initialTimer->setSingleShot(true);
	QObject::connect(initialTimer.get(), &QTimer::timeout, initialLoop.get(),
					 &QEventLoop::quit);
	Storage::ClearLegacyFilesGuarded(
		initialAlias + '/',
		[](FnMut<void(::base::flat_set<QString> &&)> then) { then({}); },
		[=] {
			initialState->completed = true;
			initialLoop->quit();
		});
	if (!initialState->completed) {
		initialTimer->start(10000);
		initialLoop->exec();
	}
	initialTimer->stop();
	const auto initialRetained
		= initialState->completed
		  && FixtureContainsCleanupFiles(fixtures.cleanupRoot);
	QFile::remove(initialAlias);
	if (!initialRetained) {
		return false;
	}

	if (!QDir().mkpath(base)
		|| !WriteFixtureFile(base + '/' + probeName, "legacy bytes")) {
		return false;
	}
	struct State {
		bool swapped = false;
		bool completed = false;
	};
	const auto state = std::make_shared<State>();
	const auto loop = std::make_shared<QEventLoop>();
	const auto timer = std::make_shared<QTimer>();
	timer->setSingleShot(true);
	QObject::connect(timer.get(), &QTimer::timeout, loop.get(),
					 &QEventLoop::quit);
	Storage::ClearLegacyFilesGuarded(
		base + '/',
		[=](FnMut<void(::base::flat_set<QString> &&)> then) mutable {
			state->swapped = QDir().rename(base, original)
							 && QFile::link(fixtures.cleanupRoot, base);
			then({});
		},
		[=] {
			state->completed = true;
			loop->quit();
		});
	if (!state->completed) {
		timer->start(10000);
		loop->exec();
	}
	timer->stop();
	const auto protectedUnchanged
		= FixtureContainsCleanupFiles(fixtures.cleanupRoot);
	auto originalContents = QFile(originalFile);
	const auto originalRetained = QFileInfo::exists(originalFile);
	return state->swapped && state->completed && protectedUnchanged
		   && originalRetained && originalContents.open(QIODevice::ReadOnly)
		   && originalContents.readAll() == "legacy bytes";
}

[[nodiscard]] std::shared_ptr<MTP::details::RSAPublicKey>
RegressionOtherServerKey() {
	return std::make_shared<MTP::details::RSAPublicKey>(bytes::make_span(
		kRegressionOtherServerKey,
		sizeof(kRegressionOtherServerKey) - 1));
}

[[nodiscard]] bool ConfigurePinnedServer(
		not_null<Main::Account*> account,
		std::shared_ptr<MTP::details::RSAPublicKey> key) {
	const auto configured = account->mtp().dcOptions().setCustomServer(
		MTP::CustomServer{
			.dcId = 2,
			.ip = "127.0.0.1",
			.port = 8443,
			.key = std::move(key),
		});
	return configured && account->local().writeMtpConfig(true);
}

[[nodiscard]] MTP::AuthKeyPtr RegressionAuthKey(int byte) {
	auto data = MTP::AuthKey::Data();
	std::fill(
		data.begin(),
		data.end(),
		static_cast<gsl::byte>(byte));
	return std::make_shared<MTP::AuthKey>(
		MTP::AuthKey::Type::Generated,
		2,
		data);
}

[[nodiscard]] bool HasAuthKey(
		not_null<Main::Account*> account,
		MTP::AuthKey::KeyId keyId) {
	const auto keys = account->mtp().getKeysForWrite();
	return std::any_of(keys.begin(), keys.end(), [=](const auto &key) {
		return key->keyId() == keyId;
	});
}

[[nodiscard]] bool HasNoAuthorizationState(
		not_null<Main::Account*> account) {
	return account->mtp().getKeysForWrite().empty()
		&& !account->sessionExists()
		&& (account->willHaveSessionUniqueId(nullptr) == 0);
}

[[nodiscard]] bool RestartDomain(
		Main::Domain &domain,
		LifecycleWriteCountsForRegressionTest *teardownWriteCounts = nullptr) {
	auto &app = Core::App();
	const auto applicationWindows = [&] {
		auto result = std::vector<Window::Controller *>();
		for (const auto widget : QApplication::topLevelWidgets()) {
			if (const auto window = app.findWindow(widget)) {
				if (std::find(
						result.begin(),
						result.end(),
						window) == result.end()) {
					result.push_back(window);
				}
			}
		}
		return result;
	};
	const auto windows = applicationWindows();
	for (const auto window : windows) {
		app.closeWindow(window);
	}
	if (!applicationWindows().empty()) {
		return false;
	}
	domain.local().writeAccounts();
	domain.finish();
	if (teardownWriteCounts) {
		*teardownWriteCounts = GetLifecycleWriteCountsForRegressionTest();
	}
	Storage::details::Sync();
	if (domain.start(QByteArray()) != Storage::StartResult::Success
		|| domain.accounts().empty()) {
		return false;
	}
	app.createPrimaryWindowForLifecycleRegression();
	const auto primary = app.activePrimaryWindow();
	if (!primary) {
		return false;
	}
	const auto active = &domain.active();
	if (primary->id().account != active) {
		primary->showAccount(active);
	}
	return &primary->account() == active;
}

[[nodiscard]] int FailAccountLifecycleRegression(const char *reason) {
	std::fprintf(
		stderr,
		"Account lifecycle regression failed: %s\n",
		reason);
	return 1;
}

[[nodiscard]] bool TeagramIconChoicePersistsAcrossSettingsReload() {
	auto settings = Core::Settings();
	if (Core::ReadTeagramIconChoice(settings)
		!= Core::TeagramIconChoice::MugSignal) {
		return false;
	}
	auto legacy = Core::Settings();
	legacy.writePref<bool>(
		Core::kLegacyTeagramIconChoicePreference,
		true);
	auto legacyReloaded = Core::Settings();
	legacyReloaded.addFromSerialized(legacy.serialize());
	if (Core::ReadTeagramIconChoice(legacyReloaded)
		!= Core::TeagramIconChoice::TPrimary) {
		return false;
	}
	for (const auto choice : {
			Core::TeagramIconChoice::TPrimary,
			Core::TeagramIconChoice::MugSignal,
			Core::TeagramIconChoice::MugGreen,
			Core::TeagramIconChoice::MugCrimson,
			Core::TeagramIconChoice::TCrimson,
			Core::TeagramIconChoice::MugBrown,
			Core::TeagramIconChoice::TBrown,
		}) {
		Core::WriteTeagramIconChoice(settings, choice);
		auto reloaded = Core::Settings();
		reloaded.addFromSerialized(settings.serialize());
		if (Core::ReadTeagramIconChoice(reloaded) != choice) {
			return false;
		}
	}
	std::fprintf(stderr, "Teagram icon choice persistence regression passed.\n");
	return true;
}

[[nodiscard]] MTPUser RegressionUser(
	UserId id,
	bool self,
	const QString &phone);

[[nodiscard]] bool CacheMismatchWaitsForDestructiveConfirmation(
		not_null<Main::Account*> account,
		std::shared_ptr<MTP::details::RSAPublicKey> candidateKey,
		UserId candidateUserId,
		uint64 retainedFingerprint,
		uint64 retainedUserId) {
	if (!ConfigurePinnedServer(account, candidateKey)) {
		return false;
	}
	account->setSessionUserId(candidateUserId);
	if (account->createSession(
			RegressionUser(candidateUserId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		return false;
	}
	const auto candidateFingerprint = candidateKey->fingerprint();
	if (account->sessionExists()
		|| !account->serverCacheBindingMismatchPending()
		|| account->mtp().isServerEnrollmentNetworkAllowed()
		|| account->local().checkServerCacheBinding(
			retainedFingerprint,
			retainedUserId) != Storage::ServerCacheBindingStatus::Match
		|| account->local().checkServerCacheBinding(
			candidateFingerprint,
			candidateUserId.bare)
			!= Storage::ServerCacheBindingStatus::Mismatch) {
		return false;
	}
	if (account->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::IdentityChange,
			true)
		|| account->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::DestructiveConfirmation,
			false)
		|| account->local().serverReenrollmentPending()) {
		return false;
	}
	return account->local().checkServerCacheBinding(
		retainedFingerprint,
		retainedUserId) == Storage::ServerCacheBindingStatus::Match;
}

template <typename Result, typename Start>
[[nodiscard]] std::optional<Result> AwaitCacheCallback(
	Start start,
	int timeoutMs = 5000) {
	struct State {
		std::optional<Result> result;
		QEventLoop *loop = nullptr;
		bool active = true;
		bool completed = false;
	};
	const auto state = std::make_shared<State>();
	auto loop = QEventLoop();
	const auto application = QCoreApplication::instance();
	if (!application) {
		return std::nullopt;
	}
	state->loop = &loop;
	QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
	// The callback only posts owned state; the queued handler touches the loop
	// while it is active on this thread.
	start([state, application](Result result) mutable {
		QMetaObject::invokeMethod(
			application,
			[state, result = std::move(result)]() mutable {
				if (!state->active || state->completed || !state->loop) {
					return;
				}
				state->result.emplace(std::move(result));
				state->completed = true;
				state->loop->quit();
			},
			Qt::QueuedConnection);
	});
	loop.exec();
	state->active = false;
	state->loop = nullptr;
	return std::move(state->result);
}

[[nodiscard]] bool CacheCallbackTimeoutIsSafe() {
	const auto failure = AwaitCacheCallback<int>([](auto done) {
		done(-1);
	});
	if (!failure || (*failure != -1)) {
		return false;
	}
	auto release = std::promise<void>();
	const auto waitForRelease = release.get_future().share();
	auto delayed = std::thread();
	const auto timedOut = AwaitCacheCallback<int>([&](auto done) {
		delayed = std::thread([done = std::move(done), waitForRelease]() mutable {
			waitForRelease.wait();
			done(1);
		});
	}, 1);
	release.set_value();
	delayed.join();
	QCoreApplication::processEvents();
	return !timedOut;
}

[[nodiscard]] bool CacheOperationSucceeded(
		const std::optional<Storage::Cache::Error> &result) {
	return result
		&& (result->type == Storage::Cache::Error::Type::None);
}

[[nodiscard]] bool OpenCache(
		Storage::Cache::Database &cache,
		not_null<Main::Account*> account) {
	return CacheOperationSucceeded(AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache.open(account->local().cacheKey(), std::move(done));
		}));
}

[[nodiscard]] bool WriteCachePayload(
		not_null<Main::Account*> account,
		const Storage::Cache::Key &key,
		const QByteArray &payload) {
	auto cache = Core::App().databases().get(
		account->local().cachePath(),
		account->local().cacheSettings());
	if (!OpenCache(*cache, account)) {
		return false;
	}
	return CacheOperationSucceeded(AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache->put(key, QByteArray(payload), std::move(done));
		}));
}

[[nodiscard]] bool CachePayloadMatches(
		Storage::Cache::Database &cache,
		const Storage::Cache::Key &key,
		const QByteArray &expected) {
	const auto payload = AwaitCacheCallback<QByteArray>([&](auto done) {
		cache.get(key, std::move(done));
	});
	return payload && (*payload == expected);
}

[[nodiscard]] bool PersistedCachePayloadMatches(
		not_null<Main::Account*> account,
		const Storage::Cache::Key &key,
		const QByteArray &expected) {
	auto cache = Core::App().databases().get(
		account->local().cachePath(),
		account->local().cacheSettings());
	return OpenCache(*cache, account)
		&& CachePayloadMatches(*cache, key, expected);
}

using CacheGetter = Fn<Storage::Cache::Database &()>;
using CacheClearer = Fn<void()>;
using CachePointerGetter = Fn<Storage::Cache::Database *()>;
using RefusedCacheReaderRegression = Fn<bool()>;

class RefusedCacheLoader final : public Media::Streaming::Loader {
  public:
	static constexpr auto kSize = int64(81) * Loader::kPartSize;

	Storage::Cache::Key baseCacheKey() const override {
		return {0x5245465553454443ULL, 0x4143484554455354ULL};
	}

	int64 size() const override { return kSize; }

	void load(int64 offset) override {
		QMutexLocker lock(&_mutex);
		_loaded.emplace(offset);
	}

	void cancel(int64) override {}

	void resetPriorities() override {}

	void setPriority(int) override {}

	void stop() override {}

	void tryRemoveFromQueue() override {}

	rpl::producer<Media::Streaming::LoadedPart> parts() const override {
		return _parts.events();
	}

	rpl::producer<Media::Streaming::SpeedEstimate>
	speedEstimate() const override {
		return _speed.events();
	}

	void
	attachDownloader(not_null<Storage::StreamedFileDownloader *>) override {}

	void clearAttachedDownloader() override {}

	[[nodiscard]] bool loaded(int64 offset) const {
		QMutexLocker lock(&_mutex);
		return _loaded.contains(offset);
	}

  private:
	mutable QMutex _mutex;
	std::set<int64> _loaded;
	rpl::event_stream<Media::Streaming::LoadedPart> _parts;
	rpl::event_stream<Media::Streaming::SpeedEstimate> _speed;
};

[[nodiscard]] bool
RunRefusedCacheStreamingRegression(CachePointerGetter cache) {
	auto loader = std::make_unique<RefusedCacheLoader>();
	auto rawLoader = loader.get();
	auto reader = std::make_shared<Media::Streaming::Reader>(std::move(loader),
															 std::move(cache));
	auto notify = std::make_shared<crl::semaphore>();
	struct Result {
		std::atomic<bool> completed = false;
		std::atomic<bool> cancelled = false;
		Media::Streaming::Reader::FillState state
			= Media::Streaming::Reader::FillState::WaitingCache;
	};
	const auto result = std::make_shared<Result>();
	auto entered = QSemaphore();
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	auto streaming = std::thread([=, &entered, &loop] {
		auto buffer = QByteArray(1, Qt::Uninitialized);
		auto state
			= reader->fill(0, bytes::make_detached_span(buffer), notify.get());
		entered.release();
		while ((state != Media::Streaming::Reader::FillState::Failed)
			   && !rawLoader->loaded(0)
			   && !result->cancelled.load(std::memory_order_acquire)) {
			notify->acquire();
			if (result->cancelled.load(std::memory_order_acquire)) {
				break;
			}
			state = reader->fill(0, bytes::make_detached_span(buffer),
								 notify.get());
		}
		result->state = state;
		result->completed.store(true, std::memory_order_release);
		QMetaObject::invokeMethod(
			&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
	});
	entered.acquire();
	if (!result->completed.load(std::memory_order_acquire)) {
		timeout.start(5000);
		loop.exec();
	}
	const auto completed = result->completed.load(std::memory_order_acquire);
	if (!completed) {
		result->cancelled.store(true, std::memory_order_release);
		notify->release();
	}
	streaming.join();
	reader->stopStreaming();
	return completed
		   && (result->state
			   == Media::Streaming::Reader::FillState::WaitingRemote)
		   && rawLoader->loaded(0);
}

[[nodiscard]] bool
RunRefusedCacheDownloaderRegression(not_null<Main::Account *> account,
									CachePointerGetter cache) {
	auto loader = std::make_unique<RefusedCacheLoader>();
	auto rawLoader = loader.get();
	auto reader = std::make_shared<Media::Streaming::Reader>(std::move(loader),
															 std::move(cache));
	auto downloader = std::make_unique<Storage::StreamedFileDownloader>(
		not_null<Main::Session *>(&account->session()), 1, 2,
		Data::FileOrigin(), Storage::Cache::Key{1, 2}, MediaKey{0, 0}, reader,
		Core::MacProtectedPath::ProfileRoot()
			+ u"tdata/refused_cache_download"_q,
		RefusedCacheLoader::kSize, DocumentFileLocation, LoadToFileOnly,
		LoadFromCloudOrLocal, false, 0);
	auto loop = QEventLoop();
	auto timeout = QTimer();
	auto poll = QTimer();
	timeout.setSingleShot(true);
	poll.setInterval(10);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (rawLoader->loaded(0)) {
			loop.quit();
		}
	});
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	reader->loadForDownloader(downloader.get(), 0);
	reader->continueDownloaderFromMainThread();
	if (!rawLoader->loaded(0)) {
		poll.start();
		timeout.start(5000);
		loop.exec();
	}
	poll.stop();
	return rawLoader->loaded(0);
}

[[nodiscard]] bool
RunRefusedCacheReaderRegressions(not_null<Main::Account *> account,
								 bool mediaCache) {
	const auto cache = [=] {
		auto &data = account->session().data();
		return mediaCache ? data.cacheBigFileIfAllowed()
						  : data.cacheIfAllowed();
	};
	const auto streaming = RunRefusedCacheStreamingRegression(cache);
	const auto downloading
		= RunRefusedCacheDownloaderRegression(account, cache);
	return streaming && downloading;
}

[[nodiscard]] bool RunCacheConcurrentDeletionRegression() {
#if defined(Q_OS_MAC) && defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	if (!Core::MacProtectedPath::IntegrationTestActive()) {
		return true;
	}
	const auto parent
		= QDir(Core::MacProtectedPath::ProfileRoot()).filePath("tdata");
	if (!QDir().mkpath(parent)) {
		return false;
	}
	auto directory
		= QTemporaryDir(QDir(parent).filePath("cache-scan-deletion-XXXXXX"));
	const auto vanished = QDir(directory.path()).filePath("vanished");
	if (!directory.isValid()
		|| !WriteFixtureFile(vanished, "synthetic cache entry")) {
		return false;
	}
	auto deleted = false;
	const auto allowed = Core::MacProtectedPath::CheckCachePathForTesting(
		directory.path(), "Tests::ProtectedCache::concurrent-deletion",
		[&](const QString &entry) {
			if (entry != vanished) {
				return;
			}
			auto remove = std::thread([&] { deleted = QFile::remove(entry); });
			remove.join();
		});
	return allowed && deleted && !QFileInfo::exists(vanished);
#else
	return true;
#endif
}

[[nodiscard]] QString ActiveCacheVersionPath(const QString &path) {
	const auto directories
		= QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const auto &directory : directories) {
		const auto version = QDir(path).filePath(directory);
		if (QFileInfo(version + "/binlog").isFile()) {
			return version;
		}
	}
	return {};
}

[[nodiscard]] bool CopyCacheVersionFiles(const QString &source,
										 const QString &destination) {
	const auto files = QDir(source).entryList(
		QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
	for (const auto &file : files) {
		if (file != "marker"
			&& !QFile::copy(QDir(source).filePath(file),
							QDir(destination).filePath(file))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool FixtureMarkerIsIntact(const QString &path) {
	auto marker = QFile(path + "/marker");
	return marker.open(QIODevice::ReadOnly)
		   && marker.readAll() == "synthetic protected fixture";
}

[[nodiscard]] bool
RunPostOpenCacheSymlinkRegression(const QString &path, const QString &fixture,
								  CacheGetter cache, CacheClearer clearCaches,
								  RefusedCacheReaderRegression cacheRefusal) {
	const auto seedKey = Storage::Cache::Key{
		0x4d41494e39353731ULL,
		0x4341434845534545ULL,
	};
	const auto writeKey = Storage::Cache::Key{
		0x4d41494e39353732ULL,
		0x4341434845575249ULL,
	};
	const auto payload = QByteArray("post-open cache guard seed");
	const auto seed = AwaitCacheCallback<Storage::Cache::Error>([&](auto done) {
		cache().put(seedKey, QByteArray(payload), std::move(done));
	});
	if (!CacheOperationSucceeded(seed)) {
		std::fprintf(stderr,
					 "Post-open cache setup failed: seed error=%d path=%s.\n",
					 seed ? int(seed->type) : -1, qPrintable(path));
		return false;
	}
	cache().sync();

	const auto active = ActiveCacheVersionPath(path);
	const auto saved = active + ".guard-test-original";
	if (active.isEmpty() || QFileInfo::exists(saved)
		|| !FixtureMarkerIsIntact(fixture)
		|| !CopyCacheVersionFiles(active, fixture)
		|| !QDir().rename(active, saved)) {
		return false;
	}
	auto restored = false;
	const auto restore = gsl::finally([&] {
		if (!restored) {
			QFile::remove(active);
			QDir().rename(saved, active);
		}
	});
	if (!QFile::link(fixture, active)) {
		return false;
	}
	const auto fixtureEntries = QDir(fixture).entryList(
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
		QDir::Name);
	const auto readerRefused = cacheRefusal();
	const auto read = AwaitCacheCallback<QByteArray>(
		[&](auto done) { cache().get(seedKey, std::move(done)); });
	const auto readRefused = read && read->isEmpty();
	const auto write = AwaitCacheCallback<Storage::Cache::Error>(
		[&](auto done) {
			cache().put(writeKey, QByteArray("must not reach protected cache"),
						std::move(done));
		});
	clearCaches();
	cache().sync();
	const auto unchanged
		= (fixtureEntries
		   == QDir(fixture).entryList(QDir::AllEntries | QDir::Hidden
										  | QDir::System | QDir::NoDotAndDotDot,
									  QDir::Name))
		  && FixtureMarkerIsIntact(fixture);
	const auto writeRefused
		= write && (write->type != Storage::Cache::Error::Type::None);
	const auto aliasRemoved = QFile::remove(active);
	const auto originalRestored = QDir().rename(saved, active);
	restored = aliasRemoved && originalRestored;
	cache().sync();
	const auto passed
		= readerRefused && readRefused && writeRefused && unchanged && restored;
	if (!passed) {
		std::fprintf(
			stderr,
			"Post-open cache regression failed: reader=%d read=%d write=%d "
			"fixture=%d restored=%d path=%s.\n",
			readerRefused, readRefused, writeRefused, unchanged, restored,
			qPrintable(path));
	}
	return passed;
}

[[nodiscard]] Main::Account *FindAuthorizationBlockedAccount(
		const Main::Domain &domain) {
	for (const auto &[index, account] : domain.accounts()) {
		if (account->local().mtpAuthorizationWriteFailed()) {
			return account.get();
		}
	}
	return nullptr;
}

[[nodiscard]] MTPUser RegressionUser(
		UserId id,
		bool self,
		const QString &phone) {
	const auto flags = (self
		? MTPDuser::Flag::f_self
		: MTPDuser::Flag())
		| (phone.isEmpty()
			? MTPDuser::Flag()
			: MTPDuser::Flag::f_phone);
	return MTP_user(
		MTP_flags(flags),
		MTP_long(id.bare),
		MTPlong(),
		MTP_string(u"Regression"_q),
		MTPstring(),
		MTPstring(),
		MTP_string(phone),
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTPint(),
		MTP_vector<MTPRestrictionReason>(0),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTP_vector<MTPUsername>(0),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong());
}

[[nodiscard]] MTPmessages_ChatFull RegressionChatFullReply(
		ChatId chatId,
		int version,
		QString selfPhone,
		UserId creatorId) {
	const auto memberId = (creatorId == UserId(1))
		? UserId(2)
		: UserId(1);
	const auto participants = MTP_chatParticipants(
		MTP_long(chatId.bare),
		MTP_vector<MTPChatParticipant>({
			MTP_chatParticipantCreator(
				MTP_flags(MTPDchatParticipantCreator::Flags()),
				MTP_long(creatorId.bare),
				MTP_string(QString())),
			MTP_chatParticipant(
				MTP_flags(MTPDchatParticipant::Flags()),
				MTP_long(memberId.bare),
				MTP_long(creatorId.bare),
				MTP_int(0),
				MTP_string(QString())),
			MTP_chatParticipant(
				MTP_flags(MTPDchatParticipant::Flags()),
				MTP_long(UserId(9).bare),
				MTP_long(UserId(1).bare),
				MTP_int(0),
				MTP_string(QString())),
		}),
		MTP_int(version));
	const auto notifySettings = MTP_peerNotifySettings(
		MTP_flags(MTPDpeerNotifySettings::Flags()),
		MTP_boolFalse(),
		MTP_boolFalse(),
		MTP_int(0),
		MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(),
		MTP_boolFalse(),
		MTP_boolFalse(),
		MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault(),
		MTP_notificationSoundDefault());
	const auto fullChat = MTP_chatFull(
		MTP_flags(MTPDchatFull::Flags()),
		MTP_long(chatId.bare),
		MTP_string(QString()),
		participants,
		MTP_photoEmpty(MTP_long(0)),
		notifySettings,
		MTPExportedChatInvite(),
		MTP_vector<MTPBotInfo>(0),
		MTPint(),
		MTPint(),
		MTPInputGroupCall(),
		MTPint(),
		MTPPeer(),
		MTPstring(),
		MTPint(),
		MTP_vector<MTPlong>(0),
		MTP_chatReactionsNone(),
		MTPint());
	return MTP_messages_chatFull(
		fullChat,
		MTP_vector<MTPChat>(0),
		MTP_vector<MTPUser>({
			RegressionUser(UserId(1), true, selfPhone),
			RegressionUser(UserId(2), false, u"2"_q),
			RegressionUser(UserId(9), false, u"9"_q),
		}));
}

[[nodiscard]] bool HasParticipant(
		not_null<ChatData*> chat,
		UserId id) {
	return std::any_of(
		chat->participants.begin(),
		chat->participants.end(),
		[id](not_null<UserData*> user) {
			return peerToUser(user->id) == id;
		});
}

[[nodiscard]] bool HasExpectedParticipants(
		not_null<ChatData*> chat,
		UserId creatorId) {
	return (chat->participants.size() == 3)
		&& (chat->creator == creatorId)
		&& HasParticipant(chat, UserId(1))
		&& HasParticipant(chat, UserId(2))
		&& HasParticipant(chat, UserId(9));
}

[[nodiscard]] int FailChatParticipantsRegression(const char *reason) {
	std::fprintf(
		stderr,
		"Chat participants regression failed: %s\n",
		reason);
	return 1;
}

[[nodiscard]] int
StartChatParticipantsRegression(Main::Domain &domain,
								const ProtectedCacheFixtures &fixtures,
								Fn<void(int)> done) {
	if (Core::MacProtectedPath::IntegrationTestActive()
		&& !RunCacheConcurrentDeletionRegression()) {
		return FailChatParticipantsRegression(
			"cache scan rejected an entry removed after enumeration");
	}
	const auto requiredAccounts
		= Core::MacProtectedPath::IntegrationTestActive() ? 3 : 2;
	if (domain.accounts().size()
		> Main::Domain::kPremiumMaxAccounts - requiredAccounts) {
		return FailChatParticipantsRegression(
			"not enough account slots for isolated cache regression sessions");
	}
	const auto selfId = UserId(1);
	const auto chatId = ChatId(1051);
	const auto stock = domain.add(MTP::Environment::Production);
	stock->mtp().stopForServerEnrollment();
	stock->setSessionUserId(selfId);
	const auto stockCachePath = stock->local().cachePath();
	const auto stockMediaCachePath = stock->local().cacheBigFilePath();
	const auto refusedDownload = FullMsgId(peerFromUser(selfId), MsgId(12345));
	if (!StoreRefusedDownloadHistory(stock, fixtures.cacheRoot + "/marker",
									 refusedDownload)) {
		return FailChatParticipantsRegression(
			"could not persist the refused download-history fixture");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()
		&& (!QDir().mkpath(QFileInfo(stockCachePath).absolutePath())
			|| !QDir().mkpath(QFileInfo(stockMediaCachePath).absolutePath())
			|| !QFile::link(fixtures.cacheRoot, stockCachePath)
			|| !QFile::link(fixtures.mediaCacheRoot, stockMediaCachePath))) {
		return FailChatParticipantsRegression(
			"could not create protected cache-root symlink fixtures");
	}
	if (!stock->createSession(
			RegressionUser(selfId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		return FailChatParticipantsRegression(
			"could not create the stock test session");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		if (!RunRefusedDownloadHistoryRegression(&stock->session(),
												 refusedDownload)) {
			return FailChatParticipantsRegression(
				"refused persisted download history was published");
		}
		std::fprintf(
			stderr, "Authenticated refused download history regression passed: "
					"entry without a live message remained unpublished.\n");
		stock->session().data().cache().sync();
		stock->session().data().cacheBigFile().sync();
		if (Core::MacProtectedPath::CheckCachePath(
				stockCachePath, "Tests::ProtectedCache::root")
			|| Core::MacProtectedPath::CheckCachePath(
				stockMediaCachePath, "Tests::ProtectedCache::media-root")
			|| !FixtureContainsOnlyMarker(fixtures.cacheRoot)
			|| !FixtureContainsOnlyMarker(fixtures.mediaCacheRoot)) {
			return FailChatParticipantsRegression(
				"authenticated session accessed a protected cache root");
		}
	}
	const auto stockChat = stock->session().data().chat(chatId);
	const auto stockPeer = not_null<PeerData*>(
		static_cast<PeerData*>(&*stockChat));
	stock->session().api().processFullPeer(
		stockPeer,
		RegressionChatFullReply(chatId, 1, QString(), UserId(2)));
	if (stock->session().user()->isLoaded()
		|| !stockChat->participants.empty()) {
		return FailChatParticipantsRegression(
			"stock session accepted phone-free self in full chat info");
	}
	stock->session().api().processFullPeer(
		stockPeer,
		RegressionChatFullReply(chatId, 2, u"+10000000001"_q, UserId(2)));
	if (!stock->session().user()->isLoaded()
		|| !HasExpectedParticipants(stockChat, UserId(2))) {
		return FailChatParticipantsRegression(
			"stock session with phone did not load creator and members");
	}

	const auto pinned = domain.add(MTP::Environment::Production);
	pinned->mtp().stopForServerEnrollment();
	if (!ConfigurePinnedServer(pinned, RegressionServerKey())) {
		return FailChatParticipantsRegression(
			"could not pin the custom test server");
	}
	pinned->setSessionUserId(selfId);
	const auto pinnedCachePath = pinned->local().cachePath();
	const auto pinnedMediaCachePath = pinned->local().cacheBigFilePath();
	if (Core::MacProtectedPath::IntegrationTestActive()
		&& (!QDir().mkpath(pinnedCachePath)
			|| !QDir().mkpath(pinnedMediaCachePath)
			|| !QFile::link(fixtures.cacheLeaf, pinnedCachePath + "/0")
			|| !QFile::link(fixtures.cacheLeaf + "/marker",
							pinnedCachePath + "/version")
			|| !QFile::link(fixtures.mediaCacheLeaf,
							pinnedMediaCachePath + "/0")
			|| !QFile::link(fixtures.mediaCacheLeaf + "/marker",
							pinnedMediaCachePath + "/version"))) {
		return FailChatParticipantsRegression(
			"could not create protected cache-file symlink fixtures");
	}
	if (!pinned->createSession(
			RegressionUser(selfId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		return FailChatParticipantsRegression(
			"could not create the pinned test session");
	}
	if (stock->session().uniqueId() != pinned->session().uniqueId()) {
		return FailChatParticipantsRegression(
			"stock and pinned sessions did not share the same user id");
	}
	const auto capabilitiesMatch = [](const Main::Session &session,
								  bool supported) {
		return (session.callsSupported() == supported)
			&& (session.botAppsSupported() == supported)
			&& (session.paidFeaturesSupported() == supported)
			&& (session.storiesSupported() == supported)
			&& (session.exportSupported() == supported)
			&& (session.passportSupported() == supported)
			&& (session.aiComposeSupported() == supported)
			&& (session.serverTranslationSupported() == supported);
	};
	auto &app = Core::App();
	pinned->mtp().stopForServerEnrollment();
	const auto primary = app.activePrimaryWindow();
	if (!primary || !primary->isPrimary()) {
		return FailChatParticipantsRegression(
			"primary window disappeared before online-update lifetime regression");
	}
	primary->showAccount(stock);
	if (primary->maybeSession() != &stock->session()) {
		return FailChatParticipantsRegression(
			"primary window did not switch to the stock session");
	}
	QCoreApplication::processEvents();
	const auto stockToPinnedStockUpdates
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto stockToPinnedPinnedUpdates
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	primary->showAccount(pinned);
	if (primary->maybeSession() != &pinned->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockToPinnedStockUpdates
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != stockToPinnedPinnedUpdates + 1) {
		return FailChatParticipantsRegression(
			"stock-to-pinned switch did not update only the shown session inline");
	}
	QCoreApplication::processEvents();
	if (stock->session().updates().sessionSwitchUpdatesForTest()
			!= stockToPinnedStockUpdates + 1
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != stockToPinnedPinnedUpdates + 1) {
		return FailChatParticipantsRegression(
			"stock-to-pinned switch did not update each session exactly once");
	}
	const auto pinnedToStockStockUpdates
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedToStockPinnedUpdates
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	primary->showAccount(stock);
	if (primary->maybeSession() != &stock->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedToStockStockUpdates + 1
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedToStockPinnedUpdates) {
		return FailChatParticipantsRegression(
			"pinned-to-stock switch did not update only the shown session inline");
	}
	QCoreApplication::processEvents();
	const auto stockAfterPinnedToStock
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedAfterPinnedToStock
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	if (stockAfterPinnedToStock != pinnedToStockStockUpdates + 1
		|| pinnedAfterPinnedToStock != pinnedToStockPinnedUpdates + 1) {
		std::fprintf(
			stderr,
			"Pinned-to-stock session-switch updates: stock=%d expected=%d "
			"pinned=%d expected=%d\n",
			stockAfterPinnedToStock, pinnedToStockStockUpdates + 1,
			pinnedAfterPinnedToStock, pinnedToStockPinnedUpdates + 1);
		return FailChatParticipantsRegression(
			"pinned-to-stock switch did not update each session exactly once");
	}
	const auto discarded = domain.add(MTP::Environment::Production);
	discarded->mtp().stopForServerEnrollment();
	discarded->setSessionUserId(selfId);
	if (!discarded->createSession(
			RegressionUser(selfId, true, QString()),
			std::make_unique<Main::SessionSettings>())) {
		return FailChatParticipantsRegression(
			"could not create the previous-session teardown fixture");
	}
	if (discarded->session().uniqueId() != stock->session().uniqueId()) {
		return FailChatParticipantsRegression(
			"previous-session teardown fixture did not share the stock user id");
	}
	const auto stockBeforeQueuedSwitches
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedBeforeQueuedSwitches
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	const auto discardedBeforeQueuedSwitches
		= discarded->session().updates().sessionSwitchUpdatesForTest();
	primary->showAccount(pinned);
	if (primary->maybeSession() != &pinned->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockBeforeQueuedSwitches
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeQueuedSwitches + 1
		|| discarded->session().updates().sessionSwitchUpdatesForTest()
			   != discardedBeforeQueuedSwitches) {
		return FailChatParticipantsRegression(
			"queued stock-to-pinned switch missed its inline session update");
	}
	primary->showAccount(discarded);
	if (primary->maybeSession() != &discarded->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockBeforeQueuedSwitches
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeQueuedSwitches + 1
		|| discarded->session().updates().sessionSwitchUpdatesForTest()
			   != discardedBeforeQueuedSwitches + 1) {
		return FailChatParticipantsRegression(
			"queued pinned-to-teardown switch missed its inline session update");
	}
	primary->showAccount(stock);
	if (primary->maybeSession() != &stock->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockBeforeQueuedSwitches + 1
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeQueuedSwitches + 1
		|| discarded->session().updates().sessionSwitchUpdatesForTest()
			   != discardedBeforeQueuedSwitches + 1) {
		return FailChatParticipantsRegression(
			"queued teardown-to-stock switch missed its inline session update");
	}
	discarded->forcedLogOut();
	if (discarded->sessionExists()) {
		return FailChatParticipantsRegression(
			"previous-session teardown fixture was not destroyed");
	}
	QCoreApplication::processEvents();
	if (stock->session().updates().sessionSwitchUpdatesForTest()
			!= stockBeforeQueuedSwitches + 2
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeQueuedSwitches + 2) {
		return FailChatParticipantsRegression(
			"queued live and destroyed-session updates reached the wrong sessions");
	}
	auto pinnedWindow = app.ensureSeparateWindowFor(pinned);
	if (app.separateWindowFor(pinned) != pinnedWindow) {
		return FailChatParticipantsRegression(
			"pinned window was not mapped before the primary closed");
	}
	if (!stock->sessionExists() || !pinned->sessionExists()) {
		return FailChatParticipantsRegression(
			"live previous-session fixtures disappeared before primary close");
	}
	// The pinned account owns a window, so the pinned id is taken. Switching
	// the primary here must not rebind it: the pinned window is shown and the
	// primary stays on the stock account. Every account lookup must keep
	// returning a window bound to the account it was asked for, and every
	// window must stay registered under its own id.
	const auto stockBeforeCollision
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedBeforeCollision
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	primary->showAccount(pinned);
	const auto stockLookup = app.windowFor(stock);
	const auto pinnedLookup = app.windowFor(pinned);
	if (primary->maybeSession() != &stock->session()
		|| app.activePrimaryWindow() != pinnedWindow || stockLookup != primary
		|| pinnedLookup != pinnedWindow
		|| &stockLookup->account() != stock.get()
		|| &pinnedLookup->account() != pinned.get()
		|| app.separateWindowFor(stock) != primary
		|| app.separateWindowFor(pinned) != pinnedWindow
		|| app.separateWindowFor(primary->id()) != primary
		|| app.separateWindowFor(pinnedWindow->id()) != pinnedWindow
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockBeforeCollision
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeCollision) {
		return FailChatParticipantsRegression(
			"primary switch to an account owning a window left an account "
			"lookup on a window bound to another account");
	}
	QCoreApplication::processEvents();
	// Free the pinned id, then run the queued-switch teardown on a switch that
	// is allowed: the previous-session update is queued for the stock session,
	// the originating window closes synchronously before the dispatch, and the
	// queued update must reach the stock session once, with no deferred update
	// of its own for the newly shown session.
	app.closeWindow(pinnedWindow);
	if (app.separateWindowFor(pinned) != nullptr
		|| app.separateWindowFor(stock) != primary
		|| app.activePrimaryWindow() != primary) {
		return FailChatParticipantsRegression(
			"closed account window stayed mapped to the pinned account");
	}
	// A window for a brand-new account, which has no session, keeps one window
	// registered while the originating window closes. A new account cannot have
	// a window yet, so its id is free, and with no session behind it that window
	// cannot answer an online update: the stock count dispatched below is
	// the queued previous-session update and nothing else.
	const auto blank = domain.add(MTP::Environment::Production);
	blank->mtp().stopForServerEnrollment();
	const auto blankWindow = app.ensureSeparateWindowFor(blank);
	if (app.separateWindowFor(blank) != blankWindow
		|| blankWindow->sessionController() != nullptr) {
		std::fprintf(
			stderr,
			"Blank window fixture: mapped=%d controller=%p\n",
			app.separateWindowFor(blank) == blankWindow,
			static_cast<void *>(blankWindow->sessionController()));
		return FailChatParticipantsRegression(
			"blank window fixture was not mapped before the close");
	}
	const auto stockBeforeCloseSwitch
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedBeforeCloseSwitch
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	primary->showAccount(pinned);
	if (primary->maybeSession() != &pinned->session()
		|| stock->session().updates().sessionSwitchUpdatesForTest()
			   != stockBeforeCloseSwitch
		|| pinned->session().updates().sessionSwitchUpdatesForTest()
			   != pinnedBeforeCloseSwitch + 1) {
		return FailChatParticipantsRegression(
			"stock-to-pinned close switch missed its inline session update");
	}
	// The switch is registered under the pinned id, not the stock one it came
	// from, so no lookup can answer with a window of another account.
	if (app.separateWindowFor(pinned) != primary
		|| app.separateWindowFor(stock) != nullptr
		|| app.separateWindowFor(primary->id()) != primary
		|| &primary->account() != pinned.get()) {
		return FailChatParticipantsRegression(
			"allowed primary switch kept the window keyed to its old account");
	}
	// The close is synchronous, before any dispatch of the queued update.
	app.closeWindow(primary);
	if (app.separateWindowFor(pinned) != nullptr
		|| app.separateWindowFor(stock) != nullptr
		|| app.separateWindowFor(blank) != blankWindow) {
		return FailChatParticipantsRegression(
			"closed primary window remained mapped to an account");
	}
	const auto stockAfterClose
		= stock->session().updates().sessionSwitchUpdatesForTest();
	const auto pinnedAfterClose
		= pinned->session().updates().sessionSwitchUpdatesForTest();
	QCoreApplication::processEvents();
	const auto stockDelivered
		= stock->session().updates().sessionSwitchUpdatesForTest()
		  - stockAfterClose;
	const auto pinnedDelivered
		= pinned->session().updates().sessionSwitchUpdatesForTest()
		  - pinnedAfterClose;
	// The stock channel is clean at the dispatch: no window is bound to the
	// stock session then, so its one update is the queued previous-session
	// update of the closed switch, never a second one. The pinned session gains
	// at most the update that closing the window which showed it produces
	// (MainWindow::handleActiveChanged), which is that window's own session.
	if (stockDelivered != 1 || pinnedDelivered > 1) {
		std::fprintf(
			stderr,
			"Deferred primary-close updates: stock=%d pinned=%d\n",
			stockDelivered,
			pinnedDelivered);
		return FailChatParticipantsRegression(
			"deferred primary-close update was not delivered once "
			"to the previous session");
	}
	pinnedWindow = app.ensureSeparateWindowFor(pinned);
	if (app.separateWindowFor(pinned) != pinnedWindow
		|| &pinnedWindow->account() != pinned.get()) {
		return FailChatParticipantsRegression(
			"pinned window was not remapped after the primary closed");
	}

	const auto stockWindow = app.ensureSeparateWindowFor(stock);
	const auto closeWindows = gsl::finally([&] {
		if (stockWindow && app.separateWindowFor(stock) == stockWindow) {
			app.closeWindow(stockWindow);
		}
		if (pinnedWindow && app.separateWindowFor(pinned) == pinnedWindow) {
			app.closeWindow(pinnedWindow);
		}
		if (blankWindow && app.separateWindowFor(blank) == blankWindow) {
			app.closeWindow(blankWindow);
		}
		domain.activate(stock);
	});
	const auto printCapabilities = [](const char *name,
								  const Main::Session &session) {
		std::fprintf(stderr,
			"%s capabilities=%d%d%d%d%d%d%d%d\n",
			name,
			session.callsSupported(),
			session.botAppsSupported(),
			session.paidFeaturesSupported(),
			session.storiesSupported(),
			session.exportSupported(),
			session.passportSupported(),
			session.aiComposeSupported(),
			session.serverTranslationSupported());
	};
	const auto windowsMatch = [&](const char *stage) {
		const auto stockController = stockWindow->sessionController();
		const auto pinnedController = pinnedWindow->sessionController();
		const auto stockMapped = app.separateWindowFor(stock) == stockWindow;
		const auto pinnedMapped = app.separateWindowFor(pinned) == pinnedWindow;
		const auto stockBound = &stockWindow->account() == stock.get();
		const auto pinnedBound = &pinnedWindow->account() == pinned.get();
		const auto stockSessionMatches = stockController
			&& (&stockController->session() == &stock->session());
		const auto pinnedSessionMatches = pinnedController
			&& (&pinnedController->session() == &pinned->session());
		const auto stockCapabilitiesMatch = stockController
			&& capabilitiesMatch(stockController->session(), true);
		const auto pinnedCapabilitiesMatch = pinnedController
			&& capabilitiesMatch(pinnedController->session(), false);
		const auto matches = (stockWindow != pinnedWindow)
			&& stockMapped
			&& pinnedMapped
			&& stockBound
			&& pinnedBound
			&& stockSessionMatches
			&& pinnedSessionMatches
			&& stockCapabilitiesMatch
			&& pinnedCapabilitiesMatch;
		if (!matches) {
			std::fprintf(
				stderr,
				"Window/session regression mismatch at %s: "
				"distinct=%d mapped=%d/%d account=%d/%d "
				"controller=%d/%d session=%d/%d gates=%d/%d active=%p\n",
				stage,
				stockWindow != pinnedWindow,
				stockMapped,
				pinnedMapped,
				stockBound,
				pinnedBound,
				stockController != nullptr,
				pinnedController != nullptr,
				stockSessionMatches,
				pinnedSessionMatches,
				stockCapabilitiesMatch,
				pinnedCapabilitiesMatch,
				static_cast<const void *>(&domain.active()));
			printCapabilities("stock account", stock->session());
			printCapabilities("pinned account", pinned->session());
			if (stockController) {
				printCapabilities("stock window", stockController->session());
			}
			if (pinnedController) {
				printCapabilities("pinned window", pinnedController->session());
			}
		}
		return matches;
	};
	const auto activateAndCheck = [&](bool customFirst) {
		const auto first = customFirst ? pinned : stock;
		const auto second = customFirst ? stock : pinned;
		domain.activate(first);
		const auto firstActive = &domain.active() == first.get();
		const auto firstCapabilities
			= capabilitiesMatch(first->session(), !customFirst);
		const auto firstWindows = windowsMatch(customFirst
			? "custom account first activation"
			: "stock account first activation");
		if (!firstActive || !firstCapabilities || !firstWindows) {
			std::fprintf(stderr,
				"First activation mismatch: customFirst=%d active=%d "
				"capabilities=%d windows=%d\n",
				customFirst, firstActive, firstCapabilities, firstWindows);
			if (!firstCapabilities) {
				printCapabilities(customFirst ? "pinned first" : "stock first",
					first->session());
			}
			return false;
		}
		domain.activate(second);
		const auto secondActive = &domain.active() == second.get();
		const auto secondCapabilities
			= capabilitiesMatch(second->session(), customFirst);
		const auto secondWindows = windowsMatch(customFirst
			? "stock account second activation"
			: "custom account second activation");
		if (!secondActive || !secondCapabilities || !secondWindows) {
			std::fprintf(stderr,
				"Second activation mismatch: customFirst=%d active=%d "
				"capabilities=%d windows=%d\n",
				customFirst, secondActive, secondCapabilities, secondWindows);
			if (!secondCapabilities) {
				printCapabilities(customFirst ? "stock second" : "pinned second",
					second->session());
			}
			return false;
		}
		return true;
	};
	if (!stockWindow || !pinnedWindow) {
		std::fprintf(stderr,
			"Separate window construction failed: stock=%p pinned=%p\n",
			static_cast<const void *>(stockWindow),
			static_cast<const void *>(pinnedWindow));
		return FailChatParticipantsRegression(
			"session feature capabilities crossed account or window boundaries");
	}
	if (!windowsMatch("initial separate windows")
		|| !activateAndCheck(false)
		|| !activateAndCheck(true)) {
		return FailChatParticipantsRegression(
			"session feature capabilities crossed account or window boundaries");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		pinned->session().data().cache().sync();
		pinned->session().data().cacheBigFile().sync();
		if (Core::MacProtectedPath::CheckCachePath(
				pinnedCachePath, "Tests::ProtectedCache::file")
			|| Core::MacProtectedPath::CheckCachePath(
				pinnedMediaCachePath, "Tests::ProtectedCache::media-file")
			|| !FixtureContainsOnlyMarker(fixtures.cacheLeaf)
			|| !FixtureContainsOnlyMarker(fixtures.mediaCacheLeaf)) {
			return FailChatParticipantsRegression(
				"authenticated session accessed a protected cache-file target");
		}
		std::fprintf(
			stderr,
			"Authenticated cache regression passed: root, directory, and file "
			"symlinks refused for cache and media_cache.\n");
	}
	if (Core::MacProtectedPath::IntegrationTestActive()) {
		for (const auto mediaCache : {false, true}) {
			const auto guarded = domain.add(MTP::Environment::Production);
			guarded->mtp().stopForServerEnrollment();
			guarded->setSessionUserId(selfId);
			if (!guarded->createSession(
					RegressionUser(selfId, true, QString()),
					std::make_unique<Main::SessionSettings>())) {
				return FailChatParticipantsRegression(
					"could not create the post-open cache test session");
			}
			const auto cacheGetter
				= [guarded, mediaCache]() -> Storage::Cache::Database & {
				auto &data = guarded->session().data();
				return mediaCache ? data.cacheBigFile() : data.cache();
			};
			const auto clearCaches = [guarded] {
				auto &data = guarded->session().data();
				data.clearLocalStorage();
				data.cache().sync();
				data.cacheBigFile().sync();
			};
			if (!RunPostOpenCacheSymlinkRegression(
					mediaCache ? guarded->local().cacheBigFilePath()
							   : guarded->local().cachePath(),
					mediaCache ? fixtures.openedMediaCache
							   : fixtures.openedCache,
					cacheGetter, clearCaches, [guarded, mediaCache] {
						return RunRefusedCacheReaderRegressions(guarded,
																mediaCache);
					})) {
				return FailChatParticipantsRegression(
					"post-open cache get or put accessed a protected alias");
			}
		}
		std::fprintf(
			stderr,
			"Authenticated cache post-open regression passed: get, put, and "
			"clear "
			"symlinks refused for cache and media_cache.\n");
	}
	const auto pinnedChat = pinned->session().data().chat(chatId);
	const auto pinnedPeer = not_null<PeerData*>(
		static_cast<PeerData*>(&*pinnedChat));
	pinned->session().api().processFullPeer(
		pinnedPeer,
		RegressionChatFullReply(chatId, 1, QString(), UserId(2)));
	if (!pinned->session().user()->isLoaded()
		|| !HasExpectedParticipants(pinnedChat, UserId(2))) {
		return FailChatParticipantsRegression(
			"pinned phone-free self did not retain creator and members");
	}

	const auto actionChatId = ChatId(1052);
	const auto stockActionChat = stock->session().data().chat(actionChatId);
	const auto stockActionPeer = not_null<PeerData*>(
		static_cast<PeerData*>(&*stockActionChat));
	stock->session().api().processFullPeer(
		stockActionPeer,
		RegressionChatFullReply(
			actionChatId,
			1,
			u"+10000000001"_q,
			selfId));
	stockActionChat->setFlags(ChatDataFlag::Creator);
	if (!HasExpectedParticipants(stockActionChat, selfId)
		|| !stockActionChat->canEditInformation()
		|| !stockActionChat->canAddMembers()
		|| !stockActionChat->canAddAdmins()
		|| !stockActionChat->canBanMembers()) {
		return FailChatParticipantsRegression(
			"stock creator lost a supported basic-group action");
	}

	const auto pinnedActionChat = pinned->session().data().chat(actionChatId);
	const auto pinnedActionPeer = not_null<PeerData*>(
		static_cast<PeerData*>(&*pinnedActionChat));
	pinned->session().api().processFullPeer(
		pinnedActionPeer,
		RegressionChatFullReply(actionChatId, 1, QString(), selfId));
	pinnedActionChat->setFlags(ChatDataFlag::Creator);
	if (!HasExpectedParticipants(pinnedActionChat, selfId)
		|| !pinnedActionChat->canEditInformation()
		|| !pinnedActionChat->canAddMembers()
		|| pinnedActionChat->canAddAdmins()
		|| !pinnedActionChat->canBanMembers()) {
		return FailChatParticipantsRegression(
			"pinned creator lost a supported action or retained admin grants");
	}
	if (!pinnedActionChat->usesCustomServer()
		|| pinnedActionChat->isDeactivated()
		|| pinnedActionChat->migrateTo()) {
		return FailChatParticipantsRegression(
			"pinned migration fixture is not an active custom-server group");
	}
	if (!windowsMatch("after primary close")) {
		return FailChatParticipantsRegression(
			"primary close changed the separate session windows");
	}

	struct MigrationResult {
		int done = 0;
		int failed = 0;
		QString error;
	};
	const auto migration = std::make_shared<MigrationResult>();
	pinned->session().api().migrateChat(
		pinnedActionChat,
		[migration](not_null<ChannelData*>) {
			++migration->done;
		},
		[migration](const QString &error) {
			++migration->failed;
			migration->error = error;
		});
	crl::on_main([=] {
		std::fprintf(
			stderr,
			"Pinned migration regression: done=%d failed=%d error=%s\n",
			migration->done,
			migration->failed,
			migration->error.toUtf8().constData());
		if (migration->done != 0
			|| migration->failed != 1
			|| migration->error != u"CLIENT_BAD_MIGRATION"_q) {
			done(FailChatParticipantsRegression(
				"pinned basic-group migration was not rejected locally"));
			return;
		}
		if (Core::MacProtectedPath::IntegrationTestActive()) {
			stock->session().data().clearLocalStorage();
			pinned->session().data().clearLocalStorage();
			stock->session().data().cache().sync();
			stock->session().data().cacheBigFile().sync();
			pinned->session().data().cache().sync();
			pinned->session().data().cacheBigFile().sync();
			if (!FixtureContainsOnlyMarker(fixtures.cacheRoot)
				|| !FixtureContainsOnlyMarker(fixtures.mediaCacheRoot)
				|| !FixtureContainsOnlyMarker(fixtures.cacheLeaf)
				|| !FixtureContainsOnlyMarker(fixtures.mediaCacheLeaf)) {
				done(FailChatParticipantsRegression(
					"asynchronous cache cleanup accessed a protected target"));
				return;
			}
		}
		std::fprintf(stderr, "Chat participants regression passed.\n");
		done(0);
	});
	return 0;
}

[[nodiscard]] int StartAccountLifecycleRegression(Fn<void(int)> done) {
	if (!TeagramIconChoicePersistsAcrossSettingsReload()) {
		return FailAccountLifecycleRegression(
			"Teagram icon choice did not survive settings serialization");
	}
	const auto failureVariable = QByteArray(
		"TDESKTOP_FAIL_MTP_AUTHORIZATION_WRITE");
	const auto failWrites = gsl::finally([&] {
		qunsetenv(failureVariable.constData());
	});

	auto &domain = Core::App().domain();
	if (!domain.started()) {
		return FailAccountLifecycleRegression(
			"application domain did not start");
	}
	if (!Core::App().activePrimaryWindow()) {
		return FailAccountLifecycleRegression(
			"application did not create the primary window fixture");
	}
	if (domain.accounts().size() != 1
		|| domain.accounts().front().account->sessionExists()) {
		return FailAccountLifecycleRegression(
			"regression requires one fresh account in its isolated workdir");
	}
	auto fixtures = ProtectedCacheFixtures();
	if (!PrepareProtectedCacheFixtures(&fixtures)
		|| !RunLegacyCleanupSymlinkRegression(fixtures)) {
		std::fprintf(
			stderr,
			"Protected storage regression failed: legacy cleanup followed "
			"a protected account-directory symlink.\n");
		return 1;
	}

	// This is the production post-auth caller. It must keep the session
	// unpublished when the synchronous authorization write is refused.
	const auto account = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	const auto cacheKey = Storage::Cache::Key{
		0x4d41494e31303632ULL,
		0x4341434845504159ULL,
	};
	const auto cachePayload = QByteArray(
		"main-1062-cache-regression-payload");
	if (!CacheCallbackTimeoutIsSafe()) {
		return FailAccountLifecycleRegression(
			"cache callback wait mishandled failure or late completion");
	}
	if (!WriteCachePayload(account, cacheKey, cachePayload)) {
		return FailAccountLifecycleRegression(
			"could not seed the cache payload");
	}
	if (!account->sessionExists()
		&& !account->mtp().dcOptions().hasCustomServer()
		&& (!account->mtp().dcOptions().unenrolled()
			|| !account->mtp().dcOptions().configEnumDcIds().empty())) {
		// A fresh account is allowed to show enrollment, but it must not
		// carry the built-in production table while it waits there.
		return FailAccountLifecycleRegression(
			"fresh account retained production endpoints before enrollment");
	}
	// Sender destruction must cancel a refused request before queued delivery.
	account->mtp().stopForServerEnrollment();
	auto cancelledFailures = 0;
	{
		auto sender = MTP::Sender(&account->mtp());
		const auto requestId = sender.request(MTPupdates_GetState(
		)).fail([&](const MTP::Error &) {
			++cancelledFailures;
		}).send();
		if (!sender.pending(requestId)) {
			return FailAccountLifecycleRegression(
				"stopped sender did not retain its request");
		}
	}
	QCoreApplication::processEvents();
	if (cancelledFailures) {
		return FailAccountLifecycleRegression(
			"destroyed sender delivered a cancelled request");
	}
	auto liveFailures = 0;
	{
		auto sender = MTP::Sender(&account->mtp());
		const auto requestId = sender.request(MTPupdates_GetState(
		)).fail([&](const MTP::Error &error) {
			if (MTP::IsServerEnrollmentPausedError(error)) {
				++liveFailures;
			}
		}).send();
		if (!sender.pending(requestId)) {
			return FailAccountLifecycleRegression(
				"paused sender did not retain its request");
		}
		QCoreApplication::processEvents();
		if (sender.pending(requestId) || liveFailures != 1) {
			return FailAccountLifecycleRegression(
				"paused request was not rejected locally");
		}
	}
	const auto originalKey = RegressionServerKey();
	const auto originalFingerprint = originalKey->fingerprint();
	if (!ConfigurePinnedServer(account, originalKey)) {
		return FailAccountLifecycleRegression(
			"could not pin the test server");
	}
	account->setSessionUserId(UserId(4242));
	qputenv(failureVariable.constData(), "1");
	const auto published = account->createSession(
		UserId(4242),
		QByteArray(),
		0,
		std::make_unique<Main::SessionSettings>());
	if (published || account->sessionExists()
		|| account->willHaveSessionUniqueId(nullptr) == 0
		|| !account->local().mtpAuthorizationWriteFailed()) {
		return FailAccountLifecycleRegression(
			"post-auth write failure published a session or lost its marker");
	}
	qunsetenv(failureVariable.constData());

	// The failed post-auth write already persisted the block marker; leaving
	// it in place verifies startup restores that durable block.
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart after the post-auth write failure");
	}

	const auto blocked = FindAuthorizationBlockedAccount(domain);
	if (!blocked || !blocked->mtp().config().blocked()) {
		return FailAccountLifecycleRegression(
			"post-auth failure did not restore a blocked account");
	}
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart after blocked account teardown");
	}
	const auto stillBlocked = FindAuthorizationBlockedAccount(domain);
	if (!stillBlocked || !stillBlocked->mtp().config().blocked()) {
		return FailAccountLifecycleRegression(
			"blocked teardown cleared the durable authorization failure");
	}
	const auto restarted = Main::details::CommitServerForget(
		&stillBlocked->local(),
		Storage::ServerCacheBinding{
			.fingerprintKnown = true,
			.fingerprint = originalFingerprint,
			.userIdKnown = true,
			.userId = 4242,
		},
		[] {});
	if (!restarted
		|| stillBlocked->local().hasStoredCustomServer()
		|| stillBlocked->local().mtpAuthorizationWriteFailed()) {
		return FailAccountLifecycleRegression(
			"Forget did not durably clear the blocked server state");
	}
	if (stillBlocked->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return FailAccountLifecycleRegression(
			"Forget left the persisted authorization snapshot before restart");
	}

	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart after Forget");
	}
	const auto forgotten = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!PersistedCachePayloadMatches(
			forgotten,
			cacheKey,
			cachePayload)
		|| !forgotten->mtp().dcOptions().unenrolled()
		|| forgotten->mtp().dcOptions().blocked()
		|| forgotten->local().hasStoredCustomServer()
		|| forgotten->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(forgotten)
		|| !forgotten->mtp().dcOptions().configEnumDcIds().empty()) {
		return FailAccountLifecycleRegression(
			"Forget did not restart unenrolled without auth data or endpoints");
	}
	auto pausedFailures = 0;
	{
		auto sender = MTP::Sender(&forgotten->mtp());
		const auto requestId = sender.request(MTPupdates_GetState(
		)).fail([&](const MTP::Error &error) {
			if (MTP::IsServerEnrollmentPausedError(error)) {
				++pausedFailures;
			}
		}).send();
		if (!sender.pending(requestId)) {
			return FailAccountLifecycleRegression(
				"unenrolled sender did not retain its request");
		}
		QCoreApplication::processEvents();
		if (sender.pending(requestId) || pausedFailures != 1) {
			return FailAccountLifecycleRegression(
				"unenrolled request was not rejected locally");
		}
	}

	const auto initialKey = RegressionAuthKey(0x11);
	forgotten->mtp().dcPersistentKeyChanged(2, initialKey);
	if (!ConfigurePinnedServer(forgotten, originalKey)) {
		return FailAccountLifecycleRegression(
			"could not configure the matching server after Forget");
	}
	forgotten->setSessionUserId(UserId(4242));
	if (!forgotten->createSession(
			RegressionUser(UserId(4242), true, QString()),
			std::make_unique<Main::SessionSettings>())
		|| !forgotten->sessionExists()
		|| forgotten->serverCacheBindingMismatchPending()
		|| forgotten->local().checkServerCacheBinding(
			originalFingerprint,
			4242) != Storage::ServerCacheBindingStatus::Match
		|| !CachePayloadMatches(
			forgotten->session().data().cache(),
			cacheKey,
			cachePayload)) {
		return FailAccountLifecycleRegression(
			"matching server and user identity did not reuse the retained cache payload");
	}
	forgotten->mtp().resume();
	const auto finalKey = RegressionAuthKey(0x22);
	const auto finalKeyId = finalKey->keyId();
	forgotten->mtp().dcPersistentKeyChanged(2, finalKey);
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart after active account teardown");
	}
	auto active = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!active->sessionExists()
		|| !HasAuthKey(active, finalKeyId)
		|| !active->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return FailAccountLifecycleRegression(
			"active teardown did not persist its final authorization snapshot");
	}

	const auto teardownFailureKey = RegressionAuthKey(0x33);
	active->mtp().dcPersistentKeyChanged(2, teardownFailureKey);
	qputenv(failureVariable.constData(), "1");
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart after teardown-only authorization failure");
	}
	qunsetenv(failureVariable.constData());
	auto failedTeardown = FindAuthorizationBlockedAccount(domain);
	if (!failedTeardown
		|| !failedTeardown->mtp().config().blocked()
		|| !failedTeardown->local().mtpAuthorizationWriteFailed()) {
		return FailAccountLifecycleRegression(
			"active teardown-only failure did not block the next startup");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	if (!failedTeardown->local().writeMtpAuthorizationFailure()) {
		return FailAccountLifecycleRegression(
			"could not rewrite the retained authorization failure marker");
	}
	const auto markerRewriteAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (markerRewriteAttempts.authorizationSnapshot != 0
		|| markerRewriteAttempts.authorizationFailureMarker != 1
		|| markerRewriteAttempts.customServerBlockMarker != 0) {
		return FailAccountLifecycleRegression(
			"authorization failure marker observer missed an identical-value rewrite");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	auto blockedTeardownAttempts = LifecycleWriteCountsForRegressionTest();
	if (!RestartDomain(domain, &blockedTeardownAttempts)) {
		return FailAccountLifecycleRegression(
			"could not restart after blocked teardown");
	}
	if (blockedTeardownAttempts.authorizationSnapshot != 0
		|| blockedTeardownAttempts.authorizationFailureMarker != 0
		|| blockedTeardownAttempts.customServerBlockMarker != 0) {
		return FailAccountLifecycleRegression(
			"blocked account teardown attempted an authorization or marker write");
	}
	failedTeardown = FindAuthorizationBlockedAccount(domain);
	if (!failedTeardown
		|| !failedTeardown->mtp().config().blocked()
		|| !failedTeardown->local().mtpAuthorizationWriteFailed()) {
		return FailAccountLifecycleRegression(
			"blocked teardown changed its durable authorization state");
	}
	if (!failedTeardown->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return FailAccountLifecycleRegression(
			"blocked teardown lost the prior authorization snapshot");
	}
	if (!Main::details::CommitServerForget(
			&failedTeardown->local(),
			Storage::ServerCacheBinding{
				.fingerprintKnown = true,
				.fingerprint = originalFingerprint,
				.userIdKnown = true,
				.userId = 4242,
			},
			[] {})) {
		return FailAccountLifecycleRegression(
			"could not Forget the blocked account after teardown");
	}
	if (failedTeardown->local().mtpAuthorizationDataExistsForRegressionTest()) {
		return FailAccountLifecycleRegression(
			"Forget left the prior authorization snapshot before restart");
	}
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not return to enrollment after teardown block");
	}
	auto unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)
		|| unenrolled->local().checkServerCacheBinding(
			originalFingerprint,
			4242) != Storage::ServerCacheBindingStatus::Match) {
		return FailAccountLifecycleRegression(
			"Forget did not retain only the cache identity binding");
	}
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"paused teardown did not complete its restart");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)) {
		return FailAccountLifecycleRegression(
			"paused teardown wrote authorization data or a failure marker");
	}

	if (!CacheMismatchWaitsForDestructiveConfirmation(
			unenrolled,
			RegressionOtherServerKey(),
			UserId(4242),
			originalFingerprint,
			4242)) {
		return FailAccountLifecycleRegression(
			"changed server fingerprint bypassed cache confirmation");
	}
	if (!PersistedCachePayloadMatches(
			unenrolled,
			cacheKey,
			cachePayload)) {
		return FailAccountLifecycleRegression(
			"declined fingerprint change damaged the cached payload");
	}
	if (!Main::details::CommitServerForget(
			&unenrolled->local(),
			Storage::ServerCacheBinding{
				.fingerprintKnown = true,
				.fingerprint = originalFingerprint,
				.userIdKnown = true,
				.userId = 4242,
			},
			[] {})
		|| !RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not reset the declined fingerprint change");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!CacheMismatchWaitsForDestructiveConfirmation(
			unenrolled,
			originalKey,
			UserId(5252),
			originalFingerprint,
			4242)) {
		return FailAccountLifecycleRegression(
			"changed user id bypassed cache confirmation");
	}
	if (!PersistedCachePayloadMatches(
			unenrolled,
			cacheKey,
			cachePayload)) {
		return FailAccountLifecycleRegression(
			"declined user-id change damaged the cached payload");
	}
	if (!unenrolled->beginServerReenrollment(
			Main::details::ServerReenrollmentPrompt::DestructiveConfirmation,
			true)
		|| !unenrolled->local().serverReenrollmentPending()
		|| !RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"confirmed identity change did not schedule destructive cleanup");
	}
	unenrolled = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!unenrolled->mtp().dcOptions().unenrolled()
		|| unenrolled->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(unenrolled)
		|| unenrolled->local().hasStoredCustomServer()
		|| unenrolled->local().customServerPinUnknown()
		|| unenrolled->local().checkServerCacheBinding(
			originalFingerprint,
			4242) != Storage::ServerCacheBindingStatus::None) {
		return FailAccountLifecycleRegression(
			"confirmed identity change did not discard the old cache binding");
	}

	unenrolled->local().writeCustomServerBlocked(false);
	if (!RestartDomain(domain)) {
		return FailAccountLifecycleRegression(
			"could not restart the synthetic blocked account");
	}
	auto blockedWithoutAuthorizationFailure = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!blockedWithoutAuthorizationFailure->mtp().config().blocked()
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)
		|| !blockedWithoutAuthorizationFailure->local().hasStoredCustomServer()
		|| blockedWithoutAuthorizationFailure->local().customServerPinUnknown()) {
		return FailAccountLifecycleRegression(
			"blocked startup did not retain its existing marker and empty auth state");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	qputenv(failureVariable.constData(), "1");
	const auto authorizationAttemptFailed =
		!blockedWithoutAuthorizationFailure->local().writeMtpAuthorization();
	qunsetenv(failureVariable.constData());
	const auto authorizationAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (!authorizationAttemptFailed
		|| authorizationAttempts.authorizationSnapshot != 1
		|| authorizationAttempts.authorizationFailureMarker != 0
		|| authorizationAttempts.customServerBlockMarker != 0
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)) {
		return FailAccountLifecycleRegression(
			"authorization snapshot observer missed an isolated write attempt");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	blockedWithoutAuthorizationFailure->local().writeCustomServerBlocked(false);
	const auto markerAttempts = GetLifecycleWriteCountsForRegressionTest();
	if (markerAttempts.authorizationSnapshot != 0
		|| markerAttempts.authorizationFailureMarker != 0
		|| markerAttempts.customServerBlockMarker != 1
		|| !blockedWithoutAuthorizationFailure->local().hasStoredCustomServer()) {
		return FailAccountLifecycleRegression(
			"block marker observer missed an identical-value rewrite");
	}
	ResetLifecycleWriteCountsForRegressionTest();
	auto teardownAttempts = LifecycleWriteCountsForRegressionTest();
	if (!RestartDomain(domain, &teardownAttempts)) {
		return FailAccountLifecycleRegression(
			"blocked teardown did not complete its restart");
	}
	if (teardownAttempts.authorizationSnapshot != 0
		|| teardownAttempts.authorizationFailureMarker != 0
		|| teardownAttempts.customServerBlockMarker != 0) {
		return FailAccountLifecycleRegression(
			"blocked account teardown attempted an authorization or marker write");
	}
	blockedWithoutAuthorizationFailure = not_null<Main::Account*>(
		domain.accounts().front().account.get());
	if (!blockedWithoutAuthorizationFailure->mtp().config().blocked()
		|| blockedWithoutAuthorizationFailure->local().mtpAuthorizationWriteFailed()
		|| !HasNoAuthorizationState(blockedWithoutAuthorizationFailure)) {
		return FailAccountLifecycleRegression(
			"blocked teardown wrote authorization data or a failure marker");
	}

	return StartChatParticipantsRegression(domain, fixtures, std::move(done));
}

} // namespace

void RunAccountLifecycleRegression(Fn<void(int)> done) {
	if (const auto result = StartAccountLifecycleRegression(done)) {
		done(result);
	}
}

} // namespace Tests
