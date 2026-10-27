/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "crl/crl_on_main.h"

#include <rpl/event_stream.h>

namespace crl {

rpl::producer<> on_main_update_requests() {
	static auto requests = rpl::event_stream<>();
	return requests.events();
}

} // namespace crl
