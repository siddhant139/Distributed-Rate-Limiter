cmake_minimum_required(VERSION 3.16)
project(rate_limiter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(rate_limiter
    src/main.cpp
    src/http_parser.cpp
    src/http_server.cpp
    src/thread_pool.cpp
    src/token_bucket.cpp
    src/sliding_window.cpp
    src/sliding_window_counter.cpp
    src/memory_backend.cpp
    src/redis_backend.cpp
    src/router.cpp
    src/stats.cpp
)
target_include_directories(rate_limiter PRIVATE src)
target_link_libraries(rate_limiter PRIVATE pthread)

enable_testing()
