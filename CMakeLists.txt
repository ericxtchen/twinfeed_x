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

# One instrument's price-level book. Reuse the existing protocol types.
add_library(twinfeed_book STATIC src/order_book.cpp)
target_link_libraries(twinfeed_book PUBLIC twinfeed_protocol PRIVATE project_options)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
