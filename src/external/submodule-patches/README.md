# Submodule patches

The four submodules below point at upstream GitLab repositories this fork cannot push to, so the
superproject records local-only submodule commits. Each patch here is the full diff of that local commit
against the upstream commit it sits on. After `git submodule update --init` on a fresh clone, check out the
upstream parent and apply the patch with `git apply` inside the submodule.

- `cmake.patch` -> `cmake`
- `dist_definitions.patch` -> `dist/definitions`
- `src_external_blizzard-archive-library.patch` -> `src/external/blizzard-archive-library`
- `src_external_blizzard-database-library.patch` -> `src/external/blizzard-database-library`
