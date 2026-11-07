/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"

#include <type_traits>
#include <utility>
#include <variant>

template <typename Request>
using AuthRequest = std::variant<Request, MTPInvokeWithReCaptcha<Request>>;

template <typename Request>
[[nodiscard]] AuthRequest<Request> BuildAuthRequest(Request request,
													const QString &token) {
	if (token.isEmpty()) {
		return AuthRequest<Request>(std::in_place_index<0>, std::move(request));
	}
	return AuthRequest<Request>(
		std::in_place_index<1>,
		MTPInvokeWithReCaptcha<Request>(MTP_string(token), std::move(request)));
}

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
	auto built = BuildAuthRequest(std::move(request), token);
	const auto result = std::visit(
		[&](auto &&value) {
			using Type = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<Type, Request>) {
				return send(sender.request(std::move(value)));
			} else {
				return send(sender.requestWrapped<Request>(std::move(value)));
			}
		},
		std::move(built));
	token.fill(QChar(0));
	token.clear();
	return result;
}
