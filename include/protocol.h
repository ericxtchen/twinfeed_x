#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

constexpr std::size_t wire_size = 40;
constexpr std::size_t replay_request_wire_size = 26;

// The delivery path is local metadata. Both paths carry the same WireMessage.
enum class Feed { A, B };

struct ReplayRequest {
  std::uint64_t session_id;
  std::uint64_t first_sequence;
  std::uint64_t last_sequence; // Inclusive.
};

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

  bool operator==(const Message&) const = default;
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
  DELETE_WITH_NONZERO_QUANTITY,
  INVALID_REPLAY_RANGE
};

using WireReplayRequest = std::array<std::byte, replay_request_wire_size>;
using WireMessage = std::array<std::byte, wire_size>;
using EncodeResult = std::expected<WireMessage, ProtocolError>;
using DecodeResult = std::expected<Message, ProtocolError>;

std::expected<void, ProtocolError> validate(const Message &message);

EncodeResult encode(const Message &message);
DecodeResult decode(std::span<const std::byte> bytes);

// Control port, big-endian: u16 version, u64 session, u64 first, u64 last (inclusive).
std::expected<WireReplayRequest, ProtocolError> encode_replay_request(const ReplayRequest& request);
std::expected<ReplayRequest, ProtocolError> decode_replay_request(std::span<const std::byte> bytes);

#endif
