#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

struct Value {
	enum class Type { Null, Boolean, Number, String, Array, Object };
	Type type = Type::Null;
	bool boolean = false;
	std::string text;
	std::vector<Value> array;
	std::map<std::string, Value> object;

	static Value Boolean(bool value) {
		auto result = Value();
		result.type = Type::Boolean;
		result.boolean = value;
		return result;
	}
	static Value Number(std::string value) {
		auto result = Value();
		result.type = Type::Number;
		result.text = std::move(value);
		return result;
	}
	static Value String(std::string value) {
		auto result = Value();
		result.type = Type::String;
		result.text = std::move(value);
		return result;
	}
	static Value Array(std::vector<Value> value) {
		auto result = Value();
		result.type = Type::Array;
		result.array = std::move(value);
		return result;
	}
	static Value Object(std::map<std::string, Value> value) {
		auto result = Value();
		result.type = Type::Object;
		result.object = std::move(value);
		return result;
	}
};

class Parser final {
public:
	explicit Parser(std::string_view input) : _input(input) {}

	Value parse() {
		skip();
		auto result = value();
		skip();
		if (_position != _input.size()) throw std::runtime_error("trailing JSON bytes");
		return result;
	}

private:
	void skip() {
		while (_position < _input.size()) {
			const auto c = _input[_position];
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
			++_position;
		}
	}
	char take() {
		if (_position >= _input.size()) throw std::runtime_error("unexpected JSON end");
		return _input[_position++];
	}
	void expect(char expected) {
		if (take() != expected) throw std::runtime_error("unexpected JSON token");
	}
	void consume(std::string_view expected) {
		if (_input.substr(_position, expected.size()) != expected) {
			throw std::runtime_error("invalid JSON literal");
		}
		_position += expected.size();
	}
	static void appendUtf8(std::string &out, unsigned code) {
		if (code < 0x80) {
			out.push_back(char(code));
		} else if (code < 0x800) {
			out.push_back(char(0xC0 | (code >> 6)));
			out.push_back(char(0x80 | (code & 0x3F)));
		} else {
			out.push_back(char(0xE0 | (code >> 12)));
			out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(char(0x80 | (code & 0x3F)));
		}
	}
	std::string string() {
		expect('"');
		auto out = std::string();
		while (_position < _input.size()) {
			const auto c = take();
			if (c == '"') return out;
			if (c != '\\') {
				if (static_cast<unsigned char>(c) < 0x20) throw std::runtime_error("control byte");
				out.push_back(c);
				continue;
			}
			switch (take()) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				unsigned code = 0;
				for (int i = 0; i != 4; ++i) {
					const auto digit = take();
					code <<= 4;
					if (digit >= '0' && digit <= '9') code |= unsigned(digit - '0');
					else if (digit >= 'a' && digit <= 'f') code |= unsigned(digit - 'a' + 10);
					else if (digit >= 'A' && digit <= 'F') code |= unsigned(digit - 'A' + 10);
					else throw std::runtime_error("invalid unicode escape");
				}
				appendUtf8(out, code);
				break;
			}
			default: throw std::runtime_error("invalid JSON escape");
			}
		}
		throw std::runtime_error("unterminated JSON string");
	}
	Value number() {
		const auto start = _position;
		if (_position < _input.size() && _input[_position] == '-') ++_position;
		if (_position >= _input.size()) throw std::runtime_error("invalid JSON number");
		if (_input[_position] == '0') ++_position;
		else if (_input[_position] >= '1' && _input[_position] <= '9') {
			while (_position < _input.size() && _input[_position] >= '0' && _input[_position] <= '9') ++_position;
		} else throw std::runtime_error("invalid JSON number");
		if (_position < _input.size() && _input[_position] == '.') {
			++_position;
			if (_position >= _input.size() || _input[_position] < '0' || _input[_position] > '9') {
				throw std::runtime_error("invalid fraction");
			}
			while (_position < _input.size() && _input[_position] >= '0' && _input[_position] <= '9') ++_position;
		}
		if (_position < _input.size() && (_input[_position] == 'e' || _input[_position] == 'E')) {
			++_position;
			if (_position < _input.size() && (_input[_position] == '+' || _input[_position] == '-')) ++_position;
			if (_position >= _input.size() || _input[_position] < '0' || _input[_position] > '9') {
				throw std::runtime_error("invalid exponent");
			}
			while (_position < _input.size() && _input[_position] >= '0' && _input[_position] <= '9') ++_position;
		}
		return Value::Number(std::string(_input.substr(start, _position - start)));
	}
	Value value() {
		if (_position >= _input.size()) throw std::runtime_error("missing JSON value");
		const auto c = _input[_position];
		if (c == '"') return Value::String(string());
		if (c == 't') { consume("true"); return Value::Boolean(true); }
		if (c == 'f') { consume("false"); return Value::Boolean(false); }
		if (c == 'n') { consume("null"); return Value(); }
		if (c == '[') {
			++_position;
			skip();
			auto items = std::vector<Value>();
			if (_position < _input.size() && _input[_position] == ']') { ++_position; return Value::Array({}); }
			while (true) {
				skip();
				items.push_back(value());
				skip();
				const auto separator = take();
				if (separator == ']') break;
				if (separator != ',') throw std::runtime_error("invalid array separator");
			}
			return Value::Array(std::move(items));
		}
		if (c == '{') {
			++_position;
			skip();
			auto items = std::map<std::string, Value>();
			if (_position < _input.size() && _input[_position] == '}') { ++_position; return Value::Object({}); }
			while (true) {
				skip();
				auto key = string();
				skip();
				expect(':');
				skip();
				items[std::move(key)] = value();
				skip();
				const auto separator = take();
				if (separator == '}') break;
				if (separator != ',') throw std::runtime_error("invalid object separator");
			}
			return Value::Object(std::move(items));
		}
		return number();
	}
	std::string_view _input;
	size_t _position = 0;
};

std::string quote(std::string_view value) {
	auto out = std::string("\"");
	for (const auto raw : value) {
		const auto c = static_cast<unsigned char>(raw);
		if (c == '"') out += "\\\"";
		else if (c == '\\') out += "\\\\";
		else if (c < 0x20) {
			static constexpr char digits[] = "0123456789abcdef";
			out += "\\u00";
			out.push_back(digits[c >> 4]);
			out.push_back(digits[c & 15]);
		} else out.push_back(raw);
	}
	out.push_back('"');
	return out;
}

