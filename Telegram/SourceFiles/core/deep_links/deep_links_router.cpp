/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/deep_links/deep_links_router.h"

#include "base/debug_log.h"
#include "base/qthelp_url.h"
#include "core/deep_links/deep_links_chats.h"
#include "core/deep_links/deep_links_contacts.h"
#include "core/deep_links/deep_links_new.h"
#include "core/deep_links/deep_links_settings.h"
#include "core/application.h"
#include "main/main_session.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace Core::DeepLinks {
namespace {

Context ParseCommand(
		Window::SessionController *controller,
		const QString &command) {
	auto result = Context{ .controller = controller };

	auto path = command;
	auto queryStart = path.indexOf('?');
	if (queryStart >= 0) {
		const auto query = path.mid(queryStart + 1);
		path = path.left(queryStart);
		for (const auto &pair : query.split('&')) {
			const auto eq = pair.indexOf('=');
			if (eq > 0) {
				result.params[pair.left(eq).toLower()] = qthelp::url_decode(pair.mid(eq + 1));
			} else if (!pair.isEmpty()) {
				result.params[pair.toLower()] = QString();
			}
		}
	}

	path = path.trimmed();
	while (path.startsWith('/')) {
		path = path.mid(1);
	}
	while (path.endsWith('/')) {
		path.chop(1);
	}

	const auto slash = path.indexOf('/');
	if (slash > 0) {
		result.section = path.left(slash).toLower();
		result.path = path.mid(slash + 1);
	} else {
		result.section = path.toLower();
	}

	return result;
}

[[nodiscard]] bool IsPaidFeaturePath(const Entry &entry, const QString &path) {
	return entry.requiresPaidFeatures
		   && (path == entry.path || path.startsWith(entry.path + u"/"_q));
}

[[nodiscard]] bool IsStoriesFeaturePath(const Entry &entry,
										const QString &path) {
	return entry.requiresStories
		   && (path == entry.path || path.startsWith(entry.path + u"/"_q));
}

} // namespace

Router &Router::Instance() {
	static auto instance = Router();
	return instance;
}

Router::Router() {
	RegisterSettingsHandlers(*this);
	RegisterContactsHandlers(*this);
	RegisterChatsHandlers(*this);
	RegisterNewHandlers(*this);
}

void Router::add(const QString &section, Entry entry) {
	_handlers[section].push_back(std::move(entry));
}

std::optional<QString> Router::findPath(
		const QString &section,
		Fn<bool(const Action &)> matches) const {
	const auto i = _handlers.find(section);
	if (i != _handlers.end()) {
		for (const auto &entry : i->second) {
			if (matches(entry.action)) {
				return entry.path;
			}
		}
	}
	return std::nullopt;
}

bool Router::tryHandle(
		Window::SessionController *controller,
		const QString &command) {
	const auto ctx = ParseCommand(controller, command);
	const auto [result, skipActivation] = dispatch(ctx);
	if (ctx.section == u"settings"_q) {
		const auto resultName = [&] {
			switch (result) {
			case Result::Handled: return u"handled"_q;
			case Result::NeedsAuth: return u"needs-auth"_q;
			case Result::Unsupported: return u"unsupported"_q;
			case Result::NotFound: return u"not-found"_q;
			}
			return u"unknown"_q;
		}();
		const auto controllerId = controller
			? QString::number(reinterpret_cast<quintptr>(controller), 16)
			: u"none"_q;
		const auto sessionId = controller
			? QString::number(
				reinterpret_cast<quintptr>(&controller->session()),
				16)
			: u"none"_q;
		LOG(("SettingsLinkTrace: router.match section=%1 path=%2 result=%3 "
			"controller=%4 session=%5")
			.arg(ctx.section)
			.arg(ctx.path)
			.arg(resultName)
			.arg(controllerId)
			.arg(sessionId));
	}

	switch (result) {
	case Result::Handled:
		if (controller && !skipActivation) {
			controller->window().activate();
		}
		return true;
	case Result::NeedsAuth:
		return false;
	case Result::Unsupported:
		showUnsupportedMessage(controller, command);
		return true;
	case Result::NotFound:
		return false;
	}
	return false;
}

bool Router::requiresPaidFeatures(const QString &command) const {
	const auto ctx = ParseCommand(nullptr, command);
	const auto path = ctx.path.toLower();
	const auto i = _handlers.find(ctx.section);
	return (i != _handlers.end())
		   && std::any_of(i->second.begin(), i->second.end(),
						  [&](const auto &entry) {
							  return IsPaidFeaturePath(entry, path);
						  });
}

Router::DispatchResult Router::dispatch(const Context &ctx) {
	if (ctx.section.isEmpty()) {
		return { Result::NotFound };
	}
	return handleSection(ctx.section, ctx);
}

