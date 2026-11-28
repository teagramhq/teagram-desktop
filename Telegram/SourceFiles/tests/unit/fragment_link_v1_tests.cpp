/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "core/fragment_link_v1.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <utility>
#include <vector>

namespace {

using Core::FragmentLinkV1::CanOpenOnDefaultServer;
using Core::FragmentLinkV1::Decode;
using Core::FragmentLinkV1::DecodeUrl;
using Core::FragmentLinkV1::Encode;
using Core::FragmentLinkV1::IsV1UrlCandidate;
using Core::FragmentLinkV1::Link;
using Core::FragmentLinkV1::LinkForPath;
using Core::FragmentLinkV1::Type;
using Core::FragmentLinkV1::UrlStatus;

[[nodiscard]] Link ExpectedLink(const QJsonObject &input) {
	auto result = Link();
	result.type = static_cast<Type>(input.value(u"type"_q).toInt());
	result.host = input.value(u"host"_q).toString();
	result.username = input.value(u"username"_q).toString();
	result.channelId = input.value(u"channel_id"_q).toString().toULongLong();
	result.messageId = input.value(u"message_id"_q).toString().toULongLong();
	result.hash = QByteArray::fromHex(
		input.value(u"join_hash_hex"_q).toString().toLatin1());
	result.secret = QByteArray::fromHex(
		input.value(u"secret_hex"_q).toString().toLatin1());
	result.shortName = input.value(u"short_name"_q).toString();
	return result;
}

[[nodiscard]] QJsonObject ReadVectors() {
	const auto path
		= QDir(QString::fromUtf8(TDESKTOP_TEST_SOURCE_DIR))
			  .filePath(
				  u"Telegram/SourceFiles/tests/data/fragment-link-v1-vectors.json"_q);
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

TEST_CASE(FragmentLinkV1MatchesTheSharedVectorFile) {
	const auto vectors = ReadVectors();
	const auto valid = vectors.value(u"valid"_q).toArray();
	const auto invalid = vectors.value(u"invalid"_q).toArray();
	CHECK(!valid.isEmpty());
	CHECK(!invalid.isEmpty());
	for (const auto &entry : valid) {
		const auto object = entry.toObject();
		const auto code = object.value(u"code"_q).toString();
		const auto decoded = Decode(code);
		CHECK(decoded.has_value());
		if (decoded) {
			CHECK(*decoded
				  == ExpectedLink(object.value(u"input"_q).toObject()));
			const auto encoded = Encode(*decoded);
			CHECK(encoded.has_value());
			if (encoded) {
				CHECK_EQ(*encoded, code);
			}
		}
	}
	for (const auto &entry : invalid) {
		CHECK(!Decode(entry.toObject().value(u"code"_q).toString()));
	}
}

TEST_CASE(FragmentLinkV1RejectsHostedLinksForOpening) {
	const auto valid = ReadVectors().value(u"valid"_q).toArray();
	auto sawDefault = false;
	auto sawHosted = false;
	for (const auto &entry : valid) {
		const auto decoded
			= Decode(entry.toObject().value(u"code"_q).toString());
		if (!decoded) {
			continue;
		} else if (decoded->host.isEmpty()) {
			sawDefault = true;
			CHECK(CanOpenOnDefaultServer(*decoded));
		} else {
			sawHosted = true;
			CHECK(!CanOpenOnDefaultServer(*decoded));
		}
	}
	CHECK(sawDefault);
	CHECK(sawHosted);
}

TEST_CASE(FragmentLinkV1UrlGateRejectsMalformedAndHostedUrls) {
	const auto invalid = std::vector<QString>{
		u"http://web.teagram.io/#~QQHDpNc"_q,
		u"https://web.teagram.io:8443/#~QQHDpNc"_q,
		u"https://user@web.teagram.io/#~QQHDpNc"_q,
		u"https://web.teagram.io/path#~QQHDpNc"_q,
		u"https://web.teagram.io/?q#~QQHDpNc"_q,
	};
	for (const auto &url : invalid) {
		CHECK(DecodeUrl(url).status == UrlStatus::Invalid);
	}
	CHECK(DecodeUrl(u"https://web.teagram.io.example.com/#~QQHDpNc"_q).status
		  == UrlStatus::NotV1);
	CHECK(DecodeUrl(u"https://web.teagram.io/#alice"_q).status
		  == UrlStatus::NotV1);
	CHECK(DecodeUrl(u"https://web.teagram.io/turnstile.html?token=abc"_q).status
		  == UrlStatus::NotV1);
}

TEST_CASE(FragmentLinkV1InvalidHiddenLinkCandidateIsIntercepted) {
	const auto url = u"https://web.teagram.io/alice#~invalid"_q;
	CHECK(DecodeUrl(url).status == UrlStatus::Invalid);
	CHECK(IsV1UrlCandidate(url));
	CHECK(!IsV1UrlCandidate(
		u"https://web.teagram.io/turnstile.html?token=abc"_q));
}

TEST_CASE(FragmentLinkV1BuildsSupportedInternalPathsSmoke) {
	const auto domain = u"https://web.teagram.io/"_q;
	const auto links = std::vector<std::pair<QString, QString>>{
		{u"alice"_q, u"https://web.teagram.io/#~QQHDpNc"_q},
		{u"alice/1"_q, u"https://web.teagram.io/#~QgQBw6TXAQ"_q},
		{u"c/123456789/42"_q, u"https://web.teagram.io/#~QwQHW80VKg"_q},
		{u"+ECAwQFBgcIA"_q, u"https://web.teagram.io/#~RBAgMEBQYHCA"_q},
		{u"addstickers/funny_cats"_q,
		 u"https://web.teagram.io/#~Rg-jJAT_640"_q},
		{u"addemoji/emoji_pack"_q, u"https://web.teagram.io/#~Rw6FK8h0mt0"_q},
	};
	for (const auto &[path, expected] : links) {
		CHECK_EQ(LinkForPath(path, domain), expected);
	}
}

TEST_CASE(FragmentLinkV1LeavesUnmappedAndNonDefaultPathsUnchanged) {
	CHECK_EQ(LinkForPath(u"addtheme/slug"_q, u"https://web.teagram.io/"_q),
			 u"https://web.teagram.io/addtheme/slug"_q);
	CHECK_EQ(LinkForPath(u"alice?game=chess"_q, u"https://web.teagram.io/"_q),
			 u"https://web.teagram.io/alice?game=chess"_q);
	CHECK_EQ(LinkForPath(u"+1234567890"_q, u"https://web.teagram.io/"_q),
			 u"https://web.teagram.io/+1234567890"_q);
	CHECK_EQ(LinkForPath(u"addlist/some_group"_q, u"https://web.teagram.io/"_q),
			 u"https://web.teagram.io/addlist/some_group"_q);
	CHECK_EQ(LinkForPath(u"alice"_q, u"https://chat.example.org/"_q),
			 u"https://chat.example.org/alice"_q);
}
