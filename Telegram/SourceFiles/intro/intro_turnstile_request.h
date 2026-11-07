/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"

#include <utility>

template <typename Request, typename Done, typename Fail>
[[nodiscard]] mtpRequestId
SendAuthRequest(MTP::Sender &sender, Request request, QString token,
				bool handleFloodErrors, Done done, Fail fail) {
	const auto send = [&](auto builder) {
		auto &configured = builder.done(done).fail(fail);
		return (handleFloodErrors ? configured.handleFloodErrors()
								  : configured.handleAllErrors())
			.send();
	};
	const auto result
		= token.isEmpty()
			  ? send(sender.request(std::move(request)))
			  : send(sender.requestWrapped<Request>(MTPinvokeWithReCaptcha(
					MTP_string(token), std::move(request))));
	token.fill(QChar(0));
	token.clear();
	return result;
}
