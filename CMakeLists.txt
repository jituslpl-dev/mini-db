cmake_minimum_required(VERSION 3.16)
project(mini_db CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Warnings ----
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

# ---- sanitizer option ----
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ---- find threads ----
find_package(Threads REQUIRED)

# ---- core library ----
file(GLOB_RECURSE MINI_DB_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/common/*.cpp
  ${CMAKE_SOURCE_DIR}/src/storage/*.cpp
  ${CMAKE_SOURCE_DIR}/src/index/*.cpp
  ${CMAKE_SOURCE_DIR}/src/concurrency/*.cpp
  ${CMAKE_SOURCE_DIR}/src/recovery/*.cpp
  ${CMAKE_SOURCE_DIR}/src/catalog/*.cpp
  ${CMAKE_SOURCE_DIR}/src/parser/*.cpp
  ${CMAKE_SOURCE_DIR}/src/execution/*.cpp
)

add_library(minidb STATIC ${MINI_DB_SOURCES})
target_include_directories(minidb PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(minidb PUBLIC Threads::Threads)

# ---- CLI executable ----
add_executable(minidb_cli src/cli/cli.cpp)
target_link_libraries(minidb_cli PRIVATE minidb)

# ---- Tests ----
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/*.cpp)
foreach(TEST_SRC ${TEST_SOURCES})
  get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_SRC})
  target_link_libraries(${TEST_NAME} PRIVATE minidb)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# ---- Benchmarks ----
file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/benchmark/*.cpp)
foreach(BENCH_SRC ${BENCH_SOURCES})
  get_filename_component(BENCH_NAME ${BENCH_SRC} NAME_WE)
  add_executable(${BENCH_NAME} ${BENCH_SRC})
  target_link_libraries(${BENCH_NAME} PRIVATE minidb)
endforeach()