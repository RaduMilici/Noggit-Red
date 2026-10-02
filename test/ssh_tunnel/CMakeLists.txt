# Unit tests for the MySQL SSH tunnel (src/noggit/ssh). Needs only Qt5 Core/Network/Test, so it can be
# built on its own without the full Noggit dependency set:
#
#   cmake -S test/ssh_tunnel -B build-ssh-tests && cmake --build build-ssh-tests && ctest --test-dir build-ssh-tests
#
# or from the main project with -DNOGGIT_BUILD_TESTS=ON.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_NAME)
  project(noggit_ssh_tunnel_tests LANGUAGES CXX)
  enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt5 COMPONENTS Core Network Test REQUIRED)

set(NOGGIT_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")

add_executable(ssh_tunnel_tests
  test_ssh_tunnel.cpp
  ${NOGGIT_SRC}/noggit/ssh/SshTunnelConfig.cpp
  ${NOGGIT_SRC}/noggit/ssh/SshRemote.cpp
  ${NOGGIT_SRC}/noggit/ssh/SshTunnelManager.cpp
  ${NOGGIT_SRC}/noggit/ssh/SshTunnelManager.hpp
  ${NOGGIT_SRC}/noggit/Log.cpp
)
set_target_properties(ssh_tunnel_tests PROPERTIES AUTOMOC ON)
target_include_directories(ssh_tunnel_tests PRIVATE ${NOGGIT_SRC})
target_link_libraries(ssh_tunnel_tests PRIVATE Qt5::Core Qt5::Network Qt5::Test)

add_test(NAME ssh_tunnel_tests COMMAND ssh_tunnel_tests WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