std::string encode(const Value &value) {
	switch (value.type) {
	case Value::Type::Null: return "null";
	case Value::Type::Boolean: return value.boolean ? "true" : "false";
	case Value::Type::Number:
		if (value.text.find_first_of(".eE") != std::string::npos) throw std::runtime_error("noninteger number");
		return value.text == "-0" ? "0" : value.text;
	case Value::Type::String: return quote(value.text);
	case Value::Type::Array: {
		auto out = std::string("[");
		for (size_t i = 0; i != value.array.size(); ++i) {
			if (i) out.push_back(',');
			out += encode(value.array[i]);
		}
		out.push_back(']');
		return out;
	}
	case Value::Type::Object: {
		auto out = std::string("{");
		bool first = true;
		for (const auto &[key, item] : value.object) {
			if (!first) out.push_back(',');
			first = false;
			out += quote(key) + ":" + encode(item);
		}
		out.push_back('}');
		return out;
	}
	}
	throw std::runtime_error("unknown JSON type");
}

Value object(std::map<std::string, Value> fields) { return Value::Object(std::move(fields)); }
Value string(std::string text) { return Value::String(std::move(text)); }
Value number(int number) { return Value::Number(std::to_string(number)); }
const Value *get(const Value &value, const std::string &key) {
	if (value.type != Value::Type::Object) return nullptr;
	const auto i = value.object.find(key);
	return i == value.object.end() ? nullptr : &i->second;
}
const std::string &stringField(const Value &value, const std::string &key) {
	const auto found = get(value, key);
	if (!found || found->type != Value::Type::String) throw std::runtime_error("missing string field " + key);
	return found->text;
}
bool numberIs(const Value &value, const std::string &key, const std::string &expected) {
	const auto found = get(value, key);
	return found && found->type == Value::Type::Number && found->text == expected;
}
bool exactKeys(const Value &value, const std::set<std::string> &expected) {
	if (value.type != Value::Type::Object || value.object.size() != expected.size()) return false;
	auto actual = std::set<std::string>();
	for (const auto &entry : value.object) actual.insert(entry.first);
	return actual == expected;
}

