# nullclaw-c tests
cmake_minimum_required(VERSION 3.20)

# Basic smoke test: build and run --version
add_test(NAME version
  COMMAND nullclaw version)

add_test(NAME status_nocrash
  COMMAND nullclaw status)
