#include "protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// These checks are intentionally NOT assert(): -DNDEBUG must not disable tests.
void check(
    bool condition, std::string_view expression,
    const std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::ostringstream out;
    out << location.file_name() << ':' << location.line()
        << ": check failed: " << expression;
    throw std::runtime_error(out.str());
  }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression)

template <typename Function>
void with_context(const std::string &context, Function function) {
  try {
    function();
  } catch (const std::exception &error) {
    throw std::runtime_error(context + ": " + error.what());
  }
}

std::string_view error_name(ProtocolError error) {
  switch (error) {
  case ProtocolError::INVALID_SIZE:
    return "INVALID_SIZE";
  case ProtocolError::UNSUPPORTED_VERSION:
    return "UNSUPPORTED_VERSION";
  case ProtocolError::INVALID_SEQ_NUM:
    return "INVALID_SEQ_NUM";
  case ProtocolError::INVALID_SIDE:
    return "INVALID_SIDE";
  case ProtocolError::INVALID_OPERATION:
    return "INVALID_OPERATION";
  case ProtocolError::INVALID_PRICE:
    return "INVALID_PRICE";
  case ProtocolError::UPSERT_WITH_ZERO_QUANTITY:
    return "UPSERT_WITH_ZERO_QUANTITY";
  case ProtocolError::DELETE_WITH_NONZERO_QUANTITY:
    return "DELETE_WITH_NONZERO_QUANTITY";
  }
  return "unrecognized ProtocolError";
}

template <typename Value>
void expect_error(
    const std::expected<Value, ProtocolError> &result, ProtocolError expected,
    const std::source_location location = std::source_location::current()) {
  if (result || result.error() != expected) {
    std::ostringstream out;
    out << location.file_name() << ':' << location.line() << ": expected "
        << error_name(expected) << ", got ";
    if (result) {
      out << "success";
    } else {
      out << error_name(result.error());
    }
    throw std::runtime_error(out.str());
  }
}

// Field comparisons avoid memcmp(), which would also compare struct padding.
void expect_message(const Message &actual, const Message &expected) {
  CHECK(actual.session_id == expected.session_id);
  CHECK(actual.seq_num == expected.seq_num);
  CHECK(actual.price_ticks == expected.price_ticks);
  CHECK(actual.quantity == expected.quantity);
  CHECK(actual.instrument_id == expected.instrument_id);
  CHECK(actual.side == expected.side);
  CHECK(actual.operation == expected.operation);
}

struct Fixture {
  std::string_view name;
  Message message;
  std::array<std::uint8_t, 40> bytes;
};

// These byte arrays are written independently from the protocol specification.
// Do not generate them using encode(), write_be(), or read_be(). The patterned
// fixture gives adjacent fields distinct values and exercises their upper
// bytes.
constexpr std::array<Fixture, 4> fixtures{
    {{"ordinary ask upsert",
      {1, 102, 10002, 25, 7, Side::ASK, Operation::UPSERT_LEVEL},
      {
          0x00, 0x01,                                     // version
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // session
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, // sequence
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x12, // price
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, // quantity
          0x00, 0x00, 0x00, 0x07,                         // instrument
          0x01, 0x00                                      // side, operation
      }},
     {"distinct full-width fields",
      {0x0102030405060708ULL, 0x1112131415161718ULL, 0x2122232425262728LL,
       0x3132333435363738ULL, 0x41424344U, Side::BID, Operation::UPSERT_LEVEL},
      {0x00, 0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22,
       0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x31, 0x32, 0x33, 0x34,
       0x35, 0x36, 0x37, 0x38, 0x41, 0x42, 0x43, 0x44, 0x00, 0x00}},
     {"ask delete",
      {0x0102030405060708ULL, 0x1112131415161718ULL, 0x2122232425262728LL, 0,
       0x41424344U, Side::ASK, Operation::DELETE_LEVEL},
      {0x00, 0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22,
       0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x01, 0x01}},
     {"maximum field values",
      {std::numeric_limits<std::uint64_t>::max(),
       std::numeric_limits<std::uint64_t>::max(),
       std::numeric_limits<std::int64_t>::max(),
       std::numeric_limits<std::uint64_t>::max(),
       std::numeric_limits<std::uint32_t>::max(), Side::ASK,
       Operation::UPSERT_LEVEL},
      {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF,
       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00}}}};