bool asciiString(const std::string &value, size_t limit = 128) {
	if (value.empty() || value.size() > limit) return false;
	for (const auto raw : value) {
		const auto c = static_cast<unsigned char>(raw);
		if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
	}
	return true;
}
bool asciiTree(const Value &value) {
	switch (value.type) {
	case Value::Type::Null:
	case Value::Type::Boolean: return false;
	case Value::Type::Number: return value.text.find_first_of(".eE") == std::string::npos;
	case Value::Type::String: return asciiString(value.text);
	case Value::Type::Array:
		return std::all_of(value.array.begin(), value.array.end(), asciiTree);
	case Value::Type::Object:
		return std::all_of(value.object.begin(), value.object.end(), [](const auto &entry) {
			return asciiString(entry.first) && asciiTree(entry.second);
		});
	}
	return false;
}
bool lowerHex(const std::string &value, size_t length) {
	if (value.size() != length) return false;
	return std::all_of(value.begin(), value.end(), [](unsigned char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}
bool keyId(const std::string &value) {
	static const auto pattern = std::regex("[a-z0-9][a-z0-9-]{0,31}");
	return std::regex_match(value, pattern);
}
bool isStringField(const Value &value, const std::string &field) {
	const auto entry = get(value, field);
	return entry && entry->type == Value::Type::String;
}
bool counter(const Value *value, uint64_t *parsed = nullptr) {
	if (!value || value->type != Value::Type::String) return false;
	static const auto pattern = std::regex("0|[1-9][0-9]{0,19}");
	if (!std::regex_match(value->text, pattern)) return false;
	uint64_t result = 0;
	for (const auto digit : value->text) {
		const auto n = unsigned(digit - '0');
		if (result > (UINT64_MAX - n) / 10) return false;
		result = result * 10 + n;
	}
	if (parsed) *parsed = result;
	return true;
}
bool digestField(const Value &value, const std::string &field) {
	const auto entry = get(value, field);
	return entry && entry->type == Value::Type::String && lowerHex(entry->text, 64);
}

std::string schemaError(const std::string &kind, const Value &value) {
	const auto sKeys = std::set<std::string>{
		"allocation_checkpoint_sha256", "authorized_package_keys", "format", "from_epoch",
		"kind", "previous_statement_sha256", "product", "repo", "repo_id",
		"revoked_key_ids", "to_epoch"};
	const auto mKeys = std::set<std::string>{
		"arch", "asset_name", "asset_sha256", "asset_size", "build", "channel", "commit",
		"epoch_statement_sha256", "format", "key_epoch", "key_id", "min_os", "product",
		"repo", "version"};
	const auto cKeys = std::set<std::string>{
		"current_epoch", "epoch_statement_sha256", "event_cursor", "high_water_build", "kind",
		"ledger_head_sha256", "prev_sha256", "product", "protection_digest_sha256", "repo",
		"repo_id", "schema", "sequence"};
	if (!asciiTree(value) || !exactKeys(value, kind == "S" ? sKeys : (kind == "M" ? mKeys : cKeys))) return "schema";
	if (kind == "S") {
		for (const auto field : {"kind", "repo", "repo_id", "product", "from_epoch", "to_epoch",
			"previous_statement_sha256", "allocation_checkpoint_sha256"}) {
			if (!isStringField(value, field)) return "schema";
		}
		if (stringField(value, "kind") != "teagram-key-epoch" || !numberIs(value, "format", "1")
			|| stringField(value, "repo") != "teagramhq/teagram-desktop"
			|| stringField(value, "repo_id") != "1332987415"
			|| stringField(value, "product") != "io.teagram.desktop"
			|| !counter(get(value, "repo_id")) || !counter(get(value, "from_epoch"))
			|| !counter(get(value, "to_epoch"))
			|| !digestField(value, "previous_statement_sha256")
			|| !digestField(value, "allocation_checkpoint_sha256")) return "schema";
		const auto keys = get(value, "authorized_package_keys");
		const auto revoked = get(value, "revoked_key_ids");
		if (!keys || keys->type != Value::Type::Array || keys->array.empty() || keys->array.size() > 4
			|| !revoked || revoked->type != Value::Type::Array || revoked->array.size() > 4) return "schema";
		auto ids = std::vector<std::string>();
		for (const auto &entry : keys->array) {
			if (!exactKeys(entry, {"algorithm", "id", "public_key"})
				|| !isStringField(entry, "algorithm") || !isStringField(entry, "id")
				|| !isStringField(entry, "public_key")
				|| stringField(entry, "algorithm") != "Ed25519"
				|| !keyId(stringField(entry, "id"))
				|| !lowerHex(stringField(entry, "public_key"), 64)) return "schema";
			ids.push_back(stringField(entry, "id"));
		}
		if (!std::is_sorted(ids.begin(), ids.end()) || std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return "schema";
		auto revokedIds = std::vector<std::string>();
		for (const auto &entry : revoked->array) {
			if (entry.type != Value::Type::String || !keyId(entry.text)) return "schema";
			revokedIds.push_back(entry.text);
		}
		if (!std::is_sorted(revokedIds.begin(), revokedIds.end())
			|| std::adjacent_find(revokedIds.begin(), revokedIds.end()) != revokedIds.end()) return "schema";
		for (const auto &id : ids) {
			if (std::binary_search(revokedIds.begin(), revokedIds.end(), id)) return "schema";
		}
	} else if (kind == "M") {
		for (const auto field : {"repo", "product", "arch", "channel", "asset_sha256", "commit",
			"epoch_statement_sha256", "key_id", "asset_name", "version", "min_os"}) {
			const auto entry = get(value, field);
			if (!entry || entry->type != Value::Type::String) return "schema";
		}
		if (!numberIs(value, "format", "2")
			|| stringField(value, "repo") != "teagramhq/teagram-desktop"
			|| stringField(value, "product") != "io.teagram.desktop"
			|| stringField(value, "arch") != "arm64"
			|| (stringField(value, "channel") != "dev" && stringField(value, "channel") != "main")
			|| !counter(get(value, "build")) || !counter(get(value, "key_epoch"))
			|| !counter(get(value, "asset_size"))
			|| !lowerHex(stringField(value, "asset_sha256"), 64)
			|| !lowerHex(stringField(value, "commit"), 40)
			|| !digestField(value, "epoch_statement_sha256")
			|| !keyId(stringField(value, "key_id"))
			|| !std::regex_match(stringField(value, "asset_name"), std::regex("Teagram-macOS-arm64-[0-9]+\\.zip"))
			|| !asciiString(stringField(value, "version"), 32)
			|| !asciiString(stringField(value, "min_os"), 32)) return "schema";
	} else {
		for (const auto field : {"current_epoch", "epoch_statement_sha256", "event_cursor", "high_water_build",
			"kind", "ledger_head_sha256", "prev_sha256", "product", "protection_digest_sha256",
			"repo", "repo_id", "sequence"}) {
			if (!isStringField(value, field)) return "schema";
		}
		if (stringField(value, "kind") != "teagram-ledger-checkpoint" || !numberIs(value, "schema", "1")
			|| stringField(value, "repo") != "teagramhq/teagram-desktop"
			|| stringField(value, "repo_id") != "1332987415"
			|| stringField(value, "product") != "io.teagram.desktop") return "schema";
		for (const auto field : {"repo_id", "sequence", "event_cursor", "high_water_build", "current_epoch"}) {
			if (!counter(get(value, field))) return "schema";
		}
		for (const auto field : {"prev_sha256", "ledger_head_sha256", "epoch_statement_sha256", "protection_digest_sha256"}) {
			if (!digestField(value, field)) return "schema";
		}
	}
	return "ok";
}

std::vector<unsigned char> fromHex(const std::string &hex) {
	if (hex.size() % 2) throw std::runtime_error("odd hex length");
	auto out = std::vector<unsigned char>();
	for (size_t i = 0; i != hex.size(); i += 2) {
		const auto parse = [](char c) -> unsigned {
			if (c >= '0' && c <= '9') return unsigned(c - '0');
			if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
			if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
			throw std::runtime_error("invalid hex");
		};
		out.push_back(static_cast<unsigned char>((parse(hex[i]) << 4) | parse(hex[i + 1])));
	}
	return out;
}
std::string hex(const std::vector<unsigned char> &bytes) {
	static constexpr char digits[] = "0123456789abcdef";
	auto out = std::string();
	for (const auto byte : bytes) { out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]); }
	return out;
}
std::vector<unsigned char> fieldHex(const Value &value, const std::string &field) {
	return fromHex(stringField(value, field));
}
std::string readFile(const std::string &path) {
	auto file = std::ifstream(path, std::ios::binary);
	if (!file) throw std::runtime_error("could not read " + path);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct RfcVector {
	std::vector<unsigned char> seed;
	std::vector<unsigned char> publicKey;
	std::vector<unsigned char> message;
	std::vector<unsigned char> signature;
};
std::map<std::string, RfcVector> RFC;
std::map<std::string, RfcVector> loadRfc() {
	const auto root = Parser(readFile("rfc8032-vectors.json")).parse();
	if (root.type != Value::Type::Object) throw std::runtime_error("RFC vector data must be an object");
	auto result = std::map<std::string, RfcVector>();
	for (const auto &entry : root.object) {
		result.emplace(entry.first, RfcVector{
			fieldHex(entry.second, "seed"), fieldHex(entry.second, "public_key"),
			fieldHex(entry.second, "message"), fieldHex(entry.second, "signature")});
	}
	return result;
}

const std::map<std::string, std::string> ROLE_TEST = {
	{"K0", "2"}, {"K1", "3"}, {"L", "1024"}, {"R", "1"}};
const std::string DOMAIN_S = "teagram-key-epoch-v1";
const std::string DOMAIN_M = "teagram-update-v2";
const std::string DOMAIN_C = "teagram-ledger-checkpoint-v1";
const std::string DOMAIN_OLD = "teagram-update-v1";
const std::string ZERO(64, '0');
const size_t MAX_OBJECT = 16 * 1024;
const uint64_t MAX_UINT64 = UINT64_MAX;

std::string domainFor(const std::string &kind) {
	if (kind == "S") return DOMAIN_S;
	if (kind == "M") return DOMAIN_M;
	if (kind == "C") return DOMAIN_C;
	throw std::runtime_error("unknown object kind");
}
std::string domainMessage(const std::string &domain, const std::string &payload) {
	auto message = domain;
	message.push_back('\0');
	message += payload;
	return message;
}
std::vector<unsigned char> signRaw(const std::string &test, const std::string &message) {
	using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
	using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
	const auto &seed = RFC.at(test).seed;
	auto key = Key(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr, seed.data(), seed.size()), EVP_PKEY_free);
	if (!key) throw std::runtime_error("could not load Ed25519 private key");
	auto context = Context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
		throw std::runtime_error("could not initialize Ed25519 signing");
	}
	auto signature = std::vector<unsigned char>(64);
	auto length = signature.size();
	if (EVP_DigestSign(context.get(), signature.data(), &length,
		reinterpret_cast<const unsigned char *>(message.data()), message.size()) != 1 || length != 64) {
		throw std::runtime_error("Ed25519 signing failed");
	}
	return signature;
}
bool verifyRaw(const std::vector<unsigned char> &publicKey, const std::string &message,
	const std::vector<unsigned char> &signature) {
	using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
	using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
	auto key = Key(EVP_PKEY_new_raw_public_key_ex(nullptr, "ED25519", nullptr,
		publicKey.data(), publicKey.size()), EVP_PKEY_free);
	if (!key) throw std::runtime_error("could not load Ed25519 public key");
	auto context = Context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
		throw std::runtime_error("could not initialize Ed25519 verification");
	}
	return EVP_DigestVerify(context.get(), signature.data(), signature.size(),
		reinterpret_cast<const unsigned char *>(message.data()), message.size()) == 1;
}
std::vector<unsigned char> sign(const std::string &test, const std::string &domain, const std::string &payload) {
	return signRaw(test, domainMessage(domain, payload));
}
bool verify(const std::vector<unsigned char> &publicKey, const std::string &domain,
	const std::string &payload, const std::vector<unsigned char> &signature) {
	return verifyRaw(publicKey, domainMessage(domain, payload), signature);
}
std::string sha256(const std::string &data) {
	auto digest = std::array<unsigned char, EVP_MAX_MD_SIZE>();
	auto length = 0U;
	if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32) {
		throw std::runtime_error("SHA-256 failed");
	}
	return hex(std::vector<unsigned char>(digest.begin(), digest.begin() + length));
}
std::string publicForRole(const std::string &role) { return hex(RFC.at(ROLE_TEST.at(role)).publicKey); }

