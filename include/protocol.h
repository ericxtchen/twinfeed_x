#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

constexpr std::size_t wire_size = 40;

enum class Side : std::uint8_t { BID = 0, ASK = 1 };

enum class Operation : std::uint8_t { UPSERT_LEVEL = 0, DELETE_LEVEL = 1 };

struct Message {
  std::uint64_t session_id;
  std::uint64_t seq_num;
  std::int64_t price_ticks; // price in integer units
  std::uint64_t quantity;
  std::uint32_t instrument_id;
  Side side;           // BID | ASK
  Operation operation; // UPSERT_LEVEL | DELETE_LEVEL
};

enum class ProtocolError {
  // Decoding-specific errors
  INVALID_SIZE,
  UNSUPPORTED_VERSION,

  // Shared message-validation errors
  INVALID_SEQ_NUM,
  INVALID_SIDE,
  INVALID_OPERATION,
  INVALID_PRICE,
  UPSERT_WITH_ZERO_QUANTITY,
  DELETE_WITH_NONZERO_QUANTITY
};

using WireMessage = std::array<std::byte, wire_size>;
using EncodeResult = std::expected<WireMessage, ProtocolError>;
using DecodeResult = std::expected<Message, ProtocolError>;

std::expected<void, ProtocolError> validate(const Message &message);

EncodeResult encode(const Message &message);
DecodeResult decode(std::span<const std::byte> bytes);

#endif