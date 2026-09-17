// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/casc/BuildInfo.hpp>

#include <fstream>
#include <map>

namespace Noggit::Casc
{
  namespace
  {
    std::vector<std::string> split(std::string const& line, char sep)
    {
      std::vector<std::string> out;
      std::string cur;
      for (char c : line)
      {
        if (c == sep)
        {
          out.push_back(cur);
          cur.clear();
        }
        else if (c != '\r')
        {
          cur += c;
        }
      }
      out.push_back(cur);
      return out;
    }
  }

  std::vector<BuildInfoProduct> readBuildInfo(std::filesystem::path const& client_root)
  {
    std::vector<BuildInfoProduct> products;

    std::ifstream in(client_root / ".build.info");
    if (!in.is_open())
    {
      return products;
    }

    // header: "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|...|Version!STRING:0|...|Product!STRING:0"
    std::string header;
    if (!std::getline(in, header))
    {
      return products;
    }

    std::map<std::string, std::size_t> columns;
    {
      auto const names = split(header, '|');
      for (std::size_t i = 0; i < names.size(); ++i)
      {
        auto const bang = names[i].find('!');
        columns.emplace(bang == std::string::npos ? names[i] : names[i].substr(0, bang), i);
      }
    }

    auto const column = [&](std::vector<std::string> const& row, char const* name) -> std::string
    {
      auto const it = columns.find(name);
      if (it == columns.end() || it->second >= row.size())
      {
        return {};
      }
      return row[it->second];
    };

    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty())
      {
        continue;
      }
      auto const row = split(line, '|');
      BuildInfoProduct product;
      product.code = column(row, "Product");
      product.version = column(row, "Version");
      product.branch = column(row, "Branch");
      product.build_key = column(row, "Build Key");
      product.active = column(row, "Active") == "1";
      if (!product.code.empty())
      {
        products.push_back(product);
      }
    }

    return products;
  }

  std::string productVersion(std::filesystem::path const& client_root, std::string const& product_code)
  {
    for (auto const& product : readBuildInfo(client_root))
    {
      if (product.code == product_code)
      {
        return product.version;
      }
    }
    return {};
  }

  std::string findStorageRoot(std::filesystem::path const& any_path)
  {
    std::error_code ec;
    std::filesystem::path path = any_path;
    for (int depth = 0; depth < 3 && !path.empty(); ++depth)
    {
      if (std::filesystem::exists(path / ".build.info", ec))
      {
        return path.generic_string();
      }
      auto const parent = path.parent_path();
      if (parent == path)
      {
        break;
      }
      path = parent;
    }
    return {};
  }
}
