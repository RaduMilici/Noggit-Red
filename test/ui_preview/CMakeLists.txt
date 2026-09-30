# Renders the content editors to PNG files without Noggit, a database or client data (fake lists, the real
# vmangos schema), to look at their layout:
#
#   cmake -S test/ui_preview -B build-ui-preview && cmake --build build-ui-preview
#   QT_QPA_PLATFORM=offscreen build-ui-preview/ui_preview <output directory>
cmake_minimum_required(VERSION 3.16)
project(noggit_ui_preview LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)

set(NOGGIT_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
file(GLOB PREVIEW_SOURCES CONFIGURE_DEPENDS
  ${NOGGIT_SRC}/noggit/content/*.cpp
  ${NOGGIT_SRC}/noggit/npc/*.cpp
  ${NOGGIT_SRC}/noggit/quest/*.cpp
  ${NOGGIT_SRC}/noggit/item/*.cpp
  ${NOGGIT_SRC}/noggit/ui/content/ContentLookups.cpp
  ${NOGGIT_SRC}/noggit/ui/content/ContentStyle.cpp
  ${NOGGIT_SRC}/noggit/ui/quest/Quest*Page.cpp
  ${NOGGIT_SRC}/noggit/ui/quest/QuestEditorDialog.cpp
  ${NOGGIT_SRC}/noggit/ui/item/ItemEditorDialog.cpp
)
add_executable(ui_preview main.cpp ${PREVIEW_SOURCES})
target_include_directories(ui_preview PRIVATE ${NOGGIT_SRC} ${CMAKE_CURRENT_LIST_DIR}/../content)
target_link_libraries(ui_preview PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