Router::DispatchResult Router::handleSection(
		const QString &section,
		const Context &ctx) {
	const auto it = _handlers.find(section);
	if (it == _handlers.end()) {
		return { Result::NotFound };
	}

	const auto &entries = it->second;
	const auto path = ctx.path.toLower();
	const auto paidRoute
		= std::any_of(entries.begin(), entries.end(), [&](const auto &entry) {
			  return IsPaidFeaturePath(entry, path);
		  });
	if (paidRoute && !ctx.controller) {
		return {Result::Handled, true};
	}
	if (paidRoute && ctx.controller) {
		const auto supported
			= ctx.controller->session().paidFeaturesSupported();
		LOG(("SettingsLinkTrace: router.paidFeaturesSupported "
			"section=%1 path=%2 value=%3")
			.arg(section, path)
			.arg(supported));
		if (!supported) {
			LOG(("SettingsLinkTrace: toast.feature-unavailable reached=1 "
				"section=%1 path=%2")
				.arg(section)
				.arg(path));
			ctx.controller->showFeatureUnavailableOnServerToast();
			return {Result::Handled, true};
		}
	}
	const auto storiesRoute
		= ctx.controller
		  && std::any_of(entries.begin(), entries.end(),
						 [&](const auto &entry) {
							 return IsStoriesFeaturePath(entry, path);
						 });
	if (storiesRoute && !ctx.controller->session().storiesSupported()) {
		if (section == u"settings"_q) {
			LOG(("SettingsLinkTrace: toast.stories-unavailable reached=1 "
				"section=%1 path=%2")
				.arg(section)
				.arg(path));
		}
		ctx.controller->showFeatureUnavailableOnServerToast();
		return {Result::Handled, true};
	}

	for (const auto &entry : entries) {
		if (entry.path == path || (entry.path.isEmpty() && path.isEmpty())) {
			if (entry.requiresAuth && !ctx.controller) {
				return { Result::NeedsAuth };
			}
			const auto action = executeAction(entry.action, ctx);
			return {
				action.result,
				action.skipActivation || entry.skipActivation,
			};
		}
	}

	for (const auto &entry : entries) {
		if (!entry.path.isEmpty() && path.startsWith(entry.path + '/')) {
			if (entry.requiresAuth && !ctx.controller) {
				return { Result::NeedsAuth };
			}
			const auto action = executeAction(entry.action, ctx);
			return {
				action.result,
				action.skipActivation || entry.skipActivation,
			};
		}
	}

	return { Result::Unsupported };
}

Router::DispatchResult Router::executeAction(const Action &action,
											 const Context &ctx) {
	return v::match(
		action,
		[&](const SettingsSection &s) -> DispatchResult {
			if (!ctx.controller) {
				return {Result::NeedsAuth};
			}
			const auto highlight = ctx.params.value(u"highlight"_q);
			if (!highlight.isEmpty()) {
				ctx.controller->setHighlightControlId(highlight);
			}
			ctx.controller->showSettings(s.sectionId);
			return {Result::Handled};
		},
		[&](const SettingsControl &s) -> DispatchResult {
			if (!ctx.controller) {
				return {Result::NeedsAuth};
			}
			if (s.available && !s.available(ctx.controller->session())) {
				if (ctx.section == u"settings"_q) {
					LOG(("SettingsLinkTrace: toast.feature-unavailable reached=1 "
						"section=%1 path=%2")
						.arg(ctx.section)
						.arg(ctx.path));
				}
				ctx.controller->showFeatureUnavailableOnServerToast();
				return {Result::Handled, true};
			}
			if (!s.controlId.isEmpty()) {
				ctx.controller->setHighlightControlId(s.controlId);
			}
			ctx.controller->showSettings(s.sectionId);
			return {Result::Handled};
		},
		[&](const CodeBlock &c) -> DispatchResult { return {c.handler(ctx)}; },
		[&](const AliasTo &a) -> DispatchResult {
			auto aliasCtx = ctx;
			aliasCtx.section = a.section;
			aliasCtx.path = a.path;
			return handleSection(a.section, aliasCtx);
		});
}

void Router::showUnsupportedMessage(
		Window::SessionController *controller,
		const QString &url) {
	const auto text = u"This link is not supported on Desktop."_q;
	if (controller) {
		if (url.startsWith(u"settings/"_q, Qt::CaseInsensitive)) {
			LOG(("SettingsLinkTrace: toast.unsupported reached=1 path=%1")
				.arg(url));
		}
		controller->showToast(text);
	} else if (const auto window = Core::App().activeWindow()) {
		if (url.startsWith(u"settings/"_q, Qt::CaseInsensitive)) {
			LOG(("SettingsLinkTrace: toast.unsupported reached=1 path=%1 "
				"destination=active-window")
				.arg(url));
		}
		window->showToast(text);
	}
}

} // namespace Core::DeepLinks