struct RfcResult {
	std::string test;
	bool publicMatches;
	bool signatureMatches;
	bool signatureVerifies;
	size_t messageBytes;
};
std::vector<RfcResult> checkRfc() {
	auto result = std::vector<RfcResult>();
	for (const auto test : {"1", "2", "3", "1024"}) {
		const auto &vector = RFC.at(test);
		using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
		auto key = Key(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr,
			vector.seed.data(), vector.seed.size()), EVP_PKEY_free);
		if (!key) throw std::runtime_error("RFC private key load failed");
		auto derived = std::vector<unsigned char>(32);
		auto derivedLength = derived.size();
		if (EVP_PKEY_get_raw_public_key(key.get(), derived.data(), &derivedLength) != 1) {
			throw std::runtime_error("RFC public key derivation failed");
		}
		derived.resize(derivedLength);
		const auto message = std::string(vector.message.begin(), vector.message.end());
		const auto signature = signRaw(test, message);
		result.push_back({test, derived == vector.publicKey, signature == vector.signature,
			verifyRaw(vector.publicKey, message, vector.signature), vector.message.size()});
	}
	return result;
}

Value makeS(const std::string &checkpointDigest, const std::string &packagePublicKey) {
	const auto key = object({
		{"algorithm", string("Ed25519")}, {"id", string("k1")},
		{"public_key", string(packagePublicKey)}});
	return object({
		{"allocation_checkpoint_sha256", string(checkpointDigest)},
		{"authorized_package_keys", Value::Array({key})}, {"format", number(1)},
		{"from_epoch", string("0")}, {"kind", string("teagram-key-epoch")},
		{"previous_statement_sha256", string(ZERO)}, {"product", string("io.teagram.desktop")},
		{"repo", string("teagramhq/teagram-desktop")}, {"repo_id", string("1332987415")},
		{"revoked_key_ids", Value::Array({string("k0")})}, {"to_epoch", string("1")}});
}
Value makeC0() {
	return object({
		{"current_epoch", string("0")}, {"epoch_statement_sha256", string(ZERO)},
		{"event_cursor", string("0")}, {"high_water_build", string("100")},
		{"kind", string("teagram-ledger-checkpoint")},
		{"ledger_head_sha256", string(sha256("fixture-ledger-bootstrap"))},
		{"prev_sha256", string(ZERO)}, {"product", string("io.teagram.desktop")},
		{"protection_digest_sha256", string(sha256("fixture-protection-v1"))},
		{"repo", string("teagramhq/teagram-desktop")}, {"repo_id", string("1332987415")},
		{"schema", number(1)}, {"sequence", string("0")}});
}
Value makeM(const std::string &statementDigest) {
	return object({
		{"arch", string("arm64")}, {"asset_name", string("Teagram-macOS-arm64-101.zip")},
		{"asset_sha256", string(sha256("test-only-package-bytes"))}, {"asset_size", string("4096")},
		{"build", string("101")}, {"channel", string("dev")},
		{"commit", string("b14d386727689a20de057385799954960ea841e4")},
		{"epoch_statement_sha256", string(statementDigest)}, {"format", number(2)},
		{"key_epoch", string("1")}, {"key_id", string("k1")}, {"min_os", string("13.0")},
		{"product", string("io.teagram.desktop")}, {"repo", string("teagramhq/teagram-desktop")},
		{"version", string("7.0.9")}});
}
Value makeC1(const std::string &statementDigest, const std::string &previousDigest) {
	return object({
		{"current_epoch", string("1")}, {"epoch_statement_sha256", string(statementDigest)},
		{"event_cursor", string("1")}, {"high_water_build", string("101")},
		{"kind", string("teagram-ledger-checkpoint")},
		{"ledger_head_sha256", string(sha256("fixture-reservation-101"))},
		{"prev_sha256", string(previousDigest)}, {"product", string("io.teagram.desktop")},
		{"protection_digest_sha256", string(sha256("fixture-protection-v1"))},
		{"repo", string("teagramhq/teagram-desktop")}, {"repo_id", string("1332987415")},
		{"schema", number(1)}, {"sequence", string("1")}});
}
struct Objects {
	Value s, m, c0, c;
	std::string sBytes, mBytes, c0Bytes, cBytes;
};
Objects makeObjects() {
	auto c0 = makeC0();
	const auto c0Bytes = encode(c0);
	auto s = makeS(sha256(c0Bytes), publicForRole("K1"));
	const auto sBytes = encode(s);
	const auto sDigest = sha256(sBytes);
	auto m = makeM(sDigest);
	auto c = makeC1(sDigest, sha256(c0Bytes));
	return {s, m, c0, c, sBytes, encode(m), c0Bytes, encode(c)};
}
struct VectorRow { std::string domain, signer, payload, signature; };
std::map<std::string, VectorRow> makeVectors(const Objects &objects) {
	const auto row = [](const std::string &domain, const std::string &test,
		const std::string &role, const std::string &payload) {
		return VectorRow{domain, role, payload, hex(sign(test, domain, payload))};
	};
	return {
		{"S", row(DOMAIN_S, "1", "R", objects.sBytes)},
		{"M", row(DOMAIN_M, "3", "K1", objects.mBytes)},
		{"C0", row(DOMAIN_C, "1024", "L", objects.c0Bytes)},
		{"C", row(DOMAIN_C, "1024", "L", objects.cBytes)}};
}

