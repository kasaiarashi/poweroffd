cmake_minimum_required(VERSION 3.15)
project(poweroffd-win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Daemon (Windows Service)
add_executable(poweroffd poweroffd-win.cpp)
target_link_libraries(poweroffd PRIVATE ws2_32 bcrypt advapi32)
target_compile_definitions(poweroffd PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)

# Client
add_executable(poweroff-send poweroff-send-win.cpp)
target_link_libraries(poweroff-send PRIVATE ws2_32 bcrypt)
target_compile_definitions(poweroff-send PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)

# Install targets
install(TARGETS poweroffd poweroff-send RUNTIME DESTINATION bin)