// This helper only converts uint8_t literals to std::byte. It does not encode
// fields, and does not call production serialization helpers.
WireMessage fixture_bytes(const Fixture &fixture = fixtures[0]) {
  WireMessage result{};
  CHECK(result.size() == fixture.bytes.size());
  for (std::size_t i = 0; i < fixture.bytes.size(); ++i) {
    result[i] = static_cast<std::byte>(fixture.bytes[i]);
  }
  return result;
}

void expect_bytes(const WireMessage &actual, const Fixture &fixture) {
  CHECK(actual.size() == fixture.bytes.size());
  for (std::size_t i = 0; i < fixture.bytes.size(); ++i) {
    const auto byte = std::to_integer<unsigned int>(actual[i]);
    if (byte != fixture.bytes[i]) {
      std::ostringstream out;
      out << fixture.name << ": byte " << i << ": expected 0x" << std::hex
          << static_cast<unsigned int>(fixture.bytes[i]) << ", got 0x" << byte;
      throw std::runtime_error(out.str());
    }
  }
}

void expect_round_trip(const Message &original) {
  CHECK(validate(original).has_value());
  const auto encoded = encode(original);
  CHECK(encoded.has_value());
  CHECK(encoded->size() == 40);
  const auto decoded = decode(*encoded);
  CHECK(decoded.has_value());
  expect_message(*decoded, original);
}

// Every semantic rejection is tested at all three public entry points.
// The malformed decoder input is constructed from a known-good literal fixture,
// not encode(invalid_message), which should itself refuse invalid messages.
void expect_shared_error(const Message &message, const WireMessage &bytes,
                         ProtocolError expected) {
  with_context("validate", [&] { expect_error(validate(message), expected); });
  with_context("encode", [&] { expect_error(encode(message), expected); });
  with_context("decode", [&] { expect_error(decode(bytes), expected); });
}

void encode_golden_frames() {
  for (const auto &fixture : fixtures) {
    with_context(std::string(fixture.name), [&] {
      const auto result = encode(fixture.message);
      CHECK(result.has_value());
      expect_bytes(*result, fixture);
    });
  }
}

void decode_golden_frames() {
  for (const auto &fixture : fixtures) {
    with_context(std::string(fixture.name), [&] {
      const auto result = decode(fixture_bytes(fixture));
      CHECK(result.has_value());
      expect_message(*result, fixture.message);
    });
  }
}

void validate_valid_messages() {
  for (const auto &fixture : fixtures) {
    with_context(std::string(fixture.name),
                 [&] { CHECK(validate(fixture.message).has_value()); });
  }
}

void round_trip_all_sides_and_operations() {
  for (const auto side : {Side::BID, Side::ASK}) {
    for (const auto operation :
         {Operation::UPSERT_LEVEL, Operation::DELETE_LEVEL}) {
      auto message = fixtures[0].message;
      message.side = side;
      message.operation = operation;
      message.quantity = operation == Operation::DELETE_LEVEL ? 0 : 25;
      expect_round_trip(message);
    }
  }
}

void uint64_boundaries() {
  constexpr std::array<std::uint64_t, 11> values{
      0,
      1,
      0xFF,
      0x100,
      0xFFFF,
      0x10000,
      0xFFFFFFFFULL,
      0x100000000ULL,
      0x7FFFFFFFFFFFFFFFULL,
      0x8000000000000000ULL,
      std::numeric_limits<std::uint64_t>::max()};
  for (const auto value : values) {
    with_context("uint64 value " + std::to_string(value), [&] {
      auto message = fixtures[0].message;
      message.session_id = value;
      with_context("session", [&] { expect_round_trip(message); });
      if (value != 0) {
        message = fixtures[0].message;
        message.seq_num = value;
        with_context("sequence", [&] { expect_round_trip(message); });
        message = fixtures[0].message;
        message.quantity = value;
        with_context("quantity", [&] { expect_round_trip(message); });
      }
    });
  }
}

