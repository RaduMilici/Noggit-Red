// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/content/ClientData.hpp>

#include <noggit/DBC.h>
#include <noggit/TextureManager.h>
#include <noggit/project/CurrentProject.hpp>

#include <algorithm>
#include <map>

namespace Noggit::Ui::Content::ClientData
{
  namespace
  {
    std::string localized(DBCFile::Record const& record, std::size_t field)
    {
      char const* text = record.getLocalizedString(field);
      return text ? std::string(text) : std::string();
    }

    void sortByName(std::vector<NamedEntry>& entries)
    {
      std::sort(entries.begin(), entries.end(), [](NamedEntry const& a, NamedEntry const& b)
      {
        return QString::localeAwareCompare(QString::fromStdString(a.name), QString::fromStdString(b.name)) < 0;
      });
    }

    // "friendly to Alliance, hostile to Horde" from FactionTemplate's masks (0x2 Alliance, 0x4 Horde).
    std::string reaction(std::uint32_t friendly_mask, std::uint32_t hostile_mask)
    {
      auto const of = [&](std::uint32_t mask)
      {
        return (hostile_mask & mask) ? std::string("hostile") : (friendly_mask & mask) ? std::string("friendly") : std::string("neutral");
      };
      auto const alliance = of(0x2);
      auto const horde = of(0x4);
      return alliance == horde ? alliance + " to everyone" : alliance + " to Alliance, " + horde + " to Horde";
    }
  }

  bool vanillaClient()
  {
    auto const* project = Noggit::Project::CurrentProject::get();
    return !project || project->projectVersion == Noggit::Project::ProjectVersion::CLASSIC;
  }

  std::vector<NamedEntry> zones()
  {
    std::vector<NamedEntry> out;
    try
    {
      for (auto it = gAreaDB.begin(); it != gAreaDB.end(); ++it)
      {
        if (it->getUInt(AreaDB::Region) == 0)
        {
          if (auto name = localized(*it, AreaDB::Name); !name.empty())
          {
            out.push_back({it->getUInt(AreaDB::AreaID), std::move(name)});
          }
        }
      }
    }
    catch (...)
    {
    }
    sortByName(out);
    return out;
  }

  std::vector<NamedEntry> reputationFactions()
  {
    std::vector<NamedEntry> out;
    try
    {
      for (auto it = gFactionDB.begin(); it != gFactionDB.end(); ++it)
      {
        if (it->getInt(FactionDB::ReputationIndex) < 0)
        {
          continue;
        }
        if (auto name = localized(*it, FactionDB::Name); !name.empty())
        {
          out.push_back({it->getUInt(FactionDB::ID), std::move(name)});
        }
      }
    }
    catch (...)
    {
    }
    sortByName(out);
    return out;
  }

  std::vector<NamedEntry> factionTemplates()
  {
    std::vector<NamedEntry> out;
    try
    {
      for (auto it = gFactionTemplateDB.begin(); it != gFactionTemplateDB.end(); ++it)
      {
        std::string name;
        try
        {
          name = localized(gFactionDB.getByID(it->getUInt(FactionTemplateDB::Faction)), FactionDB::Name);
        }
        catch (...)
        {
        }
        out.push_back({it->getUInt(FactionTemplateDB::ID),
                       (name.empty() ? std::string("Unnamed") : name) + " -- "
                         + reaction(it->getUInt(FactionTemplateDB::FriendlyMask), it->getUInt(FactionTemplateDB::HostileMask))});
      }
    }
    catch (...)
    {
    }
    sortByName(out);
    return out;
  }

  QIcon itemIcon(std::uint32_t display)
  {
    try
    {
      std::string icon = gItemDisplayInfoDB.getByID(display).getString(ItemDisplayInfoDB::InventoryIcon);
      if (icon.empty())
      {
        return {};
      }
      if (QPixmap* pixmap = BLPRenderer::getInstance().render_blp_to_pixmap("Interface\\Icons\\" + icon + ".blp", 64, 64))
      {
        return QIcon(*pixmap);
      }
    }
    catch (...)
    {
    }
    return {};
  }

  QIcon spellIcon(std::uint32_t icon)
  {
    static std::map<std::uint32_t, QIcon> cache;
    if (auto it = cache.find(icon); it != cache.end())
    {
      return it->second;
    }
    QIcon result;
    try
    {
      std::string path = gSpellIconDB.getByID(icon).getString(SpellIconDB::TextureFilename);
      if (!path.empty())
      {
        if (QPixmap* pixmap = BLPRenderer::getInstance().render_blp_to_pixmap(path + ".blp", 64, 64))
        {
          result = QIcon(*pixmap);
        }
      }
    }
    catch (...)
    {
    }
    return cache[icon] = result;
  }

  QString creatureModelName(std::uint32_t display)
  {
    try
    {
      auto record = gCreatureDisplayInfoDB.getByID(display);
      auto model = gCreatureModelDataDB.getByID(record.getUInt(CreatureDisplayInfoDB::ModelID));
      QString path = QString::fromUtf8(model.getString(CreatureModelDataDB::ModelName));
      path.replace('\\', '/');
      return path.section('/', -1).section('.', 0, 0);
    }
    catch (...)
    {
      return {};
    }
  }
}
