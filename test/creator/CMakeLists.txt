cmake_minimum_required(VERSION 3.16)
project(CreatorIntegration LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt5 REQUIRED COMPONENTS Core Network)
find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql mariadb REQUIRED)
find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb REQUIRED)
file(GLOB CONTENT_SOURCES CONFIGURE_DEPENDS
  ../../src/noggit/content/*.cpp ../../src/noggit/quest/*.cpp ../../src/noggit/item/*.cpp)
add_executable(creator_integration CreatorIntegration.cpp ${CONTENT_SOURCES}
  ../../src/noggit/creator/Database.cpp
  ../../src/noggit/creator/SqlConnection.cpp
  ../../src/noggit/creator/Ssh.cpp
  ../../src/noggit/creator/ProductionProfile.cpp
  ../../src/noggit/creator/ProductionSync.cpp
  ../../src/noggit/creator/Services.cpp
  ../../src/noggit/creator/ChangeTracker.cpp
  ../../src/noggit/creator/ChangeExport.cpp
  ../../src/noggit/creator/ContentStore.cpp
  ../../src/noggit/creator/ItemService.cpp
  ../../src/noggit/creator/LootService.cpp
  ../../src/noggit/creator/VendorService.cpp
  ../../src/noggit/creator/TrainerService.cpp
  ../../src/noggit/creator/GossipService.cpp
  ../../src/noggit/runtime/RuntimeManager.cpp)
target_include_directories(creator_integration PRIVATE ../../src ${MYSQL_INCLUDE_DIR})
target_compile_definitions(creator_integration PRIVATE USE_MYSQL_UID_STORAGE)
target_link_libraries(creator_integration PRIVATE Qt5::Core Qt5::Network ${MYSQL_LIBRARY})

# Local change tracking without a database: merging, persistence, crash recovery, SQL generation.
enable_testing()
add_executable(change_tracker_tests ChangeTrackerTests.cpp
  ../../src/noggit/creator/ChangeTracker.cpp
  ../../src/noggit/runtime/RuntimeManager.cpp)
target_include_directories(change_tracker_tests PRIVATE ../../src)
target_link_libraries(change_tracker_tests PRIVATE Qt5::Core Qt5::Network)
add_test(NAME change_tracker COMMAND change_tracker_tests)

# SSH command building, error classification, host keys, and SQL comment safety; no server needed.
add_executable(ssh_tests SshTests.cpp
  ../../src/noggit/creator/Ssh.cpp
  ../../src/noggit/creator/ChangeTracker.cpp
  ../../src/noggit/runtime/RuntimeManager.cpp)
target_include_directories(ssh_tests PRIVATE ../../src)
target_link_libraries(ssh_tests PRIVATE Qt5::Core Qt5::Network)
add_test(NAME ssh COMMAND ssh_tests)

# Loot, vendor, trainer and dialogue rules without a database: validation, group chances, the drop
# simulator, dialogue rows and parsing, Local Changes summaries and their Sync SQL.
add_executable(service_tests ServiceTests.cpp
  ../../src/noggit/creator/LootService.cpp
  ../../src/noggit/creator/VendorService.cpp
  ../../src/noggit/creator/TrainerService.cpp
  ../../src/noggit/creator/GossipService.cpp
  ../../src/noggit/creator/ItemService.cpp
  ../../src/noggit/creator/Database.cpp
  ../../src/noggit/creator/SqlConnection.cpp
  ../../src/noggit/creator/ChangeTracker.cpp
  ../../src/noggit/runtime/RuntimeManager.cpp)
target_include_directories(service_tests PRIVATE ../../src ${MYSQL_INCLUDE_DIR})
target_compile_definitions(service_tests PRIVATE USE_MYSQL_UID_STORAGE)
target_link_libraries(service_tests PRIVATE Qt5::Core Qt5::Network ${MYSQL_LIBRARY})
add_test(NAME services COMMAND service_tests)