struct MContext {
	std::string epoch = "1";
	std::map<std::string, std::string> authorizedPackageKeys;
	std::string statementDigest;
	uint64_t installedEpoch = 0;
	uint64_t installedBuild = MAX_UINT64;
	bool mainChannel = false;
};
std::map<std::string, std::string> packageKeyMap(const Value &statement) {
	const auto keys = get(statement, "authorized_package_keys");
	if (!keys || keys->type != Value::Type::Array) {
		throw std::runtime_error("epoch statement package keys are not an array");
	}
	auto result = std::map<std::string, std::string>();
	for (const auto &entry : keys->array) {
		result.emplace(stringField(entry, "id"), stringField(entry, "public_key"));
	}
	return result;
}
struct SContext {
	uint64_t floor = 0;
	std::string previousDigest = ZERO;
	std::map<std::string, std::string> known;
	std::map<std::string, std::string> keyHistory;
	std::set<std::string> cumulativeRevocations;
};
SContext advanceStatementContext(const SContext &context, const Value &statement, const std::string &payload) {
	auto result = context;
	uint64_t toEpoch = 0;
	if (!counter(get(statement, "to_epoch"), &toEpoch)) {
		throw std::runtime_error("accepted epoch statement has invalid to_epoch");
	}
	const auto statementDigest = sha256(payload);
	result.floor = toEpoch;
	result.previousDigest = statementDigest;
	result.known[std::to_string(toEpoch)] = statementDigest;
	for (const auto &[id, publicKey] : packageKeyMap(statement)) {
		result.keyHistory[id] = publicKey;
	}
	const auto revoked = get(statement, "revoked_key_ids");
	if (!revoked || revoked->type != Value::Type::Array) {
		throw std::runtime_error("accepted epoch statement has invalid revocations");
	}
	for (const auto &entry : revoked->array) {
		if (entry.type != Value::Type::String) {
			throw std::runtime_error("accepted epoch statement has invalid revocation ID");
		}
		result.cumulativeRevocations.insert(entry.text);
	}
	return result;
}
struct Outcome { bool rejected = true; bool signatureValid = false; bool parseReached = false; std::string stage = "signature"; };

std::pair<std::string, std::string> findSigner(const std::string &domain,
	const std::string &payload, const std::vector<unsigned char> &signature) {
	if (payload.size() > MAX_OBJECT) return {"", "size"};
	if (signature.size() != 64) return {"", "signature_length"};
	for (const auto &[role, test] : ROLE_TEST) {
		if (verify(RFC.at(test).publicKey, domain, payload, signature)) return {role, "ok"};
	}
	return {"", "signature"};
}
Outcome verifyObject(const std::string &kind, const std::string &domain,
	const std::string &payload, const std::vector<unsigned char> &signature,
	const MContext *mContext = nullptr, const SContext *sContext = nullptr) {
	auto result = Outcome();
	if (domain != domainFor(kind)) { result.stage = "domain"; return result; }
	const auto signedBy = findSigner(domainFor(kind), payload, signature);
	if (signedBy.second != "ok") { result.stage = signedBy.second; return result; }
	const auto &signer = signedBy.first;
	result.signatureValid = true;
	result.parseReached = true;
	Value value;
	try { value = Parser(payload).parse(); }
	catch (...) { result.stage = "parse"; return result; }
	try {
		if (encode(value) != payload) { result.stage = "canonical"; return result; }
	} catch (...) { result.stage = "canonical"; return result; }
	if (schemaError(kind, value) != "ok") { result.stage = "schema"; return result; }
	const auto rightRole = kind == "S" ? signer == "R"
		: (kind == "M" ? (signer == "K0" || signer == "K1") : signer == "L");
	if (!rightRole) { result.stage = "authority"; return result; }
	if (kind == "S" && sContext) {
		uint64_t fromEpoch = 0, toEpoch = 0;
		counter(get(value, "from_epoch"), &fromEpoch);
		counter(get(value, "to_epoch"), &toEpoch);
		const auto existing = sContext->known.find(std::to_string(toEpoch));
		if (existing != sContext->known.end() && existing->second != sha256(payload)) {
			result.stage = "conflict";
			return result;
		}
		if (fromEpoch != sContext->floor || fromEpoch == MAX_UINT64 || toEpoch != fromEpoch + 1
			|| stringField(value, "previous_statement_sha256") != sContext->previousDigest) {
			result.stage = "sequence";
			return result;
		}
		for (const auto &[id, publicKey] : packageKeyMap(value)) {
			if (sContext->cumulativeRevocations.count(id)) {
				result.stage = "revocation";
				return result;
			}
			const auto previousKey = sContext->keyHistory.find(id);
			if (previousKey != sContext->keyHistory.end() && previousKey->second != publicKey) {
				result.stage = "key_history";
				return result;
			}
		}
	}
	if (kind == "M") {
		if (!mContext) throw std::runtime_error("manifest requires an epoch context");
		uint64_t epoch = 0, build = 0;
		counter(get(value, "key_epoch"), &epoch);
		counter(get(value, "build"), &build);
		const auto key = stringField(value, "key_id");
		const auto authorizedKey = mContext->authorizedPackageKeys.find(key);
		if (stringField(value, "key_epoch") != mContext->epoch
			|| authorizedKey == mContext->authorizedPackageKeys.end()
			|| authorizedKey->second != publicForRole(signer)
			|| stringField(value, "epoch_statement_sha256") != mContext->statementDigest) {
			result.stage = "authority";
			return result;
		}
		if (epoch < mContext->installedEpoch
			|| (epoch == mContext->installedEpoch && build <= mContext->installedBuild)
			|| (mContext->mainChannel && stringField(value, "channel") == "dev")) {
			result.stage = "eligibility";
			return result;
		}
	}
	result.rejected = false;
	result.stage = "accepted";
	return result;
}

