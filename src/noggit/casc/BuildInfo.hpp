// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Noggit::Casc
{
  // One row of a CASC install's ".build.info" (the pipe-separated table Battle.net keeps next to the
  // product folders). A shared install lists every product that shares the store, e.g.
  //   wow_classic_era 1.15.9.69722 / wow_classic_era_ptr 2.5.6.69110 / wow_anniversary 2.5.6.69795.
  struct BuildInfoProduct
  {
    std::string code;      // "Product" column -> CascLib szCodeName
    std::string version;   // "Version" column, e.g. "1.15.9.69722"
    std::string branch;    // "Branch" column, e.g. "us"
    std::string build_key; // "Build Key" column (build config hash)
    bool active = false;   // "Active" column
  };

  // Empty when the file is missing or unparsable.
  std::vector<BuildInfoProduct> readBuildInfo(std::filesystem::path const& client_root);

  // The Version column of the given product, or "" when not listed.
  std::string productVersion(std::filesystem::path const& client_root, std::string const& product_code);

  // Where CascLib expects szLocalPath to point: the directory holding ".build.info". Accepts either that
  // directory or any of its product sub-directories (e.g. ".../_classic_era_") and returns "" if none.
  std::string findStorageRoot(std::filesystem::path const& any_path);
}
