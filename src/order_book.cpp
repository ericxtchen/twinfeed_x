#include "../include/order_book.h"
#include "protocol.h"
#include <optional>

void OrderBook::apply(const Message &message) {
  if (message.side == Side::ASK &&
      message.operation == Operation::UPSERT_LEVEL) {
    this->asks_[message.price_ticks] = message.quantity;
  } else if (message.side == Side::ASK &&
             message.operation == Operation::DELETE_LEVEL) {
    this->asks_.erase(message.price_ticks);
  } else if (message.side == Side::BID &&
             message.operation == Operation::UPSERT_LEVEL) {
    this->bids_[message.price_ticks] = message.quantity;
  } else if (message.side == Side::BID &&
             message.operation == Operation::DELETE_LEVEL) {
    this->bids_.erase(message.price_ticks);
  }
}

std::optional<Level> OrderBook::best_bid() const {
  if (!this->bids_.empty()) {
    Level level{};
    auto best = this->bids_.rbegin();
    level.price_ticks = best->first;
    level.quantity = best->second;
    return level;
  }
  return std::nullopt;
}
std::optional<Level> OrderBook::best_ask() const {
  if (!this->asks_.empty()) {
    Level level{};
    auto best = this->asks_.begin();
    level.price_ticks = best->first;
    level.quantity = best->second;
    return level;
  }
  return std::nullopt;
}

std::optional<std::uint64_t>
OrderBook::quantity_at(Side side, std::int64_t price_ticks) const {
  if (side == Side::ASK) {
    if (this->asks_.contains(price_ticks)) {
      return this->asks_.at(price_ticks);
    }
    return std::nullopt;
  }

  if (side == Side::BID) {
    if (this->bids_.contains(price_ticks)) {
      return this->bids_.at(price_ticks);
    }
    return std::nullopt;
  }

  return std::nullopt;
}