Outcome runCase(const std::string &kind, const std::string &domain,
	const std::string &payload, const std::string &signerTest,
	const MContext *mContext = nullptr, const SContext *sContext = nullptr,
	const std::optional<std::vector<unsigned char>> &signature = std::nullopt) {
	const auto signedBytes = signature ? *signature : sign(signerTest, domain, payload);
	return verifyObject(kind, domain, payload, signedBytes, mContext, sContext);
}
Value outcomeValue(const Outcome &outcome) {
	return object({{"parse_reached", Value::Boolean(outcome.parseReached)},
		{"rejected", Value::Boolean(outcome.rejected)},
		{"signature_valid", Value::Boolean(outcome.signatureValid)}, {"stage", string(outcome.stage)}});
}
void addCase(std::map<std::string, Value> &cases, const std::string &name, const Outcome &outcome) {
	cases[name] = outcomeValue(outcome);
}
std::string duplicatePayload(const std::string &payload) {
	auto result = payload;
	const auto target = std::string("\"format\":1");
	const auto position = result.find(target);
	if (position == std::string::npos) throw std::runtime_error("format field missing");
	result.replace(position, target.size(), target + "," + target);
	return result;
}
bool mutateFirstString(Value &value) {
	if (value.type == Value::Type::String) {
		if (value.text.empty()) return false;
		value.text[0] = char(0x7F);
		return true;
	}
	if (value.type == Value::Type::Array) {
		for (auto &item : value.array) if (mutateFirstString(item)) return true;
	} else if (value.type == Value::Type::Object) {
		for (auto &item : value.object) if (mutateFirstString(item.second)) return true;
	}
	return false;
}
size_t signedFieldFlipCases(const std::string &kind, const std::string &domain,
	const Value &original, const std::string &signer, const MContext *mContext,
	const SContext *sContext) {
	size_t count = 0;
	for (const auto &entry : original.object) {
		auto changed = original;
		auto &value = changed.object.at(entry.first);
		if (value.type == Value::Type::Number) value.text = value.text == "1" ? "2" : "1";
		else if (!mutateFirstString(value)) throw std::runtime_error("no mutable string field");
		const auto outcome = runCase(kind, domain, encode(changed), signer, mContext, sContext);
		if (!outcome.rejected || !outcome.signatureValid || !outcome.parseReached) {
			throw std::runtime_error("signed field flip escaped post-signature rejection");
		}
		++count;
	}
	return count;
}

