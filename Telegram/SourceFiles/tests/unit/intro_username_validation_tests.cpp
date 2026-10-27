/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_input_policy.h"
#include "intro/intro_username_validation.h"

#include <limits>

namespace {

using namespace Intro::details;

const auto kPublishedUsernamePolicy = UsernamePolicy{
	.minLength = 2,
	.maxLength = 32,
	.pattern = u"^[a-zA-Z][a-zA-Z0-9_]{1,31}$"_q,
};

QString AstralString(int count) {
	return QString::fromUtf8("\xF0\x9F\x98\x80").repeated(count);
}

TEST_CASE(NormalizeTrimsWhitespace) {
	CHECK_EQ(NormalizeUsernameInput(u"  alice  "_q), u"alice"_q);
	CHECK_EQ(NormalizeUsernameInput(u"\talice\n"_q), u"alice"_q);
}

TEST_CASE(NormalizeStripsOneLeadingAt) {
	CHECK_EQ(NormalizeUsernameInput(u"@alice"_q), u"alice"_q);
	// A second '@' survives normalisation and fails the pattern later:
	// it is a typo, not something to silently swallow.
	CHECK_EQ(NormalizeUsernameInput(u"@@alice"_q), u"@alice"_q);
	CHECK_EQ(NormalizeUsernameInput(u"alice"_q), u"alice"_q);
}

TEST_CASE(ValidUsernamesAtBoundaries) {
	CHECK(IsValidUsername(u"Ab"_q,
						  kPublishedUsernamePolicy)); // 2 chars, the shortest.
	CHECK(IsValidUsername(u"abcdefghijklmnopqrstuvwxyz012345"_q,
						  kPublishedUsernamePolicy)); // 32 chars, the longest.
	CHECK(IsValidUsername(u"abcd"_q, kPublishedUsernamePolicy));
	CHECK(!IsValidUsername(u"a"_q,
						   kPublishedUsernamePolicy)); // 1 char, one too short.
	CHECK(!IsValidUsername(u"abcdefghijklmnopqrstuvwxyz0123456"_q,
						   kPublishedUsernamePolicy)); // 33 chars.
}

TEST_CASE(UsernamesRejectedByPattern) {
	CHECK(!IsValidUsername(
		u"1alice"_q,
		kPublishedUsernamePolicy)); // Must start with a letter.
	CHECK(!IsValidUsername(u"_alice"_q, kPublishedUsernamePolicy));
	CHECK(
		!IsValidUsername(u"al-ice"_q,
						 kPublishedUsernamePolicy)); // Dash outside the class.
	CHECK(!IsValidUsername(
		u"al ice"_q,
		kPublishedUsernamePolicy)); // Inner space is not trimmed away.
	CHECK(!IsValidUsername(u"alice!"_q, kPublishedUsernamePolicy));
	CHECK(!IsValidUsername(
		QString(),
		kPublishedUsernamePolicy)); // Empty is its own message.
	CHECK(!IsValidUsername(u"ali\u0441e"_q,
						   kPublishedUsernamePolicy)); // Non-ASCII inside.
}

TEST_CASE(MissingUsernamePolicyLeavesValidationToServer) {
	const auto policy = UsernamePolicy();
	CHECK(IsValidUsername(u"a"_q, policy));
	CHECK(IsValidUsername(u"not a username"_q, policy));
	CHECK(!IsValidUsername(QString(), policy));
}

TEST_CASE(UsernamePolicyUsesPublishedValues) {
	CHECK(IsValidUsername(u"adam"_q, kPublishedUsernamePolicy));

	const auto minFive = UsernamePolicy{
		.minLength = 5,
		.maxLength = 32,
		.pattern = u"^[a-zA-Z][a-zA-Z0-9_]{1,31}$"_q,
	};
	CHECK(!IsValidUsername(u"adam"_q, minFive));
	CHECK(IsValidUsername(u"adams"_q, minFive));

	const auto limited = UsernamePolicy{
		.minLength = 2,
		.maxLength = 4,
		.pattern = u"^[a-zA-Z][a-zA-Z0-9_]{1,31}$"_q,
	};
	CHECK(!IsValidUsername(u"adam5"_q, limited));
	CHECK(IsValidUsername(u"adam"_q, limited));

	const auto lettersOnly = UsernamePolicy{
		.pattern = u"^[a-zA-Z]{2,32}$"_q,
	};
	CHECK(!IsValidUsername(u"adam_1"_q, lettersOnly));
	CHECK(IsValidUsername(u"adam"_q, lettersOnly));
}

TEST_CASE(ServerTextLengthPolicyUsesPublishedBounds) {
	CHECK(Data::details::IsWithinInputLengthPolicy(u"ab"_q, 2, 4));
	CHECK(!Data::details::IsWithinInputLengthPolicy(u"a"_q, 2, 4));
	CHECK(!Data::details::IsWithinInputLengthPolicy(u"abcde"_q, 2, 4));
}

TEST_CASE(MissingServerTextLengthPolicyLeavesValidationToServer) {
	CHECK(Data::details::IsWithinInputLengthPolicy(u"any length"_q, 0, 0));
	CHECK(Data::details::IsWithinInputLengthPolicy(QString(), 0, 0));
	CHECK(!Data::details::ExceedsInputLengthLimit(1000, 0));
	CHECK_EQ(Data::details::InputLengthLimitForSplitting(0),
			 std::numeric_limits<int>::max());
}

TEST_CASE(ServerTextLengthPolicyCountsUnicodeScalars) {
	const auto astral = QString::fromUtf8("\xF0\x9F\x98\x80").repeated(2);
	CHECK(Data::details::IsWithinInputLengthPolicy(astral, 2, 2));
	CHECK(!Data::details::IsWithinInputLengthPolicy(astral + u"a"_q, 2, 2));
	CHECK(!Data::details::ExceedsInputLengthLimit(astral, 2));
	CHECK(Data::details::ExceedsInputLengthLimit(astral + u"a"_q, 2));
	CHECK(!Data::details::ExceedsInputLengthLimit(
		Data::details::UnicodeScalarCount(astral), 2));
	CHECK(Data::details::ExceedsInputLengthLimit(
		Data::details::UnicodeScalarCount(astral + u"a"_q), 2));
}

TEST_CASE(ServerDisplayNameLengthPolicyChecksFieldsIndividually) {
	const auto first = QString(40, QChar('x'));
	const auto last = QString(40, QChar('y'));
	CHECK(Data::details::AreWithinInputLengthPolicy(first, last, 0, 64));
	CHECK(!Data::details::IsWithinInputLengthPolicy(first + QChar(' ') + last,
													0, 64));
}

TEST_CASE(WireFormIsLowercased) {
	CHECK_EQ(ToWireUsername(u"Alice"_q), u"alice"_q);
	CHECK_EQ(ToWireUsername(u"Alice_1B"_q), u"alice_1b"_q);
}

const auto kServerA = UsernameServerIdentity{ u"10.0.0.1"_q, 443, 111 };
const auto kServerB = UsernameServerIdentity{ u"10.0.0.2"_q, 443, 222 };

TEST_CASE(CacheIsFreshOnlyForSameNameInsideWindow) {
	const auto name = u"alice"_q;
	const auto cache = UsernameCodeCache{
		name,
		kServerA,
		QByteArray("hash"),
		1000,
	};

	CHECK(cache.freshFor(name, kServerA, 1000));
	// The window is four minutes: checked as literals so a change to
	// kFreshForMs cannot move the goalposts of its own test.
	constexpr auto kFourMinutesMs = qint64(4) * 60 * 1000;
	CHECK(cache.freshFor(name, kServerA, 1000 + kFourMinutesMs - 1));
	// codeTTL on the server is 5 minutes; the cache holds only 4, so a
	// hash that is still valid on the wire is not reused past the window.
	CHECK(!cache.freshFor(name, kServerA, 1000 + kFourMinutesMs));

	// Another username reissues.
	CHECK(!cache.freshFor(u"bob"_q, kServerA, 1001));
}

TEST_CASE(CacheNeverCrossesToAnotherServer) {
	const auto name = u"alice"_q;
	const auto cache = UsernameCodeCache{
		name,
		kServerA,
		QByteArray("hash"),
		1000,
	};

	// Back to the server step, commit a different pair, submit the same
	// name inside the window: server A's hash must not reach server B.
	CHECK(!cache.freshFor(name, kServerB, 1001));
	// One differing component of the triple is enough.
	CHECK(!cache.freshFor(
		name,
		UsernameServerIdentity{ u"10.0.0.1"_q, 443, 999 },
		1001));
	CHECK(!cache.freshFor(
		name,
		UsernameServerIdentity{ u"10.0.0.1"_q, 8443, 111 },
		1001));
	CHECK(!cache.freshFor(
		name,
		UsernameServerIdentity{ u"10.0.0.9"_q, 443, 111 },
		1001));

	// Fails closed when either side has no usable server identity.
	CHECK(!cache.freshFor(name, UsernameServerIdentity(), 1001));
	const auto noServer = UsernameCodeCache{
		name,
		UsernameServerIdentity(),
		QByteArray("hash"),
		1000,
	};
	CHECK(!noServer.freshFor(name, kServerA, 1001));
}

TEST_CASE(ServerIdentityIsEmptyWithoutAllThreeParts) {
	CHECK(!kServerA.empty());
	CHECK(UsernameServerIdentity().empty());
	const auto noIp = UsernameServerIdentity{ QString(), 443, 111 };
	const auto noPort = UsernameServerIdentity{ u"10.0.0.1"_q, 0, 111 };
	// No pinned key fingerprint is no identity at all.
	const auto noKey = UsernameServerIdentity{ u"10.0.0.1"_q, 443, 0 };
	CHECK(noIp.empty());
	CHECK(noPort.empty());
	CHECK(noKey.empty());
}

TEST_CASE(DroppedAndEmptyCachesNeverLookFresh) {
	auto cache = UsernameCodeCache{
		u"alice"_q,
		kServerA,
		QByteArray("hash"),
		1000,
	};
	cache.drop();
	CHECK(cache.hash.isEmpty());
	CHECK(!cache.freshFor(u"alice"_q, kServerA, 1000));

	const auto neverIssued = UsernameCodeCache();
	CHECK(!neverIssued.freshFor(u"alice"_q, kServerA, 1000));
}

TEST_CASE(FloodWaitSecondsReadsDigitsFromTheEnd) {
	CHECK_EQ(FloodWaitSeconds(u"FLOOD_WAIT_42"_q), 42);
	// The whole family, not just the fixed FLOOD_WAIT_ prefix length:
	// MTP::IsFloodError matches this one too and a fixed offset read 0.
	CHECK_EQ(FloodWaitSeconds(u"FLOOD_PREMIUM_WAIT_42"_q), 42);
	CHECK_EQ(FloodWaitSeconds(u"FLOOD_WAIT_0"_q), 0);
	CHECK_EQ(FloodWaitSeconds(u"FLOOD_WAIT_3600"_q), 3600);
	// No trailing digits at all degrades to 0 rather than misreading.
	CHECK_EQ(FloodWaitSeconds(u"FLOOD_WAIT_"_q), 0);
	CHECK_EQ(FloodWaitSeconds(QString()), 0);
}

TEST_CASE(SignupNameValidationTrimsAndCapsInput) {
	CHECK(ValidateSignupName(u"  Ada Lovelace  "_q, 60)
		  == SignupNameValidation::Valid);
	CHECK(ValidateSignupName(QString(60, QChar('x')), 60)
		  == SignupNameValidation::Valid);
	CHECK(ValidateSignupName(QString(61, QChar('x')), 60)
		  == SignupNameValidation::TooLong);
	CHECK(ValidateSignupName(u" \t\n"_q, 60) == SignupNameValidation::Empty);
}

TEST_CASE(SignupNameValidationDoesNotImposeCompositionRules) {
	CHECK(ValidateSignupName(u"  你好\u200f  "_q, 64)
		  == SignupNameValidation::Valid);
}

TEST_CASE(SignupNameValidationUsesServerDisplayNameLimit) {
	CHECK(ValidateSignupName(QString(64, QChar('x')), 64)
		  == SignupNameValidation::Valid);
	CHECK(ValidateSignupName(QString(65, QChar('x')), 64)
		  == SignupNameValidation::TooLong);
	CHECK(ValidateSignupName(QString(65, QChar('x')), 0)
		  == SignupNameValidation::Valid);
}

TEST_CASE(SignupNameValidationCountsUnicodeScalars) {
	CHECK(ValidateSignupName(AstralString(60), 60)
		  == SignupNameValidation::Valid);
	CHECK(ValidateSignupName(AstralString(61), 60)
		  == SignupNameValidation::TooLong);
}

TEST_CASE(SignupPasswordValidationAllowsShortPasswords) {
	CHECK(
		ValidateSignupPassword(QString(), QString())
		== SignupPasswordValidation::Empty);
	CHECK(ValidateSignupPassword(u"short"_q, u"short"_q)
		  == SignupPasswordValidation::Valid);
	const auto longPassword = QString(300, QChar('x'));
	CHECK(ValidateSignupPassword(longPassword, longPassword)
		  == SignupPasswordValidation::Valid);
	CHECK(
		ValidateSignupPassword(u"long enough"_q, QString())
		== SignupPasswordValidation::RepeatEmpty);
	CHECK(
		ValidateSignupPassword(u"long enough"_q, u"different"_q)
		== SignupPasswordValidation::Mismatch);
	CHECK(
		ValidateSignupPassword(u"long enough"_q, u"long enough"_q)
		== SignupPasswordValidation::Valid);
}

TEST_CASE(SignupPasswordValidationHasNoUnicodeLengthMinimum) {
	CHECK(ValidateSignupPassword(AstralString(1), AstralString(1))
		  == SignupPasswordValidation::Valid);
	CHECK(
		ValidateSignupPassword(AstralString(8), AstralString(8))
		== SignupPasswordValidation::Valid);
}

TEST_CASE(SignupPasswordUpdateKeepsFloodVisible) {
	CHECK(
		ClassifySignupPasswordUpdateFailure(u"FLOOD_WAIT_60"_q)
		== SignupPasswordUpdateFailure::Flood);
	CHECK(
		ClassifySignupPasswordUpdateFailure(u"FLOOD_PREMIUM_WAIT_60"_q)
		== SignupPasswordUpdateFailure::Flood);
	CHECK(
		ClassifySignupPasswordUpdateFailure(u"NEW_PASSWORD_BAD"_q)
		== SignupPasswordUpdateFailure::InvalidVerifier);
	CHECK(
		ClassifySignupPasswordUpdateFailure(u"NEW_SALT_INVALID"_q)
		== SignupPasswordUpdateFailure::InvalidVerifier);
	CHECK(
		ClassifySignupPasswordUpdateFailure(u"INTERNAL"_q)
		== SignupPasswordUpdateFailure::Other);
}

TEST_CASE(SigninPasswordValidationRejectsEmptyInput) {
	CHECK(
		ValidateSigninPassword(QString())
		== SigninPasswordValidation::Empty);
	CHECK(
		ValidateSigninPassword(u"correct horse battery staple"_q)
		== SigninPasswordValidation::Valid);
}

TEST_CASE(SigninPasswordFailuresHaveExplicitFallbacks) {
	CHECK(
		ClassifySigninPasswordFailure(u"PASSWORD_HASH_INVALID"_q)
		== SigninPasswordFailure::WrongPassword);
	CHECK(
		ClassifySigninPasswordFailure(u"FLOOD_WAIT_60"_q)
		== SigninPasswordFailure::Flood);
	CHECK(
		ClassifySigninPasswordFailure(u"FLOOD_PREMIUM_WAIT_60"_q)
		== SigninPasswordFailure::Flood);
	CHECK(
		ClassifySigninPasswordFailure(u"SRP_PASSWORD_CHANGED"_q)
		== SigninPasswordFailure::WrongPassword);
	CHECK(
		ClassifySigninPasswordFailure(u"SRP_ID_INVALID"_q)
		== SigninPasswordFailure::SrpIdInvalid);
	CHECK(
		ClassifySigninPasswordFailure(u"UNEXPECTED_ERROR"_q)
		== SigninPasswordFailure::Other);
}

TEST_CASE(SigninPasswordDataFailuresKeepFloodVisible) {
	CHECK(
		ClassifySigninPasswordDataFailure(u"FLOOD_WAIT_60"_q)
		== SigninPasswordDataFailure::Flood);
	CHECK(
		ClassifySigninPasswordDataFailure(u"FLOOD_PREMIUM_WAIT_60"_q)
		== SigninPasswordDataFailure::Flood);
	CHECK(
		ClassifySigninPasswordDataFailure(u"UNEXPECTED_ERROR"_q)
		== SigninPasswordDataFailure::Other);
}

} // namespace
