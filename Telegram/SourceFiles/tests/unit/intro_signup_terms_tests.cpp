/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "intro/intro_signup_terms.h"
#include "mtproto/mtp_instance.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include <QtCore/Qt>

namespace Intro::details {
namespace {

static_assert(!std::is_default_constructible_v<SignupTermsPermission>);

SignupTermsPayload Terms(QString text, QByteArray id = QByteArray("terms-id")) {
	return {
		bytes::make_vector(id),
		{std::move(text), {}},
		std::nullopt,
		false,
	};
}

TEST_CASE(SignupTermsMtpExtractionKeepsOnlySupportedLinks) {
	const auto text
		= u"@admin #tag /help https://terms.example "
		  u"server@example.test bare.example bare@bare.test privacy unsafe"_q;
	const auto mention = text.indexOf(u"@admin"_q);
	const auto hashtag = text.indexOf(u"#tag"_q);
	const auto command = text.indexOf(u"/help"_q);
	const auto url = text.indexOf(u"https://terms.example"_q);
	const auto email = text.indexOf(u"server@example.test"_q);
	const auto textUrl = text.indexOf(u"privacy"_q);
	const auto unsafeUrl = text.indexOf(u"unsafe"_q);
	auto entities = QVector<MTPMessageEntity>{
		MTP_messageEntityMention(MTP_int(mention), MTP_int(6)),
		MTP_messageEntityHashtag(MTP_int(hashtag), MTP_int(4)),
		MTP_messageEntityBotCommand(MTP_int(command), MTP_int(5)),
		MTP_messageEntityMentionName(MTP_int(mention), MTP_int(6),
									 MTP_long(123)),
		MTP_messageEntityUrl(MTP_int(url), MTP_int(21)),
		MTP_messageEntityEmail(MTP_int(email), MTP_int(19)),
		MTP_messageEntityTextUrl(MTP_int(textUrl), MTP_int(7),
								 MTP_string("https://privacy.example")),
		MTP_messageEntityTextUrl(MTP_int(unsafeUrl), MTP_int(6),
								 MTP_string("tg://resolve?domain=admin")),
		MTP_messageEntityUrl(MTP_int(text.size() + 1), MTP_int(9)),
	};
	const auto response = MTP_help_termsOfService(
		MTP_flags(0), MTP_dataJSON(MTP_bytes(QByteArray("terms-id"))),
		MTP_string(text), MTP_vector<MTPMessageEntity>(std::move(entities)),
		MTP_int(0));
	const auto terms
		= SignupTermsPayloadFromMTP(response.c_help_termsOfService());

	CHECK(terms.id == bytes::make_vector(QByteArray("terms-id")));
	CHECK_EQ(terms.text.text, text);
	CHECK(terms.text.entities.size() >= 5);
	CHECK(std::ranges::all_of(terms.text.entities, [](const auto &entity) {
		return (entity.type() == EntityType::Url)
			   || (entity.type() == EntityType::CustomUrl)
			   || (entity.type() == EntityType::Email);
	}));
	CHECK(std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return (entity.type() == EntityType::CustomUrl)
			   && (entity.offset() == textUrl)
			   && (entity.data() == u"https://privacy.example"_q);
	}));
	CHECK(std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return (entity.type() == EntityType::Url) && (entity.offset() == url);
	}));
	CHECK(std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return (entity.type() == EntityType::Email)
			   && (entity.offset() == email);
	}));
	CHECK(std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return (entity.type() == EntityType::Url)
			   && (entity.offset() == text.indexOf(u"bare.example"_q));
	}));
	CHECK(std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return (entity.type() == EntityType::Email)
			   && (entity.offset() == text.indexOf(u"bare@bare.test"_q));
	}));
	CHECK(!std::ranges::any_of(terms.text.entities, [&](const auto &entity) {
		return entity.offset() == unsafeUrl;
	}));
}

