/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/font_shutdown_probe.h"

#if defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)

#include <CoreText/CTFontManager.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

constexpr auto kProbeMaxFrames = 64;
pthread_t ProbeWorker = {};
std::atomic_bool ProbeArmed = false;
std::atomic_bool ProbeReleased = false;
std::atomic_int ProbeSkippedPopulations = 0;
std::atomic_bool ProbeInterposerReported = false;

[[nodiscard]] bool IsQtFontDatabasePopulation() {
	void *frames[kProbeMaxFrames] = {};
	const auto depth = backtrace(frames, kProbeMaxFrames);
	auto database = false;
	auto families = false;
	auto coreTextDatabase = false;
	auto qtPopulation = false;
	auto privateDatabase = false;
	auto databaseEnsure = false;
	for (auto i = 0; i != depth; ++i) {
		auto info = Dl_info();
		if (!dladdr(frames[i], &info) || !info.dli_sname) {
			continue;
		}
		const auto name = std::string_view(info.dli_sname);
		database = database || (name.find("QFontDatabase") != name.npos);
		families = families || (name.find("families") != name.npos);
		coreTextDatabase = coreTextDatabase
						   || (name.find("QCoreTextFontDatabase") != name.npos);
		qtPopulation = qtPopulation
					   || (name.find("populateFromDescriptor") != name.npos)
					   || (name.find("populateFontDatabase") != name.npos);
		privateDatabase = privateDatabase
						  || (name.find("QFontDatabasePrivate") != name.npos);
		databaseEnsure
			= databaseEnsure || (name.find("ensureFontDatabase") != name.npos);
	}
	return (database && families) || (coreTextDatabase && qtPopulation)
		   || (privateDatabase && databaseEnsure);
}

using CopyAvailableFontFamilyNames = CFArrayRef (*)(void);

[[nodiscard]] CopyAvailableFontFamilyNames
OriginalCopyAvailableFontFamilyNames() {
	static const auto result = reinterpret_cast<CopyAvailableFontFamilyNames>(
		dlsym(RTLD_NEXT, "CTFontManagerCopyAvailableFontFamilyNames"));
	return result;
}

} // namespace

namespace Core::Tests {

void ArmFontShutdownProbe(pthread_t worker, int skippedPopulations) {
	ProbeWorker = worker;
	ProbeSkippedPopulations.store(skippedPopulations,
								  std::memory_order_relaxed);
	ProbeArmed.store(true, std::memory_order_release);
}

void ReleaseFontShutdownProbe() {
	ProbeReleased.store(true, std::memory_order_release);
}

bool IsFontShutdownProbeReleased() {
	return ProbeReleased.load(std::memory_order_acquire);
}

} // namespace Core::Tests

extern "C" __attribute__((noinline, used)) CFArrayRef
Teagram_FontFamilyPopulationProbe() {
	const auto original = OriginalCopyAvailableFontFamilyNames();
	if (!original) {
		fprintf(stderr,
				"Font shutdown regression: probe_error=coretext_lookup\n");
		std::abort();
	}
	if (!ProbeArmed.load(std::memory_order_acquire)
		|| !pthread_equal(pthread_self(), ProbeWorker)
		|| !IsQtFontDatabasePopulation()) {
		return original();
	}
	const auto skipped
		= ProbeSkippedPopulations.load(std::memory_order_relaxed);
	if (skipped > 0) {
		ProbeSkippedPopulations.fetch_sub(1, std::memory_order_relaxed);
		return original();
	}
	// Repeat the real CoreText query inside Qt's families() call. The GUI
	// sampler can then catch a real CoreText frame while Qt still owns its
	// mutex. Only that stack sample reports the hold as observed.
	auto result = original();
	while (!ProbeReleased.load(std::memory_order_acquire)) {
		if (result) {
			CFRelease(result);
		}
		result = original();
		if (!ProbeInterposerReported.exchange(true,
											  std::memory_order_acq_rel)) {
			fprintf(stderr,
					"Font shutdown regression: probe_interposer=active\n");
		}
	}
	return result;
}

#define TDESKTOP_DYLD_INTERPOSE(replacement, replacee)                         \
	__attribute__((used)) static struct {                                      \
		const void *replacement;                                               \
		const void *replacee;                                                  \
	} _teagram_interpose_##replacee                                            \
		__attribute__((section("__DATA,__interpose"))) = {                     \
			(const void *)(unsigned long)&replacement,                         \
			(const void *)(unsigned long)&replacee,                            \
	}

TDESKTOP_DYLD_INTERPOSE(Teagram_FontFamilyPopulationProbe,
						CTFontManagerCopyAvailableFontFamilyNames);

#endif // TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST
