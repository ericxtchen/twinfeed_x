#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H
#include "protocol.h"
#include <cstdint>
#ifdef TWINFEED_USE_FLAT_MAP
#include <flat_map>
#else
#include <map>
#endif
#include <optional>

struct Level {
  std::int64_t price_ticks;
  std::uint64_t quantity;
};

class OrderBook {
public:
  // Preconditions:
  // - The message has passed protocol validation.
  // - It belongs to this book's instrument.
  // - The caller supplies messages in the correct application order.
  void apply(const Message &message);

  [[nodiscard]]
  std::optional<Level> best_bid() const;

  [[nodiscard]]
  std::optional<Level> best_ask() const;

  [[nodiscard]]
  std::optional<std::uint64_t> quantity_at(Side side,
                                           std::int64_t price_ticks) const;

private:
#ifdef TWINFEED_USE_FLAT_MAP
      using Levels = std::flat_map<std::int64_t, std::uint64_t>;
#else
      using Levels = std::map<std::int64_t, std::uint64_t>;
#endif
  Levels bids_;
  Levels asks_;
#ifdef TWINFEED_USE_FLAT_MAP
    static_assert(std::is_same_v<
        decltype(bids_), std::flat_map<std::int64_t, std::uint64_t>>);
#else
    static_assert(std::is_same_v<
        decltype(bids_), std::map<std::int64_t, std::uint64_t>>);
#endif
};

#endif