void instrument_boundaries() {
  constexpr std::array<std::uint32_t, 9> values{
      0,           1,           0xFF,
      0x100,       0xFFFF,      0x10000,
      0x7FFFFFFFU, 0x80000000U, std::numeric_limits<std::uint32_t>::max()};
  for (const auto value : values) {
    with_context("instrument " + std::to_string(value), [&] {
      auto message = fixtures[0].message;
      message.instrument_id = value;
      expect_round_trip(message);
    });
  }
}

void price_boundaries() {
  constexpr std::array<std::int64_t, 10> values{
      1,
      0xFF,
      0x100,
      0xFFFF,
      0x10000,
      0x7FFFFFFFLL,
      0x80000000LL,
      0xFFFFFFFFLL,
      0x100000000LL,
      std::numeric_limits<std::int64_t>::max()};
  for (const auto value : values) {
    with_context("price " + std::to_string(value), [&] {
      for (const auto operation :
           {Operation::UPSERT_LEVEL, Operation::DELETE_LEVEL}) {
        auto message = fixtures[0].message;
        message.price_ticks = value;
        message.operation = operation;
        message.quantity = operation == Operation::DELETE_LEVEL ? 0 : 25;
        expect_round_trip(message);
      }
    });
  }
}

void decode_empty() {
  expect_error(decode(std::span<const std::byte>{}),
               ProtocolError::INVALID_SIZE);
}

void decode_all_truncated_lengths() {
  const auto good = fixture_bytes();
  for (std::size_t length = 1; length < 40; ++length) {
    with_context("length " + std::to_string(length), [&] {
      // Allocate the short input separately, rather than handing a short
      // span over an intact 40-byte allocation. This helps ASan detect OOB
      // reads.
      const std::vector<std::byte> short_input(
          good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
      expect_error(decode(short_input), ProtocolError::INVALID_SIZE);
    });
  }
}

void decode_oversized() {
  const auto good = fixture_bytes();
  constexpr std::array<std::size_t, 7> lengths{41, 42, 48, 64, 80, 4096, 65507};
  for (const auto length : lengths) {
    with_context("length " + std::to_string(length), [&] {
      std::vector<std::byte> input(length, std::byte{0});
      std::copy(good.begin(), good.end(), input.begin());
      expect_error(decode(input), ProtocolError::INVALID_SIZE);
    });
  }
}

void decode_concatenated_frames() {
  // Version 1 permits one event per datagram, not two valid frames
  // concatenated.
  const auto first = fixture_bytes(fixtures[0]);
  const auto second = fixture_bytes(fixtures[2]);
  std::array<std::byte, 80> input{};
  std::copy(first.begin(), first.end(), input.begin());
  std::copy(second.begin(), second.end(), input.begin() + 40);
  expect_error(decode(input), ProtocolError::INVALID_SIZE);
}

void decode_unsupported_versions() {
  // The complete 16-bit version space is small enough to check exhaustively.
  // In particular, 0x0100 must not be mistaken for big-endian version 0x0001.
  for (std::uint32_t version = 0; version <= 0xFFFFU; ++version) {
    if (version == 1) {
      continue;
    }
    with_context("version " + std::to_string(version), [&] {
      auto bytes = fixture_bytes();
      bytes[0] = static_cast<std::byte>((version >> 8) & 0xFFU);
      bytes[1] = static_cast<std::byte>(version & 0xFFU);
      expect_error(decode(bytes), ProtocolError::UNSUPPORTED_VERSION);
    });
  }
}

void reject_zero_sequence() {
  auto message = fixtures[0].message;
  message.seq_num = 0;
  auto bytes = fixture_bytes();
  std::fill(bytes.begin() + 10, bytes.begin() + 18, std::byte{0});
  expect_shared_error(message, bytes, ProtocolError::INVALID_SEQ_NUM);
}

void reject_invalid_sides() {
  for (unsigned int raw = 2; raw <= 255; ++raw) {
    with_context("side " + std::to_string(raw), [&] {
      auto message = fixtures[0].message;
      message.side = static_cast<Side>(raw);
      auto bytes = fixture_bytes();
      bytes[38] = static_cast<std::byte>(raw);
      expect_shared_error(message, bytes, ProtocolError::INVALID_SIDE);
    });
  }
}

void reject_invalid_operations() {
  for (unsigned int raw = 2; raw <= 255; ++raw) {
    with_context("operation " + std::to_string(raw), [&] {
      auto message = fixtures[0].message;
      message.operation = static_cast<Operation>(raw);
      auto bytes = fixture_bytes();
      bytes[39] = static_cast<std::byte>(raw);
      expect_shared_error(message, bytes, ProtocolError::INVALID_OPERATION);
    });
  }
}

void expect_invalid_price(std::int64_t price,
                          const std::array<std::uint8_t, 8> &raw) {
  for (const auto operation :
       {Operation::UPSERT_LEVEL, Operation::DELETE_LEVEL}) {
    auto message = fixtures[0].message;
    auto bytes = fixture_bytes();
    message.price_ticks = price;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      bytes[18 + i] = static_cast<std::byte>(raw[i]);
    }
    message.operation = operation;
    bytes[39] = static_cast<std::byte>(operation);
    if (operation == Operation::DELETE_LEVEL) {
      message.quantity = 0;
      std::fill(bytes.begin() + 26, bytes.begin() + 34, std::byte{0});
    }
    expect_shared_error(message, bytes, ProtocolError::INVALID_PRICE);
  }
}

