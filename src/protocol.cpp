#include "../include/protocol.h"
#include <bit>
#include <expected>

namespace {
void write_be(WireMessage &buf, std::size_t offset, std::uint64_t value,
              std::size_t byte_count) {
  for (std::size_t i = 0; i < byte_count; ++i) {
    const auto shift = 8 * (byte_count - 1 - i);

    buf[offset + i] = static_cast<std::byte>((value >> shift) & 0xFF);
  }
}

// Preconditions:
// - byte_count is between 1 and 8.
// - the requested range exists in buf.
std::uint64_t read_be(std::span<const std::byte> buf, std::size_t offset,
                      std::size_t byte_count) {
  std::uint64_t value = 0;

  for (std::size_t i = 0; i < byte_count; ++i) {
    value <<= 8;
    value |= std::to_integer<std::uint64_t>(buf[offset + i]);
  }

  return value;
}
} // namespace

std::expected<void, ProtocolError> validate(const Message &message) {
  if (message.seq_num == 0) {
    return std::unexpected(ProtocolError::INVALID_SEQ_NUM);
  } else if (message.operation == Operation::UPSERT_LEVEL &&
             message.quantity == 0) {
    return std::unexpected(ProtocolError::UPSERT_WITH_ZERO_QUANTITY);
  } else if (message.operation == Operation::DELETE_LEVEL &&
             message.quantity != 0) {
    return std::unexpected(ProtocolError::DELETE_WITH_NONZERO_QUANTITY);
  } else if (message.side != Side::BID && message.side != Side::ASK) {
    return std::unexpected(ProtocolError::INVALID_SIDE);
  } else if (message.operation != Operation::UPSERT_LEVEL &&
             message.operation != Operation::DELETE_LEVEL) {
    return std::unexpected(ProtocolError::INVALID_OPERATION);
  } else if (message.price_ticks <= 0) {
    return std::unexpected(ProtocolError::INVALID_PRICE);
  }

  return {};
}

EncodeResult encode(const Message &message) {
  auto check = validate(message);
  if (!check) {
    return std::unexpected(check.error());
  }
  WireMessage buf{};

  write_be(buf, 0, 1, 2); // version number
  write_be(buf, 2, message.session_id, 8);
  write_be(buf, 10, message.seq_num, 8);
  write_be(buf, 18, static_cast<std::uint64_t>(message.price_ticks), 8);
  write_be(buf, 26, message.quantity, 8);
  write_be(buf, 34, message.instrument_id, 4);

  buf[38] = static_cast<std::byte>(message.side);
  buf[39] = static_cast<std::byte>(message.operation);

  return buf;
}

DecodeResult decode(std::span<const std::byte> buf) {
  auto bytes_len = buf.size();
  if (bytes_len != wire_size) {
    return std::unexpected(ProtocolError::INVALID_SIZE);
  }
  const auto version = read_be(buf, 0, 2);
  if (version != 1) {
    return std::unexpected(ProtocolError::UNSUPPORTED_VERSION);
  }

  Message message{
      .session_id = read_be(buf, 2, 8),
      .seq_num = read_be(buf, 10, 8),
      .price_ticks = std::bit_cast<std::int64_t>(read_be(buf, 18, 8)),
      .quantity = read_be(buf, 26, 8),
      .instrument_id = static_cast<std::uint32_t>(read_be(buf, 34, 4)),
      .side = static_cast<Side>(std::to_integer<std::uint8_t>(buf[38])),
      .operation =
          static_cast<Operation>(std::to_integer<std::uint8_t>(buf[39]))};

  auto check = validate(message);

  if (!check) {
    return std::unexpected(check.error());
  }

  return message;
}