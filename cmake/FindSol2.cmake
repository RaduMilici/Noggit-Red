# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# Dependency: sol2
FetchContent_Declare (sol2
        GIT_REPOSITORY https://github.com/tswow/sol2
        GIT_TAG b9c83d5ecf6bc9503dc66779f2395dc32dffb1e5
        )
FetchContent_MakeAvailable (sol2)

# sol2 3.2.3's optional<T&>::emplace calls a nonexistent construct() member.
# GCC 15 diagnoses the invalid template body even when emplace is not instantiated.
# Apply the two-line upstream fix (ThePhD/sol2@d805d02) to the pinned source.
set (_sol2_optional_header "${sol2_SOURCE_DIR}/include/sol/optional_implementation.hpp")
file (READ "${_sol2_optional_header}" _sol2_optional_contents)
set (_sol2_broken_emplace "this->construct(std::forward<Args>(args)...);")
set (_sol2_fixed_emplace
     "new (static_cast<void*>(this)) optional(std::in_place, std::forward<Args>(args)...);\n\t\t\treturn **this;")
string (FIND "${_sol2_optional_contents}" "${_sol2_broken_emplace}" _sol2_broken_emplace_pos)
if (NOT _sol2_broken_emplace_pos EQUAL -1)
  string (REPLACE "${_sol2_broken_emplace}" "${_sol2_fixed_emplace}"
          _sol2_optional_contents "${_sol2_optional_contents}")
  file (WRITE "${_sol2_optional_header}" "${_sol2_optional_contents}")
endif ()

# sol2::sol2 neither links lua nor sets include directories as system so will clobber us with
# loads of warnings, sadly. It also wants to be install(EXPORT)ed which is not what we want.
add_library (sane-sol2 INTERFACE)
add_library (sol2::sane ALIAS sane-sol2)
target_link_libraries (sane-sol2 INTERFACE Lua::Lua)
target_include_directories (sane-sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
