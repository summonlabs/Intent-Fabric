// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/json.hpp"
#include "ifabric/types.hpp"
#include "ifabric_test.hpp"

#include <limits>

using namespace ifabric;

IFABRIC_TEST(core, sha256_known_vectors) {
  CHECK_EQ(Sha256::hash("").hex(),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(Sha256::hash("abc").hex(),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  Sha256 stream;
  for (int i = 0; i < 1000; ++i) {
    stream.update(std::string(1000, 'a'));
  }
  CHECK_EQ(stream.finish().hex(),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  // Incremental and one-shot hashing agree across block boundaries.
  for (std::size_t size : {std::size_t{1}, std::size_t{55}, std::size_t{56}, std::size_t{63},
                           std::size_t{64}, std::size_t{65}, std::size_t{119}, std::size_t{128}}) {
    const std::string data(size, 'x');
    Sha256 incremental;
    incremental.update(data.substr(0, size / 2));
    incremental.update(data.substr(size / 2));
    CHECK_EQ(incremental.finish().hex(), Sha256::hash(data).hex());
  }
}

IFABRIC_TEST(core, crc32_known_vector) {
  CHECK_EQ(crc32("123456789"), 0xCBF43926u);
  CHECK_EQ(crc32(""), 0u);
}

IFABRIC_TEST(core, digest_hex_round_trip) {
  const Digest digest = Sha256::hash("intent");
  const auto parsed = Digest::from_hex(digest.hex());
  REQUIRE(parsed.ok());
  CHECK_EQ(parsed.value(), digest);
  CHECK_ERROR_CODE(Digest::from_hex("zz").error(), ErrorCode::InvalidArgument);
  CHECK_ERROR_CODE(Digest::from_hex("0011").error(), ErrorCode::InvalidArgument);
}

IFABRIC_TEST(core, identity_classes_are_enforced) {
  const auto fabric = FabricId::parse("fab.core");
  REQUIRE(fabric.ok());
  CHECK_EQ(fabric.value().local(), std::string("core"));
  CHECK_EQ(fabric.value().str(), std::string("fab.core"));
  CHECK_ERROR_CODE(FabricId::parse("dev.core").error(), ErrorCode::IdentityClassMismatch);
  CHECK_ERROR_CODE(FabricId::parse("nope.core").error(), ErrorCode::IdentityMalformed);
  CHECK_ERROR_CODE(FabricId::parse("core").error(), ErrorCode::IdentityMalformed);
  CHECK_ERROR_CODE(FabricId::parse("fab.").error(), ErrorCode::IdentityMalformed);
  CHECK_ERROR_CODE(FabricId::parse("fab.-leading").error(), ErrorCode::IdentityMalformed);
  CHECK(FabricId::parse("fab.a.b-c_d").ok());
  CHECK_EQ(std::string(id_class_prefix(IdClass::Port)), std::string("port"));
  CHECK_EQ(id_class_name(IdClass::Device), std::string_view("device"));
}

IFABRIC_TEST(core, any_id_ordering_is_canonical) {
  const auto port = AnyId::parse("port.p1").value();
  const auto device = AnyId::parse("dev.d1").value();
  CHECK(device < port);  // Device (5) sorts before Port (6)
  CHECK_EQ(port.str(), std::string("port.p1"));
  const auto round_trip = AnyId::parse(port.str());
  REQUIRE(round_trip.ok());
  CHECK(round_trip.value() == port);
  CHECK_ERROR_CODE(AnyId::parse("port.").error(), ErrorCode::IdentityMalformed);
  const auto as_device = port.as<IdClass::Device>();
  CHECK_ERROR_CODE(as_device.error(), ErrorCode::IdentityClassMismatch);
}

IFABRIC_TEST(core, generation_and_epoch_bounds) {
  CHECK(GenerationNumber::genesis().is_genesis());
  const auto first = GenerationNumber::from(1).value();
  CHECK_EQ(first.next().value().value(), 2ull);
  CHECK_ERROR_CODE(GenerationNumber::from(GenerationNumber::kMax + 1).error(),
                   ErrorCode::InvalidArgument);
  CHECK_ERROR_CODE(GenerationNumber::from(GenerationNumber::kMax).value().next().error(),
                   ErrorCode::CheckedArithmeticOverflow);
  CHECK_EQ(GenerationNumber::from(7).value().padded(), std::string("0000000000000007"));
  CHECK_EQ(Epoch(0).str(), std::string("0"));
  CHECK_ERROR_CODE(Epoch::from(Epoch::kMax + 1).error(), ErrorCode::InvalidArgument);
  const auto incarnation = IncarnationId::generate();
  CHECK_EQ(incarnation.str().size(), std::size_t{32});
  CHECK_ERROR_CODE(IncarnationId::parse("zz").error(), ErrorCode::IdentityMalformed);
  CHECK_ERROR_CODE(IncarnationId::parse("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ").error(),
                   ErrorCode::IdentityMalformed);
}

IFABRIC_TEST(core, checked_arithmetic_reports_overflow) {
  const auto max = (std::numeric_limits<std::uint64_t>::max)();
  CHECK_EQ(checked_add(std::uint64_t{1}, std::uint64_t{2}).value(), 3ull);
  CHECK_ERROR_CODE(checked_add(max, std::uint64_t{1}).error(), ErrorCode::CheckedArithmeticOverflow);
  CHECK_EQ(checked_mul(std::uint64_t{1000}, std::uint64_t{1000}).value(), 1000000ull);
  CHECK_ERROR_CODE(checked_mul(max, std::uint64_t{2}).error(),
                   ErrorCode::CheckedArithmeticOverflow);
  CHECK_EQ(checked_size_add(std::size_t{4}, std::size_t{5}).value(), std::size_t{9});
  CHECK_EQ(checked_size_mul(std::size_t{4}, std::size_t{5}).value(), std::size_t{20});
  CHECK_ERROR_CODE(narrow_u32(0x1FFFFFFFFull).error(), ErrorCode::CheckedArithmeticOverflow);
  CHECK_ERROR_CODE(to_i64(0x8000000000000000ull).error(), ErrorCode::CheckedArithmeticOverflow);
  CHECK_EQ(saturating_add(max, std::size_t{1}), max);
}

IFABRIC_TEST(core, utf8_validation) {
  CHECK(is_valid_utf8("plain ascii"));
  CHECK(is_valid_utf8("caf\xC3\xA9"));
  CHECK(is_valid_utf8("\xE2\x82\xAC"));
  CHECK(is_valid_utf8("\xF0\x9F\x9A\x80"));
  CHECK(!is_valid_utf8("\xC3"));
  CHECK(!is_valid_utf8("\xC0\x80"));
  CHECK(!is_valid_utf8("\xE0\x80\x80"));
  CHECK(!is_valid_utf8("\xED\xA0\x80"));  // surrogate
  CHECK(!is_valid_utf8("\xF5\x80\x80\x80"));
}

IFABRIC_TEST(core, timestamps_round_trip) {
  const Timestamp epoch{};
  CHECK_EQ(format_timestamp(epoch), std::string("1970-01-01T00:00:00.000Z"));
  const Timestamp sample = epoch + std::chrono::milliseconds(1767225600123ll);
  const std::string text = format_timestamp(sample);
  CHECK_EQ(text, std::string("2026-01-01T00:00:00.123Z"));
  const auto parsed = parse_timestamp(text);
  REQUIRE(parsed.ok());
  CHECK(parsed.value() == sample);
  CHECK_ERROR_CODE(parse_timestamp("2026-13-01T00:00:00.000Z").error(),
                   ErrorCode::InvalidArgument);
  CHECK_ERROR_CODE(parse_timestamp("2026-01-01T00:00:00.000+01:00").error(),
                   ErrorCode::InvalidArgument);
  CHECK_ERROR_CODE(parse_timestamp("short").error(), ErrorCode::InvalidArgument);
}

IFABRIC_TEST(core, source_revision_is_bounded) {
  CHECK(SourceRevision::parse("git:abc123").ok());
  CHECK_ERROR_CODE(SourceRevision::parse("   ").error(), ErrorCode::InvalidArgument);
  CHECK_ERROR_CODE(SourceRevision::parse(std::string(300, 'x')).error(), ErrorCode::LimitExceeded);
  CHECK_EQ(SourceRevision::parse("  git:abc  ").value().str(), std::string("git:abc"));
}

IFABRIC_TEST(core, deterministic_rng_reproduces) {
  DeterministicRng left(1234);
  DeterministicRng right(1234);
  for (int i = 0; i < 64; ++i) {
    CHECK_EQ(left.next_u64(), right.next_u64());
  }
  DeterministicRng bounded(99);
  for (int i = 0; i < 128; ++i) {
    CHECK(bounded.next_bounded(7) < 7);
  }
}

IFABRIC_TEST(json, parses_and_canonicalizes) {
  const auto value = parse_json("{\"b\":1,\"a\":[true,null,\"x\"]}");
  REQUIRE(value.ok());
  CHECK_EQ(value.value().dump(), std::string("{\"a\":[true,null,\"x\"],\"b\":1}"));
  CHECK_EQ(value.value().dump_pretty(2).find('\n') != std::string::npos, true);
  const JsonValue* array = value.value().find("a");
  REQUIRE(array != nullptr);
  CHECK_EQ(array->size(), std::size_t{3});
}

IFABRIC_TEST(json, rejects_malformed_input) {
  CHECK_ERROR_CODE(parse_json("{\"a\":1,\"a\":2}").error(), ErrorCode::JsonDuplicateKey);
  CHECK_ERROR_CODE(parse_json("{\"a\":1} trailing").error(), ErrorCode::JsonTrailingContent);
  CHECK_ERROR_CODE(parse_json("").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("[1,]").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("{\"a\":}").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("01").error(), ErrorCode::JsonTrailingContent);
  CHECK_ERROR_CODE(parse_json("1.").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("1e").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("\"\\x\"").error(), ErrorCode::JsonParseError);
  CHECK_ERROR_CODE(parse_json("\"\\ud800\"").error(), ErrorCode::JsonEncodingError);
  CHECK_ERROR_CODE(parse_json("\"\\udc00\"").error(), ErrorCode::JsonEncodingError);
  CHECK_ERROR_CODE(parse_json("\"\\ud800\\u0041\"").error(), ErrorCode::JsonEncodingError);
  CHECK_ERROR_CODE(parse_json("9223372036854775808").error(), ErrorCode::JsonNumberOutOfRange);
  CHECK_ERROR_CODE(parse_json(std::string("\"") + std::string(9000, 'a') + "\"").error(),
                   ErrorCode::LimitExceeded);
  CHECK_ERROR_CODE(parse_json(std::string(70, '[') + std::string(70, ']')).error(),
                   ErrorCode::JsonDepthExceeded);
}

IFABRIC_TEST(json, decodes_escapes_and_surrogates) {
  const auto value = parse_json("\"\\u20ac\\ud83d\\ude00\\n\"");
  REQUIRE(value.ok());
  CHECK_EQ(*value.value().try_string(), std::string("\xE2\x82\xAC\xF0\x9F\x98\x80\n"));
  CHECK_EQ(parse_json("\"\\u0041\"").value().dump(), std::string("\"A\""));
  CHECK_EQ(parse_json("\"\\u0001\"").value().dump(), std::string("\"\\u0001\""));
}

IFABRIC_TEST(json, numbers_round_trip_with_kind_preserved) {
  CHECK_EQ(parse_json("1.0").value().dump(), std::string("1.0"));
  CHECK_EQ(parse_json("1").value().dump(), std::string("1"));
  CHECK_EQ(parse_json("-0.5").value().dump(), std::string("-0.5"));
  CHECK_EQ(parse_json("1e3").value().dump(), std::string("1000.0"));
  CHECK_EQ(parse_json("0.1").value().dump(), std::string("0.1"));
  CHECK(parse_json("1").value().is_int());
  CHECK(parse_json("1.0").value().is_double());
  CHECK_NE(JsonValue::integer(1), parse_json("1.0").value());
}

IFABRIC_TEST(json, mutation_keeps_members_unique_and_sorted) {
  JsonValue object;
  CHECK(object.set("b", JsonValue::integer(1)).ok());
  CHECK(object.set("a", JsonValue::integer(2)).ok());
  CHECK(object.set("b", JsonValue::integer(3)).ok());
  CHECK_EQ(object.dump(), std::string("{\"a\":2,\"b\":3}"));
  CHECK_ERROR_CODE(object.insert("a", JsonValue::integer(9)), ErrorCode::JsonDuplicateKey);
  CHECK(object.erase("a"));
  CHECK(!object.erase("a"));
  CHECK_EQ(object.dump(), std::string("{\"b\":3}"));
  CHECK_ERROR_CODE(JsonValue::number(std::numeric_limits<double>::infinity()).error(),
                   ErrorCode::JsonNumberOutOfRange);
}
