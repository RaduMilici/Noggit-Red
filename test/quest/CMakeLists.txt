# Unit tests for the quest and item editors' SQL (src/noggit/content, quest, item). Pure: no database.
#
#   cmake -S test/quest -B build-quest-tests && cmake --build build-quest-tests && ctest --test-dir build-quest-tests
cmake_minimum_required(VERSION 3.16)
project(noggit_quest_tests LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 COMPONENTS Core Test REQUIRED)

set(NOGGIT_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
file(GLOB SOURCES CONFIGURE_DEPENDS
  ${NOGGIT_SRC}/noggit/content/*.cpp
  ${NOGGIT_SRC}/noggit/quest/*.cpp
  ${NOGGIT_SRC}/noggit/item/*.cpp)
file(GLOB TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

add_executable(quest_tests ${TESTS} ${SOURCES})
target_include_directories(quest_tests PRIVATE ${NOGGIT_SRC})
target_link_libraries(quest_tests PRIVATE Qt5::Core Qt5::Test)
add_test(NAME quest_tests COMMAND quest_tests)
