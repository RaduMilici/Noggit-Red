# Release packagers supply one tested, platform-specific runtime payload.
# End users do not install a database, shell, Git, Docker, or build tools.
set(CREATOR_RUNTIME_BUNDLE "" CACHE PATH "Prepared Runtime directory to include in Creator installation")
if(CREATOR_RUNTIME_BUNDLE)
  foreach(required creator-runtime.json MariaDB/bin/mariadbd${CMAKE_EXECUTABLE_SUFFIX}
      MariaDB/bin/mariadb${CMAKE_EXECUTABLE_SUFFIX} realmd/realmd${CMAKE_EXECUTABLE_SUFFIX}
      mangosd/mangosd${CMAKE_EXECUTABLE_SUFFIX} realmd/realmd.conf.dist
      mangosd/mangosd.conf.dist DatabaseSeed/mysql mangosd/data/maps mangosd/data/dbc
      mangosd/data/vmaps mangosd/data/mmaps)
    if(NOT EXISTS "${CREATOR_RUNTIME_BUNDLE}/${required}")
      message(FATAL_ERROR "Incomplete Creator runtime: ${required}")
    endif()
  endforeach()
  install(DIRECTORY "${CREATOR_RUNTIME_BUNDLE}/" DESTINATION Runtime USE_SOURCE_PERMISSIONS)
  install(TARGETS noggit RUNTIME DESTINATION Noggit BUNDLE DESTINATION Noggit)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/dist/themes" "${CMAKE_SOURCE_DIR}/dist/definitions"
      DESTINATION Noggit PATTERN ".git" EXCLUDE)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/dist/listfile/" DESTINATION Noggit PATTERN ".git" EXCLUDE)
endif()