TEST_CASE(NameSignupRpcWaitsForTermsAcceptance) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(u"Terms for this instance"_q);
	auto signupRequests = 0;
	Fn<void()> accept;
	Fn<void()> decline;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()> declineNow) {
			accept = std::move(acceptNow);
			decline = std::move(declineNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK_EQ(signupRequests, 0);
	accept();
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(PasswordSignupRpcWaitsForTermsAcceptance) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(u"Terms for this instance"_q);
	auto signupRequests = 0;
	Fn<void()> accept;
	Fn<void()> decline;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()> declineNow) {
			accept = std::move(acceptNow);
			decline = std::move(declineNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK_EQ(signupRequests, 0);
	accept();
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(SignupTermsDeclinePreservesFormAndSendsNoRpc) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(u"Terms for this instance"_q);
	auto username = u"alice"_q;
	auto firstName = u"Alice"_q;
	auto password = u"secret"_q;
	auto signupRequests = 0;
	auto declined = false;
	Fn<void()> accept;
	Fn<void()> decline;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()> declineNow) {
			accept = std::move(acceptNow);
			decline = std::move(declineNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; },
		[&] { declined = true; });
	decline();

	CHECK(declined);
	CHECK_EQ(signupRequests, 0);
	CHECK_EQ(username, u"alice"_q);
	CHECK_EQ(firstName, u"Alice"_q);
	CHECK_EQ(password, u"secret"_q);
}

TEST_CASE(SignupTermsEscapeRoutesToDecline) {
	auto accepts = 0;
	auto declines = 0;
	CHECK(HandleSignupTermsKey(
		Qt::Key_Escape, true, [&] { ++accepts; }, [&] { ++declines; }));
	CHECK_EQ(accepts, 0);
	CHECK_EQ(declines, 1);
	CHECK(!HandleSignupTermsKey(
		Qt::Key_Escape, false, [&] { ++accepts; }, [&] { ++declines; }));
	CHECK_EQ(accepts, 0);
	CHECK_EQ(declines, 1);
}

TEST_CASE(SignupTermsEnterRoutesToAccept) {
	auto accepts = 0;
	auto declines = 0;
	CHECK(HandleSignupTermsKey(
		Qt::Key_Enter, true, [&] { ++accepts; }, [&] { ++declines; }));
	CHECK(HandleSignupTermsKey(
		Qt::Key_Return, true, [&] { ++accepts; }, [&] { ++declines; }));
	CHECK_EQ(accepts, 2);
	CHECK_EQ(declines, 0);
}

TEST_CASE(SignupTermsEmptyTextSendsWithoutPrompt) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(QString());
	auto prompted = false;
	auto signupRequests = 0;

	gate.submit(
		terms, [&](Fn<void()>, Fn<void()>) { prompted = true; },
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK(!prompted);
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(SignupTermsSameIdRetryDoesNotReopen) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(u"Terms for this instance"_q);
	auto prompts = 0;
	auto signupRequests = 0;
	Fn<void()> accept;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()>) {
			++prompts;
			accept = std::move(acceptNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});
	accept();
	gate.submit(
		terms, [&](Fn<void()>, Fn<void()>) { ++prompts; },
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK_EQ(prompts, 1);
	CHECK_EQ(signupRequests, 2);
}

TEST_CASE(SignupTermsChangedTextWithSameIdRequiresAcceptanceAgain) {
	auto gate = SignupTermsGate();
	auto terms = Terms(u"Terms for this instance"_q);
	auto prompts = 0;
	auto signupRequests = 0;
	Fn<void()> accept;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()>) {
			++prompts;
			accept = std::move(acceptNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});
	accept();
	terms.text.text = u"Changed terms with a reused id"_q;
	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()>) {
			++prompts;
			accept = std::move(acceptNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK_EQ(prompts, 2);
	CHECK_EQ(signupRequests, 1);
}

TEST_CASE(SignupTermsHappyPathSmoke) {
	auto gate = SignupTermsGate();
	const auto terms = Terms(u"Terms for this instance"_q);
	auto signupRequests = 0;
	Fn<void()> accept;

	gate.submit(
		terms,
		[&](Fn<void()> acceptNow, Fn<void()>) {
			accept = std::move(acceptNow);
		},
		[&](SignupTermsPermission) { ++signupRequests; }, [] {});

	CHECK_EQ(signupRequests, 0);
	CHECK(HandleSignupTermsKey(Qt::Key_Return, true, [&] { accept(); }, [] {}));
	CHECK_EQ(signupRequests, 1);
}

} // namespace
} // namespace Intro::details
