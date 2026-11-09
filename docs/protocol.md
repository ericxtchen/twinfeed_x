# Struct Layout
This is the struct that represents the data we get from a feed.
```cpp
struct Message {
    std::uint64_t session_id;
    std::uint64_t seq_num;
    std::int64_t  price_ticks; // price in integer units
    std::uint64_t quantity;
    std::uint32_t instrument_id;
    Side          side; // BID | ASK
    Operation     operation; // UPSERT_LEVEL | DELETE_LEVEL
};
```
```cpp
enum class Side : std::uint8_t {
    BID = 0,
    ASK = 1
};

enum class Operation : std::uint8_t {
    UPSERT_LEVEL = 0,
    DELETE_LEVEL = 1
};
```

This struct layout gives us 38 bytes total of actual data plus 2 bytes of version totalling 40 bytes.

Version numbers are unsigned 16-bit integers. This specification is version 1. Unsupported versions are rejected.

Payload size is exactly 40 bytes for version 1, containing one event. Reject other sizes and transport-reported truncation.

`seq_num` starts at 1 within a session and is shared across feeds A and B; sequence 0 is not a valid incremental event.

Price units and valid instrument IDs come from shared configuration. For our initial stock model, require positive prices, positive quantity for UPSERT_LEVEL, and zero quantity for DELETE_LEVEL.

All multibyte fields are big-endian.

# Wire Protocol
We therefore get this table:
| Offset | Field | Bytes |
|---:|---|---:|
| 0 | Version | 2 |
| 2 | Session ID | 8 |
| 10 | Sequence number | 8 |
| 18 | Price ticks | 8 |
| 26 | Quantity | 8 |
| 34 | Instrument ID | 4 |
| 38 | Side | 1 |
| 39 | Operation | 1 |