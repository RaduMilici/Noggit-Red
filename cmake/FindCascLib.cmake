# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# adds target CascLib
#
# 2026-09-15: built from UPSTREAM SOURCE instead of the prebuilt CascLib 2.1 binary that used to be
# fetched from gitlab.com/prophecy-rp/dependencies (dep-casclib). That binary predates root manifest
# version 2 (every client since 11.1 retail, 1.15.x Classic Era, 2.5.x Anniversary): it opened the
# storage and named the product but every file open by fileDataID failed (features=0x5, err=2).
# Upstream master (CascLib 3.0) reads those stores -- proven against the shared
# "F:\World of Warcraft" install, see docs/client_re/41_modern_casc_client_support_research.md.
if (NOT TARGET CascLib)
  FetchContent_Declare(
    casclib_upstream
    GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
    GIT_TAG        2a280f5a231966dc5d1b534978dd9f9f04a374cd # master 2026-08-22 (CASCLIB_VERSION 3.0)
  )

  FetchContent_GetProperties(casclib_upstream)
  if (NOT casclib_upstream_POPULATED)
    MESSAGE(STATUS "---------------------------------------------")
    MESSAGE(STATUS "Installing CascLib (upstream source)...")
    FetchContent_PopulateFast(casclib_upstream)
  endif()

  # static, ANSI (noggit passes narrow paths), no shared lib / tests. The upstream CMakeLists still
  # declares cmake_minimum_required(3.2); the root CMakeLists sets CMAKE_POLICY_VERSION_MINIMUM 3.5
  # which covers this subdirectory on CMake 4.
  SET(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "CascLib: shared library" FORCE)
  SET(CASC_BUILD_STATIC_LIB ON  CACHE BOOL "CascLib: static library" FORCE)
  SET(CASC_BUILD_TESTS      OFF CACHE BOOL "CascLib: test application" FORCE)
  SET(CASC_UNICODE          OFF CACHE BOOL "CascLib: UNICODE build" FORCE)

  # Local patches on top of the pinned upstream (see src/external/casclib-patch/README.md).
  file(COPY_FILE "${CMAKE_SOURCE_DIR}/src/external/casclib-patch/FileTree.cpp"
                 "${casclib_upstream_SOURCE_DIR}/src/common/FileTree.cpp" ONLY_IF_DIFFERENT)
  ADD_SUBDIRECTORY(${casclib_upstream_SOURCE_DIR} ${casclib_upstream_BINARY_DIR})
  set_target_properties(casc_static PROPERTIES FOLDER "external")

  # casc_static carries its include dir (src/), CASCLIB_NO_AUTO_LINK_LIBRARY and wininet as PUBLIC usage
  # requirements, so the INTERFACE target below is all the consumers need.
  add_library (CascLib INTERFACE)
  target_link_libraries (CascLib INTERFACE casc_static)

  MESSAGE(STATUS "Casclib source          : ${casclib_upstream_SOURCE_DIR}")
  MESSAGE(STATUS "Casclib Installed!")
  MESSAGE(STATUS "---------------------------------------------")
endif()

set (CascLib_FOUND TRUE)
