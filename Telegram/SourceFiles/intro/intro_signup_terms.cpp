/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_signup_terms.h"

#include "mtproto/mtp_instance.h"

#include <algorithm>

#include <QtCore/Qt>

namespace Intro::details {
namespace {

bool IsSupportedTermsLink(const QString &link) {
	if (link.isEmpty()) {
		return false;
	}
	const auto colon = link.indexOf(u':');
	if (colon < 0) {
		return true;
	}
	const auto scheme = link.left(colon).toLower();
	if (scheme == u"http"_q || scheme == u"https"_q || scheme == u"mailto"_q) {
		return true;
	}
	if (!scheme.contains(u'.')) {
		return false;
	}
	// A hostname with a numeric port is still a web address, not an app URI.
	auto digits = 0;
	for (auto i = colon + 1; i != link.size(); ++i) {
		const auto ch = link[i];
		if (ch == u'/' || ch == u'?' || ch == u'#') {
			break;
		}
		if (ch < u'0' || ch > u'9') {
			return false;
		}
		++digits;
	}
	return digits > 0;
}

bool IsValidEntityRange(const QString &text, int offset, int length) {
	return (offset >= 0) && (length > 0) && (offset <= text.size())
		   && (length <= text.size() - offset);
}

} // namespace

bool SignupTermsPayload::hasSameConsent(const SignupTermsPayload &other) const {
	return (id == other.id) && (text == other.text) && (minAge == other.minAge);
}

SignupTermsPayload
SignupTermsPayloadFromMTP(const MTPDhelp_termsOfService &data) {
	auto result = SignupTermsPayload{
		bytes::make_vector(data.vid().c_dataJSON().vdata().v),
		{qs(data.vtext()), {}},
		std::nullopt,
		data.is_popup(),
	};
	TextUtilities::ParseEntities(result.text, TextParseLinks);
	auto serverEntities = EntitiesInText();
	const auto add = [&](EntityType type, int offset, int length,
						 const QString &url = QString()) {
		if (!IsValidEntityRange(result.text.text, offset, length)) {
			return;
		}
		const auto visible = result.text.text.mid(offset, length);
		if (type == EntityType::Url && !IsSupportedTermsLink(visible)) {
			return;
		} else if (type == EntityType::CustomUrl
				   && !IsSupportedTermsLink(url)) {
			return;
		} else if (type == EntityType::Email && !visible.contains(u'@')) {
			return;
		}
		serverEntities.push_back({type, offset, length, url});
	};
	for (const auto &entity : data.ventities().v) {
		entity.match(
			[&](const MTPDmessageEntityUrl &d) {
				add(EntityType::Url, d.voffset().v, d.vlength().v);
			},
			[&](const MTPDmessageEntityEmail &d) {
				add(EntityType::Email, d.voffset().v, d.vlength().v);
			},
			[&](const MTPDmessageEntityTextUrl &d) {
				add(EntityType::CustomUrl, d.voffset().v, d.vlength().v,
					qs(d.vurl()));
			},
			[](const auto &) {
				// Other server entities are literal text in signup terms.
			});
	}
	const auto overlaps
		= [](const EntityInText &left, const EntityInText &right) {
			  return (left.offset() < right.offset() + right.length())
					 && (right.offset() < left.offset() + left.length());
		  };
	auto &entities = result.text.entities;
	entities.erase(std::remove_if(entities.begin(), entities.end(),
								  [&](const EntityInText &parsed) {
									  return std::ranges::any_of(
										  serverEntities,
										  [&](const EntityInText &server) {
											  return overlaps(parsed, server);
										  });
								  }),
				   entities.end());
	entities += serverEntities;
	std::stable_sort(entities.begin(), entities.end(),
					 [](const EntityInText &left, const EntityInText &right) {
						 return left.offset() < right.offset();
					 });
	const auto keepSupported = [&](const EntityInText &entity) {
		if (!IsValidEntityRange(result.text.text, entity.offset(),
								entity.length())) {
			return false;
		}
		switch (entity.type()) {
		case EntityType::Url:
			return IsSupportedTermsLink(
				result.text.text.mid(entity.offset(), entity.length()));
		case EntityType::CustomUrl:
			return IsSupportedTermsLink(entity.data());
		case EntityType::Email:
			return result.text.text.mid(entity.offset(), entity.length())
				.contains(u'@');
		default:
			return false;
		}
	};
	entities.erase(std::remove_if(entities.begin(), entities.end(),
								  [&](const EntityInText &entity) {
									  return !keepSupported(entity);
								  }),
				   entities.end());
	if (const auto minAge = data.vmin_age_confirm()) {
		result.minAge = minAge->v;
	}
	return result;
}

bool HandleSignupTermsKey(int key, bool declineOnEscape, Fn<void()> accept,
						  Fn<void()> decline) {
	if (key == Qt::Key_Enter || key == Qt::Key_Return) {
		accept();
		return true;
	} else if (key == Qt::Key_Escape && declineOnEscape) {
		decline();
		return true;
	}
	return false;
}

void SignupTermsGate::submit(const SignupTermsPayload &terms,
							 Fn<void(Fn<void()>, Fn<void()>)> showTerms,
							 Fn<void(SignupTermsPermission)> sendSignup,
							 Fn<void()> declined) {
	if (terms.text.text.isEmpty()
		|| (_accepted && _accepted->hasSameConsent(terms))) {
		sendSignup(SignupTermsPermission());
		return;
	}
	auto accept = [this, terms, sendSignup = std::move(sendSignup)] {
		_accepted = terms;
		sendSignup(SignupTermsPermission());
	};
	showTerms(std::move(accept), std::move(declined));
}

} // namespace Intro::details
