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

add_executable(receiver_ab_test receiver_ab_test.cpp ../src/receiver.cpp)
target_link_libraries(receiver_ab_test PRIVATE twinfeed_book project_options)
add_test(NAME receiver.ab_four_arrivals COMMAND receiver_ab_test)
add_test(NAME receiver.slot_collision COMMAND receiver_ab_test slot_collision)
add_test(NAME receiver.conflicting_copy COMMAND receiver_ab_test conflicting_copy)

add_executable(receiver_fault_test receiver_fault_test.cpp ../src/receiver.cpp)
target_link_libraries(receiver_fault_test PRIVATE twinfeed_book twinfeed_feed project_options)
foreach(test_case IN ITEMS full_buffer_gap_closure full_buffer_overflow known_gap
                           committed_conflict drained_conflict recent_conflict restart seeded_replay retransmission_deadline)
    add_test(NAME "receiver.${test_case}" COMMAND receiver_fault_test "${test_case}")
endforeach()

if(UNIX)
    add_executable(receiver_udp_loopback_test receiver_udp_loopback_test.cpp ../src/receiver.cpp)
    target_link_libraries(receiver_udp_loopback_test PRIVATE twinfeed_book twinfeed_feed project_options)
    add_test(NAME receiver.udp_loopback COMMAND receiver_udp_loopback_test)
    set_tests_properties(receiver.udp_loopback PROPERTIES TIMEOUT 10)
endif()

add_executable(feed_test feed_test.cpp)
target_link_libraries(feed_test PRIVATE twinfeed_feed project_options)
add_test(NAME publisher.replay_window COMMAND feed_test)
add_test(NAME publisher.replay_after_live_loss COMMAND receiver_fault_test publisher_replay)

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

add_executable(receiver_replay_test receiver_replay_test.cpp ../src/receiver.cpp)
target_link_libraries(receiver_replay_test PRIVATE twinfeed_book project_options)
add_test(NAME receiver.replay COMMAND receiver_replay_test)
