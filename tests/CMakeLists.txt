add_executable(protocol_tests unit/protocol_test.cpp)
target_link_libraries(protocol_tests PRIVATE twinfeed_protocol project_options)

# Each named case can be run individually through CTest. Running the executable
# without arguments runs the entire suite.
set(protocol_test_cases
    encode_golden_frames
    decode_golden_frames
    validate_valid_messages
    round_trip_all_sides_and_operations
    uint64_boundaries
    instrument_boundaries
    price_boundaries
    decode_empty
    decode_all_truncated_lengths
    decode_oversized
    decode_concatenated_frames
    decode_unsupported_versions
    reject_zero_sequence
    reject_invalid_sides
    reject_invalid_operations
    reject_zero_price
    reject_negative_one_price
    reject_minimum_signed_price
    reject_zero_upsert_quantity
    reject_nonzero_delete_quantity
    allow_zero_session_and_instrument
    decode_unaligned_buffer
    deterministic_round_trips
)

foreach(test_case IN LISTS protocol_test_cases)
    add_test(NAME "protocol.${test_case}" COMMAND protocol_tests "${test_case}")
    set_tests_properties("protocol.${test_case}" PROPERTIES LABELS "unit;protocol")
endforeach()

add_executable(order_book_tests unit/order_book_test.cpp)
target_link_libraries(order_book_tests PRIVATE twinfeed_book project_options)

set(book_test_cases
    empty_book
    bid_only_lookup_regression
    ask_only_bid_query_regression
    ask_lookup_and_absent_price
    same_price_sides_are_independent
    highest_bid_wins
    lowest_ask_wins
    upsert_replaces_quantity
    delete_best_reveals_next
    delete_nonbest_preserves_best
    delete_last_level_empties_side
    missing_deletes_are_noops
    repeated_absolute_updates
    delete_then_reinsert
    queries_do_not_create_levels
    sparse_and_large_prices
    quantity_boundaries
    separate_books_are_independent
    documented_bid_trace
    deterministic_model_replay
)

foreach(test_case IN LISTS book_test_cases)
    add_test(NAME "book.${test_case}" COMMAND order_book_tests "${test_case}")
    set_tests_properties("book.${test_case}" PROPERTIES LABELS "unit;book")
endforeach()
