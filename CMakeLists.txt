cmake_minimum_required(VERSION 3.20)
project(tiniclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Build types ──────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -Wall -Wextra -Wpedantic -Wno-unused-parameter")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# ── Homebrew prefix detection (Apple Silicon / Intel) ────────────
if(APPLE)
  if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(BREW_PREFIX "/opt/homebrew")
  else()
    set(BREW_PREFIX "/usr/local")
  endif()
endif()

# ── Find packages ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

# cJSON
find_path(CJSON_INCLUDE_DIR cjson/cJSON.h HINTS ${BREW_PREFIX}/include)
find_library(CJSON_LIB cjson HINTS ${BREW_PREFIX}/lib)

# SQLite (FTS5-enabled from brew)
set(SQLITE_INCLUDE_DIR "${BREW_PREFIX}/opt/sqlite/include")
set(SQLITE_LIB         "${BREW_PREFIX}/opt/sqlite/lib/libsqlite3.dylib")

# OpenSSL 3 (ChaCha20-Poly1305)
set(OPENSSL_ROOT_DIR   "${BREW_PREFIX}/opt/openssl@3")
find_package(OpenSSL REQUIRED)

# ── Include dirs ─────────────────────────────────────────────────
include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/vendor
  ${CJSON_INCLUDE_DIR}
  ${SQLITE_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
  ${CURL_INCLUDE_DIRS}
  ${PCRE2_INCLUDE_DIRS}
)

# ── Sources ──────────────────────────────────────────────────────
set(SOURCES
  # Core infrastructure
  src/arena.c
  src/util.c
  src/platform.c
  src/observability.c
  src/health.c
  src/version.c
  src/bus.c

  # Config
  src/config.c

  # Providers
  src/providers/provider.c
  src/providers/openai.c
  src/providers/anthropic.c
  src/providers/ollama.c
  src/providers/compatible.c
  src/providers/gemini.c
  src/providers/openrouter.c
  src/providers/factory.c
  src/providers/sse.c
  src/providers/reliable.c
  src/providers/router.c
  src/providers/scrub.c

  # Channels (combined implementations + per-file stubs for CMake listing)
  src/channels/channel.c
  src/channels/channel_stubs.c
  src/channels/telegram.c

  # Memory
  src/memory/memory.c
  src/memory/sqlite_mem.c
  src/memory/embeddings.c
  src/memory/hygiene.c
  src/memory/snapshot.c
  src/memory/cache.c

  # Security
  src/security/secrets.c
  src/security/pairing.c

  # Tools (consolidated into combined files)
  src/tools/tool.c
  src/tools/shell.c
  src/tools/file_tools.c
  src/tools/http_request.c
  src/tools/git.c
  src/tools/memory_tools.c
  src/tools/misc_tools.c

  # Agent
  src/agent/agent.c
  src/agent/prompt.c
  src/agent/dispatcher.c
  src/agent/compaction.c
  src/agent/memory_loader.c

  # Top-level subsystems
  src/gateway.c
  src/cron.c
  src/mcp.c
  src/net_security.c
  src/onboard.c
  src/status.c
  src/doctor.c
  src/version.c

  # Entry point
  src/main.c
)

add_executable(tiniclaw ${SOURCES})

target_link_libraries(tiniclaw
  ${CJSON_LIB}
  ${SQLITE_LIB}
  OpenSSL::SSL
  OpenSSL::Crypto
  ${CURL_LIBRARIES}
  ${PCRE2_LIBRARIES}
  pthread
)

if(APPLE)
  target_link_libraries(tiniclaw "-framework CoreFoundation" "-framework Security")
endif()

# ── Install ──────────────────────────────────────────────────────
install(TARGETS tiniclaw DESTINATION bin)

# ── Tests ────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