std::map<std::string, Value> runCases(const Objects &objects) {
	const auto context0 = MContext{
		"0",
		{{"k0", publicForRole("K0")}},
		ZERO,
		0,
		MAX_UINT64,
		false};
	const auto context1 = MContext{
		"1",
		packageKeyMap(objects.s),
		sha256(objects.sBytes),
		0,
		MAX_UINT64,
		false};
	auto cases = std::map<std::string, Value>();
	const auto duplicate = duplicatePayload(objects.sBytes);
	addCase(cases, "duplicate_key_S", runCase("S", DOMAIN_S, duplicate, "1"));
	addCase(cases, "signed_duplicate_S", runCase("S", DOMAIN_S, duplicate, "1"));
	auto unknown = objects.s;
	unknown.object["unknown_field"] = string("x");
	addCase(cases, "unknown_key_S", runCase("S", DOMAIN_S, encode(unknown), "1"));
	auto badStatement = objects.s;
	badStatement.object["repo"] = number(101);
	addCase(cases, "S_repo_number", runCase("S", DOMAIN_S, encode(badStatement), "1"));
	badStatement = objects.s;
	badStatement.object["previous_statement_sha256"] = Value::Array({string(std::string(64, '0'))});
	addCase(cases, "S_previous_digest_array", runCase("S", DOMAIN_S, encode(badStatement), "1"));
	badStatement = objects.s;
	badStatement.object["authorized_package_keys"].array[0].object["public_key"] = number(101);
	addCase(cases, "S_key_public_key_number", runCase("S", DOMAIN_S, encode(badStatement), "1"));
	auto badCheckpoint = objects.c;
	badCheckpoint.object["repo"] = number(101);
	addCase(cases, "C_repo_number", runCase("C", DOMAIN_C, encode(badCheckpoint), "1024"));
	badCheckpoint = objects.c;
	badCheckpoint.object["ledger_head_sha256"] = number(101);
	addCase(cases, "C_ledger_head_digest_number", runCase("C", DOMAIN_C, encode(badCheckpoint), "1024"));
	auto leading = objects.m;
	leading.object["build"] = string("0101");
	addCase(cases, "leading_zero_build_M", runCase("M", DOMAIN_M, encode(leading), "3", &context1));
	auto numberBuild = objects.m;
	numberBuild.object["build"] = Value::Number("101");
	addCase(cases, "number_build_M", runCase("M", DOMAIN_M, encode(numberBuild), "3", &context1));
	auto overflow = objects.m;
	overflow.object["build"] = string("18446744073709551616");
	addCase(cases, "overflow_build_M", runCase("M", DOMAIN_M, encode(overflow), "3", &context1));
	auto assetNameNumber = objects.m;
	assetNameNumber.object["asset_name"] = number(101);
	addCase(cases, "M_asset_name_number", runCase("M", DOMAIN_M, encode(assetNameNumber), "3", &context1));
	auto channelArray = objects.m;
	channelArray.object["channel"] = Value::Array({string("dev")});
	addCase(cases, "M_channel_array", runCase("M", DOMAIN_M, encode(channelArray), "3", &context1));
	auto escaped = objects.mBytes;
	const auto channel = std::string("\"channel\":\"dev\"");
	const auto channelPosition = escaped.find(channel);
	escaped.replace(channelPosition, channel.size(), "\"channel\":\"d\\u0065v\"");
	addCase(cases, "escaped_string_M", runCase("M", DOMAIN_M, escaped, "3", &context1));
	addCase(cases, "trailing_newline_M", runCase("M", DOMAIN_M, objects.mBytes + "\n", "3", &context1));
	addCase(cases, "wrong_domain_S", runCase("S", DOMAIN_M, objects.sBytes, "1"));
	addCase(cases, "old_v1_domain_for_M", runCase("M", DOMAIN_M, objects.mBytes, "3", &context1,
		nullptr, sign("3", DOMAIN_OLD, objects.mBytes)));
	auto changedBytes = objects.mBytes;
	const auto flipAt = changedBytes.find("\"channel\":\"d") + std::string("\"channel\":\"d").size();
	changedBytes[flipAt] ^= 1;
	addCase(cases, "unsigned_one_byte_flip_M", runCase("M", DOMAIN_M, changedBytes, "3", &context1,
		nullptr, sign("3", DOMAIN_M, objects.mBytes)));

	auto wrongRole = std::vector<std::pair<std::string, Outcome>>();
	wrongRole.push_back({"S_by_K0", runCase("S", DOMAIN_S, objects.sBytes, "2")});
	wrongRole.push_back({"S_by_K1", runCase("S", DOMAIN_S, objects.sBytes, "3")});
	wrongRole.push_back({"S_by_ledger", runCase("S", DOMAIN_S, objects.sBytes, "1024")});
	wrongRole.push_back({"M_by_R", runCase("M", DOMAIN_M, objects.mBytes, "1", &context1)});
	wrongRole.push_back({"M_by_ledger", runCase("M", DOMAIN_M, objects.mBytes, "1024", &context1)});
	wrongRole.push_back({"C_by_R", runCase("C", DOMAIN_C, objects.cBytes, "1")});
	wrongRole.push_back({"C_by_K0", runCase("C", DOMAIN_C, objects.cBytes, "2")});
	wrongRole.push_back({"C_by_K1", runCase("C", DOMAIN_C, objects.cBytes, "3")});
	wrongRole.push_back({"M_by_K1_before_authorization", runCase("M", DOMAIN_M, objects.mBytes, "3", &context0)});
	auto package0 = objects.m;
	package0.object["build"] = string("101");
	package0.object["epoch_statement_sha256"] = string(ZERO);
	package0.object["key_epoch"] = string("0");
	package0.object["key_id"] = string("k0");
	const auto context0Authorized = MContext{
		"0",
		{{"k0", publicForRole("K0")}},
		ZERO,
		0,
		100,
		false};
	addCase(cases, "M_by_K0_when_authorized", runCase("M", DOMAIN_M, encode(package0), "2", &context0Authorized));
	wrongRole.push_back({"M_by_K0_after_revocation", runCase("M", DOMAIN_M, encode(package0), "2", &context1)});
	auto mismatchStatement = objects.s;
	const auto mismatchKey = object({
		{"algorithm", string("Ed25519")},
		{"id", string("k1")},
		{"public_key", string(publicForRole("K0"))},
	});
	mismatchStatement.object["authorized_package_keys"] = Value::Array({mismatchKey});
	mismatchStatement.object["revoked_key_ids"] = Value::Array(std::vector<Value>());
	const auto mismatchStatementBytes = encode(mismatchStatement);
	const auto mismatchStatementResult = runCase(
		"S",
		DOMAIN_S,
		mismatchStatementBytes,
		"1");
	if (mismatchStatementResult.rejected || !mismatchStatementResult.signatureValid
		|| !mismatchStatementResult.parseReached) {
		throw std::runtime_error("fixture mismatch statement must be R-authorized");
	}
	const auto mismatchStatementDigest = sha256(mismatchStatementBytes);
	const auto mismatchContext = MContext{
		"1",
		packageKeyMap(mismatchStatement),
		mismatchStatementDigest,
		0,
		MAX_UINT64,
		false};
	auto mismatchManifest = objects.m;
	mismatchManifest.object["epoch_statement_sha256"] = string(mismatchStatementDigest);
	const auto mismatchOutcome = runCase(
		"M",
		DOMAIN_M,
		encode(mismatchManifest),
		"3",
		&mismatchContext);
	addCase(
		cases,
		"M_key_id_public_key_mismatch",
		mismatchOutcome);
	auto wrongNames = std::vector<std::string>();
	for (const auto &entry : wrongRole) { addCase(cases, entry.first, entry.second); wrongNames.push_back(entry.first); }
	std::sort(wrongNames.begin(), wrongNames.end());
	auto wrongValues = std::vector<Value>();
	for (const auto &name : wrongNames) wrongValues.push_back(string(name));
	cases["wrong_role_matrix"] = Value::Array(std::move(wrongValues));

	auto skip = objects.s;
	skip.object["from_epoch"] = string("0");
	skip.object["to_epoch"] = string("2");
	const auto skipBytes = encode(skip);
	addCase(cases, "online_signed_epoch_skip", runCase("S", DOMAIN_S, skipBytes, "2"));
	const auto sequenceContext = SContext{0, ZERO, {{"1", sha256(objects.sBytes)}}, {}, {}};
	addCase(cases, "signed_epoch_skip_R", runCase("S", DOMAIN_S, skipBytes, "1", nullptr, &sequenceContext));
	auto conflict = objects.s;
	conflict.object["allocation_checkpoint_sha256"] = string(sha256("different-signed-checkpoint"));
	const auto conflictContext = SContext{0, ZERO, {{"1", sha256(objects.sBytes)}}, {}, {}};
	addCase(cases, "signed_conflicting_epoch_R", runCase("S", DOMAIN_S, encode(conflict), "1", nullptr, &conflictContext));
	auto initialStatementContext = SContext();
	initialStatementContext.keyHistory.emplace("k0", publicForRole("K0"));
	const auto initialStatement = runCase("S", DOMAIN_S, objects.sBytes, "1", nullptr, &initialStatementContext);
	if (initialStatement.rejected || !initialStatement.signatureValid || !initialStatement.parseReached) {
		throw std::runtime_error("fixture epoch 1 statement must advance from the baseline");
	}
	const auto statementContext1 = advanceStatementContext(initialStatementContext, objects.s, objects.sBytes);
	auto linked = objects.s;
	linked.object["from_epoch"] = string("1");
	linked.object["to_epoch"] = string("2");
	linked.object["previous_statement_sha256"] = string(sha256(objects.sBytes));
	linked.object["revoked_key_ids"] = Value::Array({});
	auto revokedKey = linked;
	const auto key0 = object({
		{"algorithm", string("Ed25519")}, {"id", string("k0")},
		{"public_key", string(publicForRole("K0"))}});
	revokedKey.object["authorized_package_keys"] = Value::Array({key0});
	addCase(cases, "S_reauthorizes_cumulative_revocation",
		runCase("S", DOMAIN_S, encode(revokedKey), "1", nullptr, &statementContext1));
	auto reboundKey = linked;
	const auto rebound = object({
		{"algorithm", string("Ed25519")}, {"id", string("k1")},
		{"public_key", string(publicForRole("K0"))}});
	reboundKey.object["authorized_package_keys"] = Value::Array({rebound});
	addCase(cases, "S_rebinds_historical_key_id",
		runCase("S", DOMAIN_S, encode(reboundKey), "1", nullptr, &statementContext1));
	addCase(cases, "S_accepts_linked_epoch_2",
		runCase("S", DOMAIN_S, encode(linked), "1", nullptr, &statementContext1));

	const auto flipsS = signedFieldFlipCases("S", DOMAIN_S, objects.s, "1", nullptr, nullptr);
	const auto flipsM = signedFieldFlipCases("M", DOMAIN_M, objects.m, "3", &context1, nullptr);
	const auto flipsC = signedFieldFlipCases("C", DOMAIN_C, objects.c, "1024", nullptr, nullptr);
	cases["signed_one_byte_flip_fields_S"] = number(int(flipsS));
	cases["signed_one_byte_flip_fields_M"] = number(int(flipsM));
	cases["signed_one_byte_flip_fields_C"] = number(int(flipsC));
	cases["signed_one_byte_flip_fields"] = number(int(flipsS + flipsM + flipsC));
	auto oversized = objects.mBytes;
	oversized.append(MAX_OBJECT + 1 - oversized.size(), ' ');
	addCase(cases, "oversized_object", runCase("M", DOMAIN_M, oversized, "3", &context1));
	auto shortSignature = sign("3", DOMAIN_M, objects.mBytes);
	shortSignature.pop_back();
	addCase(cases, "short_signature", runCase("M", DOMAIN_M, objects.mBytes, "3", &context1, nullptr, shortSignature));
	return cases;
}

