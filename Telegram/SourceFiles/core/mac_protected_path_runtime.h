/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/mac_protected_path_policy.h"

#include <functional>
#include <memory>

namespace Core::MacProtectedPath {

struct CachePathDescriptor;

struct CachePathComponentIdentity final {
	QString path;
	quint64 device = 0;
	quint64 inode = 0;
	quint32 type = 0;
	quint64 targetDevice = 0;
	quint64 targetInode = 0;
	quint32 targetType = 0;
	qint64 size = 0;
	qint64 modifiedSeconds = 0;
	qint64 modifiedNanoseconds = 0;
	qint64 changedSeconds = 0;
	qint64 changedNanoseconds = 0;
	bool exists = false;
};

struct CachePathIdentity final {
	QString absolutePath;
	QString resolvedPath;
	QString versionName;
	QString activeVersionName;
	std::vector<CachePathComponentIdentity> components;
	std::shared_ptr<const CachePathDescriptor> rootDescriptor;
	std::shared_ptr<const CachePathDescriptor> versionMarkerDescriptor;
	CachePathComponentIdentity versionMarker;
	qint32 version = 0;
	std::shared_ptr<const CachePathDescriptor> activeVersionDescriptor;
	CachePathComponentIdentity activeVersionDirectory;
};

#ifdef Q_OS_MAC
[[nodiscard]] bool InitializeProfile();
[[nodiscard]] bool IntegrationTestActive();
[[nodiscard]] bool IsActive();
[[nodiscard]] QString InitialWorkingDirectory();
[[nodiscard]] QString IpcDirectory();
[[nodiscard]] QString NotificationSoundsDirectory();
[[nodiscard]] QString ProfileRoot();

[[nodiscard]] bool CheckPath(Operation operation, const QString &path,
							 const char *callsite);

[[nodiscard]] bool CheckPathAt(Operation operation, const QString &path,
							   const QString &anchor, const char *callsite);

[[nodiscard]] bool CheckExternalPath(Operation operation, const QString &path,
									 const char *callsite);

[[nodiscard]] bool CheckCachePath(const QString &path, const char *callsite);
[[nodiscard]] bool
CheckCachePathAndCaptureIdentity(const QString &path, const char *callsite,
								 CachePathIdentity *identity);
[[nodiscard]] bool
CaptureCachePathActiveVersionIdentity(const QString &path, const char *callsite,
									  CachePathIdentity *identity);
[[nodiscard]] bool CheckCachePathIdentity(const QString &path,
										  const char *callsite,
										  CachePathIdentity *identity);
[[nodiscard]] bool CheckCachePathAccessIdentity(const QString &path,
												const char *callsite,
												CachePathIdentity *identity);

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
void ResetCachePathWalkCountForTesting();
[[nodiscard]] int CachePathWalkCountForTesting();
[[nodiscard]] int CachePathWalkMainThreadCountForTesting();
[[nodiscard]] bool
CheckCachePathForTesting(const QString &path, const char *callsite,
						 std::function<void(const QString &)> beforeEntryStat);
[[nodiscard]] int RunNotificationSoundProbe();
[[nodiscard]] int RunSeatbeltOpenProbe(const char *path, bool expectDenied,
									   bool forkChild);
[[nodiscard]] int RunSeatbeltCatProbe(const char *path, bool expectDenied);
[[nodiscard]] int RunSeatbeltForkExecCatProbe(const char *path,
											  bool expectDenied);
#endif

[[nodiscard]] bool CheckPair(Operation operation, const QString &first,
							 const QString &second, const char *callsite);
#else  // Q_OS_MAC
[[nodiscard]] inline bool InitializeProfile() { return true; }
[[nodiscard]] inline bool IntegrationTestActive() { return false; }
[[nodiscard]] inline bool IsActive() { return false; }
[[nodiscard]] inline QString InitialWorkingDirectory() { return {}; }
[[nodiscard]] inline QString IpcDirectory() { return {}; }
[[nodiscard]] inline QString NotificationSoundsDirectory() { return {}; }
[[nodiscard]] inline QString ProfileRoot() { return {}; }
[[nodiscard]] inline bool CheckPath(Operation, const QString &, const char *) {
	return true;
}
[[nodiscard]] inline bool CheckPathAt(Operation, const QString &,
									  const QString &, const char *) {
	return true;
}
[[nodiscard]] inline bool CheckExternalPath(Operation, const QString &,
											const char *) {
	return true;
}
[[nodiscard]] inline bool CheckCachePath(const QString &, const char *) {
	return true;
}
[[nodiscard]] inline bool
CheckCachePathAndCaptureIdentity(const QString &, const char *,
								 CachePathIdentity *) {
	return true;
}
[[nodiscard]] inline bool
CaptureCachePathActiveVersionIdentity(const QString &, const char *,
									  CachePathIdentity *) {
	return true;
}
[[nodiscard]] inline bool CheckCachePathIdentity(const QString &, const char *,
												 CachePathIdentity *) {
	return true;
}
[[nodiscard]] inline bool CheckCachePathAccessIdentity(const QString &,
													   const char *,
													   CachePathIdentity *) {
	return true;
}
[[nodiscard]] inline bool CheckPair(Operation, const QString &, const QString &,
									const char *) {
	return true;
}
#endif // !Q_OS_MAC

} // namespace Core::MacProtectedPath