void reject_zero_price() { expect_invalid_price(0, {0, 0, 0, 0, 0, 0, 0, 0}); }

void reject_negative_one_price() {
  expect_invalid_price(-1, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
}

void reject_minimum_signed_price() {
  expect_invalid_price(std::numeric_limits<std::int64_t>::min(),
                       {0x80, 0, 0, 0, 0, 0, 0, 0});
}

void reject_zero_upsert_quantity() {
  auto message = fixtures[0].message;
  message.quantity = 0;
  auto bytes = fixture_bytes();
  std::fill(bytes.begin() + 26, bytes.begin() + 34, std::byte{0});
  expect_shared_error(message, bytes, ProtocolError::UPSERT_WITH_ZERO_QUANTITY);
}

void reject_nonzero_delete_quantity() {
  for (const auto quantity : std::array<std::uint64_t, 2>{
           1, std::numeric_limits<std::uint64_t>::max()}) {
    auto message = fixtures[0].message;
    message.operation = Operation::DELETE_LEVEL;
    message.quantity = quantity;
    auto bytes = fixture_bytes();
    bytes[39] = std::byte{1};
    if (quantity == 1) {
      std::fill(bytes.begin() + 26, bytes.begin() + 34, std::byte{0});
      bytes[33] = std::byte{1};
    } else {
      std::fill(bytes.begin() + 26, bytes.begin() + 34, std::byte{0xFF});
    }
    expect_shared_error(message, bytes,
                        ProtocolError::DELETE_WITH_NONZERO_QUANTITY);
  }
}

void allow_zero_session_and_instrument() {
  // No codec rule currently reserves session/instrument zero. Active-session
  // and configured-instrument checks belong to later components.
  auto message = fixtures[0].message;
  message.session_id = 0;
  message.instrument_id = 0;
  auto bytes = fixture_bytes();
  std::fill(bytes.begin() + 2, bytes.begin() + 10, std::byte{0});
  std::fill(bytes.begin() + 34, bytes.begin() + 38, std::byte{0});
  CHECK(validate(message).has_value());
  const auto encoded = encode(message);
  CHECK(encoded.has_value());
  CHECK(*encoded == bytes);
  const auto decoded = decode(bytes);
  CHECK(decoded.has_value());
  expect_message(*decoded, message);
}

void decode_unaligned_buffer() {
  const auto good = fixture_bytes(fixtures[1]);
  alignas(std::uint64_t) std::array<std::byte, 48> storage{};
  std::copy(good.begin(), good.end(), storage.begin() + 1);
  const auto input = std::span<const std::byte>(storage).subspan(1, 40);
  const auto decoded = decode(input);
  CHECK(decoded.has_value());
  expect_message(*decoded, fixtures[1].message);
}

void deterministic_round_trips() {
  // A fixed engine and explicit integer transformations make failures
  // reproducible. This supplements the independent fixtures; it cannot replace
  // them.
  constexpr std::uint64_t seed = 0x5457494E46454544ULL;
  constexpr std::size_t iterations = 10000;
  std::mt19937_64 random(seed);

  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    Message message{};
    message.session_id = random();
    message.seq_num = random();
    if (message.seq_num == 0) {
      message.seq_num = 1;
    }
    message.price_ticks = static_cast<std::int64_t>(
        random() &
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    if (message.price_ticks == 0) {
      message.price_ticks = 1;
    }
    message.quantity = random();
    message.instrument_id = static_cast<std::uint32_t>(random());
    message.side = static_cast<Side>(random() & 1U);
    message.operation = static_cast<Operation>(random() & 1U);
    if (message.operation == Operation::DELETE_LEVEL) {
      message.quantity = 0;
    } else if (message.quantity == 0) {
      message.quantity = 1;
    }

    with_context("seed " + std::to_string(seed) + ", iteration " +
                     std::to_string(iteration),
                 [&] { expect_round_trip(message); });
  }
}

struct TestCase {
  std::string_view name;
  void (*run)();
};

constexpr TestCase cases[]{
    {"encode_golden_frames", encode_golden_frames},
    {"decode_golden_frames", decode_golden_frames},
    {"validate_valid_messages", validate_valid_messages},
    {"round_trip_all_sides_and_operations",
     round_trip_all_sides_and_operations},
    {"uint64_boundaries", uint64_boundaries},
    {"instrument_boundaries", instrument_boundaries},
    {"price_boundaries", price_boundaries},
    {"decode_empty", decode_empty},
    {"decode_all_truncated_lengths", decode_all_truncated_lengths},
    {"decode_oversized", decode_oversized},
    {"decode_concatenated_frames", decode_concatenated_frames},
    {"decode_unsupported_versions", decode_unsupported_versions},
    {"reject_zero_sequence", reject_zero_sequence},
    {"reject_invalid_sides", reject_invalid_sides},
    {"reject_invalid_operations", reject_invalid_operations},
    {"reject_zero_price", reject_zero_price},
    {"reject_negative_one_price", reject_negative_one_price},
    {"reject_minimum_signed_price", reject_minimum_signed_price},
    {"reject_zero_upsert_quantity", reject_zero_upsert_quantity},
    {"reject_nonzero_delete_quantity", reject_nonzero_delete_quantity},
    {"allow_zero_session_and_instrument", allow_zero_session_and_instrument},
    {"decode_unaligned_buffer", decode_unaligned_buffer},
    {"deterministic_round_trips", deterministic_round_trips}};

} // namespace

int main(int argc, char **argv) {
  if (argc > 2) {
    std::cerr << "Usage: protocol_tests [case-name]\n";
    return 2;
  }

  const std::string_view selected = argc == 2 ? argv[1] : "";
  std::size_t executed = 0;
  std::size_t failed = 0;
  for (const auto &test : cases) {
    if (argc == 2 && test.name != selected) {
      continue;
    }
    ++executed;
    try {
      test.run();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": unexpected exception\n";
    }
  }

  if (executed == 0) {
    std::cerr << "Unknown test case: " << selected << '\n';
    return 2;
  }
  std::cout << executed - failed << '/' << executed << " tests passed\n";
  return failed == 0 ? 0 : 1;
}