Value vectorValue(const VectorRow &row) {
	const auto message = domainMessage(row.domain, row.payload);
	return object({{"canonical_hex", string(hex(std::vector<unsigned char>(row.payload.begin(), row.payload.end())))},
		{"domain", string(row.domain)},
		{"message_hex", string(hex(std::vector<unsigned char>(message.begin(), message.end())))},
		{"sha256", string(sha256(row.payload))}, {"signature_hex", string(row.signature)},
		{"signer", string(row.signer)}});
}
Value vectorsValue(const std::map<std::string, VectorRow> &vectors) {
	auto rows = std::map<std::string, Value>();
	for (const auto &entry : vectors) rows[entry.first] = vectorValue(entry.second);
	return object(std::move(rows));
}
Value casesValue(const std::map<std::string, Value> &cases) { return object(cases); }
Value rfcValue(const std::vector<RfcResult> &rows) {
	auto values = std::vector<Value>();
	for (const auto &row : rows) {
		values.push_back(object({{"message_bytes", number(int(row.messageBytes))},
			{"public_key_matches", Value::Boolean(row.publicMatches)},
			{"signature_matches", Value::Boolean(row.signatureMatches)},
			{"signature_verifies", Value::Boolean(row.signatureVerifies)},
			{"test", string(row.test)}}));
	}
	return Value::Array(std::move(values));
}
bool checkpointChainValid(const Objects &objects) {
	const auto c0 = Parser(objects.c0Bytes).parse();
	const auto c1 = Parser(objects.cBytes).parse();
	const auto c0Valid = !verifyObject("C", DOMAIN_C, objects.c0Bytes,
		sign("1024", DOMAIN_C, objects.c0Bytes)).rejected;
	const auto c1Valid = !verifyObject("C", DOMAIN_C, objects.cBytes,
		sign("1024", DOMAIN_C, objects.cBytes)).rejected;
	return c0Valid && c1Valid
		&& stringField(c0, "sequence") == "0" && stringField(c1, "sequence") == "1"
		&& stringField(c1, "prev_sha256") == sha256(objects.c0Bytes)
		&& stringField(c0, "high_water_build") == "100"
		&& stringField(c1, "high_water_build") == "101"
		&& stringField(c1, "current_epoch") == "1"
		&& stringField(c1, "epoch_statement_sha256") == sha256(objects.sBytes);
}
std::optional<uint64_t> reserveBuild(uint64_t highWater) {
	if (highWater == MAX_UINT64) return std::nullopt;
	return highWater + 1;
}
Value stateValue(const Objects &objects) {
	const auto initial = SContext{0, ZERO, {}, {}, {}};
	const auto statement = verifyObject("S", DOMAIN_S, objects.sBytes,
		sign("1", DOMAIN_S, objects.sBytes), nullptr, &initial);
	const auto before = uint64_t(100);
	const auto afterStatement = statement.rejected ? MAX_UINT64 : before;
	const auto reservation = reserveBuild(afterStatement);
	const auto afterReservation = reservation.value_or(MAX_UINT64);
	const auto context = MContext{
		"1",
		packageKeyMap(objects.s),
		sha256(objects.sBytes),
		0,
		MAX_UINT64,
		false};
	const auto manifest = !verifyObject("M", DOMAIN_M, objects.mBytes,
		sign("3", DOMAIN_M, objects.mBytes), &context).rejected;
	return object({
		{"checkpoint_chain_valid", Value::Boolean(checkpointChainValid(objects))},
		{"client_accepts_epoch1_build101_from_epoch0_max", Value::Boolean(manifest)},
		{"global_h_after_real_reservation", string(std::to_string(afterReservation))},
		{"global_h_after_statement", string(std::to_string(afterStatement))},
		{"global_h_before_reservation", string(std::to_string(before))},
		{"legitimate_h_max_permanently_stops", Value::Boolean(!reserveBuild(MAX_UINT64).has_value())}});
}

int main() {
	try {
		RFC = loadRfc();
		if (RFC.size() != 4 || ROLE_TEST.size() != 4) throw std::runtime_error("fixture key map must contain four vectors");
		auto roleKeys = std::set<std::string>();
		for (const auto &entry : ROLE_TEST) roleKeys.insert(hex(RFC.at(entry.second).publicKey));
		if (roleKeys.size() != 4) throw std::runtime_error("fixture authorities must use distinct public keys");
		const auto rfc = checkRfc();
		for (const auto &row : rfc) {
			if (!row.publicMatches || !row.signatureMatches || !row.signatureVerifies) {
				throw std::runtime_error("RFC 8032 TEST " + row.test + " known-answer failed");
			}
		}
		const auto objects = makeObjects();
		const auto vectors = makeVectors(objects);
		const auto cases = runCases(objects);
		const auto state = stateValue(objects);
		std::cout << encode(object({{"cases", casesValue(cases)}, {"rfc", rfcValue(rfc)},
			{"state", state}, {"vectors", vectorsValue(vectors)}})) << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
