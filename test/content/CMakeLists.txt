# Unit tests for the content editors' SQL (src/noggit/content, npc, quest, item). Needs only Qt5 Core/Test
# and the MySQL client library, so it can be built on its own:
#
#   cmake -S test/content -B build-content-tests && cmake --build build-content-tests && ctest --test-dir build-content-tests
#
# or from the main project with -DNOGGIT_BUILD_TESTS=ON.
#
# The live tests run only when NOGGIT_NPC_TEST_DB is set (see LiveDb.hpp); they work in a scratch schema and
# never write to the world database they copy from.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_NAME)
  project(noggit_content_tests LANGUAGES CXX)
  enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt5 COMPONENTS Core Test REQUIRED)
find_library(CONTENT_TEST_MYSQL_LIBRARY NAMES mariadb libmariadb mysqlclient libmysql)
find_path(CONTENT_TEST_MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql mariadb)

set(NOGGIT_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
file(GLOB CONTENT_SOURCES CONFIGURE_DEPENDS
  ${NOGGIT_SRC}/noggit/content/*.cpp
  ${NOGGIT_SRC}/noggit/npc/*.cpp
  ${NOGGIT_SRC}/noggit/quest/*.cpp
  ${NOGGIT_SRC}/noggit/item/*.cpp
)
# The database layer, connected to the live tests' scratch schema by TestConnection.cpp.
list(APPEND CONTENT_SOURCES ${NOGGIT_SRC}/mysql/content_db.cpp)
file(GLOB CONTENT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

add_executable(content_tests ${CONTENT_TESTS} ${CONTENT_SOURCES})
set_target_properties(content_tests PROPERTIES AUTOMOC ON)
target_include_directories(content_tests PRIVATE ${NOGGIT_SRC} ${CONTENT_TEST_MYSQL_INCLUDE_DIR})
target_link_libraries(content_tests PRIVATE Qt5::Core Qt5::Test ${CONTENT_TEST_MYSQL_LIBRARY})

add_test(NAME content_tests COMMAND content_tests)
