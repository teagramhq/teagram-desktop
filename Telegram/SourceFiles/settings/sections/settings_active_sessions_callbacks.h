/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <crl/crl.h>
#include <rpl/event_stream.h>

#include <utility>

namespace Settings::details {

template <typename Producer, typename Handler>
void BindReloadFailures(Producer &&failures, Handler &&handler,
						rpl::lifetime &lifetime) {
	std::forward<Producer>(failures)
		| rpl::on_next([handler = std::forward<Handler>(handler)](
						   const auto &) { handler(); },
					   lifetime);
}

template <typename Weak, typename Done, typename Fail>
[[nodiscard]] auto GuardTerminateAllCallbacks(const Weak &weak, Done &&done,
											  Fail &&fail) {
	return std::pair(crl::guard(weak, std::forward<Done>(done)),
					 crl::guard(weak, std::forward<Fail>(fail)));
}

} // namespace Settings::details
