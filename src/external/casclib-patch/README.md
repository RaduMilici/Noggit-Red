# CascLib source patches

`cmake/FindCascLib.cmake` fetches upstream CascLib (ladislav-zezula/CascLib, pinned commit) and copies the
files in this folder over the fetched tree before building it.

- `FileTree.cpp` (src/common): `CASC_FILE_TREE::InsertById` prefers a root record whose content is present in
  the local storage when a FileDataId is listed more than once. WoW Classic Forever 1.60.1 beta roots carry
  two records for most plain files; upstream keeps the first, which is frequently not downloaded, so
  `CascOpenFile` succeeded and every read failed (docs/client_re/42 sec 17.3).
- `FileTree.cpp` also exports `CascSetPreferredContentFlags(mask, value)`: between two LOCAL records of
  one FileDataId the one whose content flags match `value` under `mask` wins. noggit passes mask 0x1
  (the Forever Beta's 4x-resolution texture bit, 95,774 textures) from the Settings > Paths
  "Prefer HD texture variants" box before the storage opens (docs/client_re/42 sec 18).

Gotcha: the copy happens at CMake CONFIGURE time (`file(COPY_FILE ...)` runs in the script, not as a build
rule). After editing a file here, a plain `cmake --build` still compiles the previous copy in
`nrcln/_deps/casclib_upstream-src/` -- the first build of `CascSetPreferredContentFlags` failed with LNK2019
that way. Either re-run the configure step or copy the file over the fetched tree by hand before building.
