/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/launcher.h"
#include "core/mac_protected_path_runtime.h"

#include <cstring>

int main(int argc, char *argv[]) {
	if (!Core::MacProtectedPath::InitializeProfile()) {
		return 1;
	}
#if defined(Q_OS_MAC) && defined(TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST)
	if (Core::MacProtectedPath::IntegrationTestActive() && argc == 3) {
		if (!std::strcmp(argv[1], "--mac-seatbelt-parent-open-probe")) {
			return Core::MacProtectedPath::RunSeatbeltOpenProbe(
				argv[2], true, false);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-parent-open-allow-probe")) {
			return Core::MacProtectedPath::RunSeatbeltOpenProbe(
				argv[2], false, false);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-fork-open-probe")) {
			return Core::MacProtectedPath::RunSeatbeltOpenProbe(
				argv[2], true, true);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-fork-open-allow-probe")) {
			return Core::MacProtectedPath::RunSeatbeltOpenProbe(
				argv[2], false, true);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-fork-exec-cat-probe")) {
			return Core::MacProtectedPath::RunSeatbeltForkExecCatProbe(
				argv[2], true);
		}
		if (!std::strcmp(argv[1],
					 "--mac-seatbelt-fork-exec-cat-allow-probe")) {
			return Core::MacProtectedPath::RunSeatbeltForkExecCatProbe(
				argv[2], false);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-cat-probe")) {
			return Core::MacProtectedPath::RunSeatbeltCatProbe(argv[2], true);
		}
		if (!std::strcmp(argv[1], "--mac-seatbelt-cat-allow-probe")) {
			return Core::MacProtectedPath::RunSeatbeltCatProbe(argv[2], false);
		}
	}
#endif
	const auto launcher = Core::Launcher::Create(argc, argv);
	return launcher ? launcher->exec() : 1;
}
