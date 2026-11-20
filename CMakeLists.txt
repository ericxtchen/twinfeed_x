cmake_minimum_required(VERSION 3.25)

project(twinfeed_x LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion>
)

option(TWINFEED_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TWINFEED_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(project_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(project_options INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(FATAL_ERROR "This sanitizer configuration requires GCC or Clang.")
    endif()
endif()

# protocol.cpp has no main(): compile it once as a library.
# A future application executable can link to this same target.
add_library(twinfeed_protocol STATIC src/protocol.cpp)
target_include_directories(twinfeed_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(twinfeed_protocol PRIVATE project_options)

add_library(twinfeed_feed STATIC src/feed.cpp)
target_link_libraries(twinfeed_feed PUBLIC twinfeed_protocol PRIVATE project_options)

option(TWINFEED_USE_FLAT_MAP "Store book levels in std::flat_map" OFF)

add_library(twinfeed_book STATIC src/order_book.cpp)
target_link_libraries(twinfeed_book PUBLIC twinfeed_protocol PRIVATE project_options)

if(TWINFEED_USE_FLAT_MAP)
    target_compile_definitions(twinfeed_book PUBLIC TWINFEED_USE_FLAT_MAP=1)
endif()

add_executable(book_bench benchmarks/book_bench.cpp)
target_link_libraries(book_bench PRIVATE twinfeed_book project_options)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(twinfeed_udp STATIC src/udp_receiver.cpp src/receiver.cpp)
    target_link_libraries(twinfeed_udp PUBLIC twinfeed_book PRIVATE project_options)

    add_executable(twinfeed_publisher apps/publisher_main.cpp)
    target_link_libraries(twinfeed_publisher PRIVATE twinfeed_feed project_options)
    add_executable(twinfeed_receiver apps/receiver_main.cpp)
    target_link_libraries(twinfeed_receiver PRIVATE twinfeed_udp project_options)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
