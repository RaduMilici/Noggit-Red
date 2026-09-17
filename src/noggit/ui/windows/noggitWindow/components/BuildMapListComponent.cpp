// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/windows/noggitWindow/components/BuildMapListComponent.hpp>
#include <noggit/ui/windows/noggitWindow/widgets/MapListItem.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/DBC.h>
#include <noggit/World.h>
#include <noggit/Log.h>
#include <noggit/application/Utils.hpp>
#include <QMenuBar>
#include <QAction>
#include <QObject>

#include <exception>
#include <algorithm>

using namespace Noggit::Ui::Component;

namespace
{
  bool isUsableDisplayString(std::string const& value)
  {
    return !value.empty() && value.size() < 1024;
  }

  std::string classicLocalizedStringOrDefault(DBCFile::Record const& record,
                                              std::size_t field,
                                              std::string const& default_value)
  {
    if (field > 0)
    {
      auto shifted_preferred = std::string(record.getLocalizedString(field - 1, 0));
      if (isUsableDisplayString(shifted_preferred))
        return shifted_preferred;

      auto shifted_fallback = std::string(record.getLocalizedString(field - 1));
      if (isUsableDisplayString(shifted_fallback))
        return shifted_fallback;
    }

    auto preferred = std::string(record.getLocalizedString(field, 0));
    if (isUsableDisplayString(preferred))
      return preferred;

    auto fallback = std::string(record.getLocalizedString(field));
    if (isUsableDisplayString(fallback))
      return fallback;

    return default_value;
  }

  std::string localizedColumnValueOrDefault(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record,
                                            std::string const& column_name,
                                            std::string const& default_value)
  {
    auto itr = record.Columns.find(column_name);
    if (itr == record.Columns.end())
      return default_value;

    for (auto const& value: itr->second.Values)
    {
      if (isUsableDisplayString(value))
        return value;
    }

    if (isUsableDisplayString(itr->second.Value))
      return itr->second.Value;

    return default_value;
  }

  std::string columnValueOrDefault(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record,
                                   std::string const& column_name,
                                   std::string const& default_value)
  {
    auto itr = record.Columns.find(column_name);
    if (itr == record.Columns.end() || itr->second.Value.empty())
      return default_value;

    return itr->second.Value;
  }

  std::string lowercaseCopy(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character)
                   {
                     return static_cast<char>(std::tolower(character));
                   });
    return value;
  }

  bool isTrackedTurtleMap(std::string const& value)
  {
    auto lower = lowercaseCopy(value);
    return lower.find("timbermaw") != std::string::npos
        || lower.find("karazhan") != std::string::npos;
  }
}

void BuildMapListComponent::buildMapList(Noggit::Ui::Windows::NoggitWindow* parent)
{
  LogDebug << "BuildMapListComponent::buildMapList begin" << std::endl;

  try
  {
    parent->_continents_table->clear();
    LogDebug << "BuildMapListComponent::buildMapList after clear" << std::endl;

    auto pinned_maps = std::vector<Widget::MapListData>();
    auto maps = std::vector<Widget::MapListData>();

    if (parent->_project->projectVersion == Noggit::Project::ProjectVersion::CLASSIC
        || Noggit::Project::usesSynthesizedDbc(parent->_project->projectVersion))
    {
      for (DBCFile::Iterator iterator = gMapDB.begin(); iterator != gMapDB.end(); ++iterator)
      {
        try
        {
          auto record = *iterator;

          Widget::MapListData map_list_data{};
          map_list_data.map_id = record.getUInt(MapDB::MapID);

          auto directory = record.getString(MapDB::InternalName);
          auto display_name = classicLocalizedStringOrDefault(record, MapDB::Name, directory);
          if (display_name.empty())
            display_name = directory;
          if (display_name.empty())
            display_name = "Map " + std::to_string(map_list_data.map_id);

          map_list_data.map_name = QString::fromUtf8(display_name.c_str());
          map_list_data.map_type_id = record.getInt(MapDB::AreaType);
          map_list_data.expansion_id = gMapDB.getFieldCount() > MapDB::ExpansionID ? record.getInt(MapDB::ExpansionID) : 0;

          auto editable_world = World::IsEditableWorld(record);
          if (isTrackedTurtleMap(display_name) || isTrackedTurtleMap(directory))
          {
            LogDebug << "Tracked map row: id=" << map_list_data.map_id
                     << " name='" << display_name << "'"
                     << " directory='" << directory << "'"
                     << " type=" << map_list_data.map_type_id
                     << " expansion=" << map_list_data.expansion_id
                     << " editable=" << editable_world << std::endl;
          }

          if (map_list_data.map_type_id < 0 || map_list_data.map_type_id > 5 || !editable_world)
            continue;

          map_list_data.wmo_map = World::IsWMOWorld(record);

          auto project_pinned_maps = parent->_project->PinnedMaps;
          auto pinned_map_found = std::find_if(std::begin(project_pinned_maps), std::end(project_pinned_maps),
                                               [&](Project::NoggitProjectPinnedMap pinned_map)
                                               {
                                                 return pinned_map.MapId == map_list_data.map_id;
                                               });

          if (pinned_map_found != std::end(project_pinned_maps))
          {
            map_list_data.pinned = true;
            pinned_maps.push_back(map_list_data);
          }
          else
          {
            maps.push_back(map_list_data);
          }
        }
        catch (std::exception const& exception)
        {
          LogError << "BuildMapListComponent::buildMapList skipping classic map row: " << exception.what() << std::endl;
        }
      }
    }
    else
    {
      const auto& table = std::string("Map");
      auto map_table = parent->_project->ClientDatabase->LoadTable(table, readFileAsIMemStream);
      auto iterator = map_table.Records();
    LogDebug << "BuildMapListComponent::buildMapList after LoadTable" << std::endl;

      while (iterator.HasRecords())
      {
        try
        {
          auto record = iterator.Next();

          Widget::MapListData map_list_data{};

          map_list_data.map_id = record.RecordId;
          auto display_name = localizedColumnValueOrDefault(record, "MapName_lang", columnValueOrDefault(record, "Directory", ""));
          if (display_name.empty())
            display_name = "Map " + std::to_string(map_list_data.map_id);

          auto directory = columnValueOrDefault(record, "Directory", "");

          map_list_data.map_name = QString::fromUtf8(display_name.c_str());
          map_list_data.map_type_id = std::stoi(columnValueOrDefault(record, "InstanceType", "0"));
          map_list_data.expansion_id = std::stoi(columnValueOrDefault(record, "ExpansionID", "0"));

          auto editable_world = World::IsEditableWorld(record);

          if (map_list_data.map_type_id < 0 || map_list_data.map_type_id > 5 || !editable_world)
            continue;

          map_list_data.wmo_map = (World::IsWMOWorld(record));

          auto project_pinned_maps = parent->_project->PinnedMaps;

          auto pinned_map_found = std::find_if(std::begin(project_pinned_maps), std::end(project_pinned_maps),
                                               [&](Project::NoggitProjectPinnedMap pinned_map)
                                             {
                                               return pinned_map.MapId == map_list_data.map_id;
                                             });

          if (pinned_map_found != std::end(project_pinned_maps))
          {
            map_list_data.pinned = true;
            pinned_maps.push_back(map_list_data);
          }
          else
          {
            maps.push_back(map_list_data);
          }
        }
        catch (std::exception const& exception)
        {
          LogError << "BuildMapListComponent::buildMapList skipping map row: " << exception.what() << std::endl;
        }
      }
    }
    LogDebug << "BuildMapListComponent::buildMapList after collect records" << std::endl;

    auto sort_by_map_id = [](Widget::MapListData const& left, Widget::MapListData const& right)
    {
      return left.map_id < right.map_id;
    };

    std::sort(pinned_maps.begin(), pinned_maps.end(), sort_by_map_id);
    std::sort(maps.begin(), maps.end(), sort_by_map_id);
    pinned_maps.insert(pinned_maps.end(), maps.begin(), maps.end());
    LogDebug << "BuildMapListComponent::buildMapList after sort" << std::endl;

    for (auto const& map: pinned_maps)
    {
      auto map_list_item = new Widget::MapListItem(map, parent->_continents_table);
      auto item = new QListWidgetItem(parent->_continents_table);

      if (map.pinned)
      {
        QObject::connect(map_list_item, &QListWidget::customContextMenuRequested,
                         [=](const QPoint& pos)
                         {
                           QMenu context_menu(map_list_item->tr("Context menu"), map_list_item);

                           QAction action_1("Unpin Map", map_list_item);
                           auto icon = QIcon();
                           icon.addPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
                           action_1.setIcon(icon);

                           QObject::connect(&action_1, &QAction::triggered, [=]()
                           {
                             parent->handleEventMapListContextMenuUnpinMap(map.map_id);
                           });

                           context_menu.addAction(&action_1);
                           context_menu.exec(map_list_item->mapToGlobal(pos));
                         });
      }
      else
      {
        QObject::connect(map_list_item, &QListWidget::customContextMenuRequested,
                         [=](const QPoint& pos)
                         {
                           QMenu context_menu(map_list_item->tr("Context menu"), map_list_item);
                           QAction action_1("Pin Map", map_list_item);
                           auto icon = QIcon();
                           icon.addPixmap(FontAwesomeIcon(FontAwesome::star).pixmap(QSize(16, 16)));
                           action_1.setIcon(icon);

                           QObject::connect(&action_1, &QAction::triggered, [=]()
                           {
                             parent->handleEventMapListContextMenuPinMap(map.map_id, map.map_name.toStdString());
                           });

                           context_menu.addAction(&action_1);
                           context_menu.exec(map_list_item->mapToGlobal(pos));
                         });
      }

      item->setSizeHint(map_list_item->minimumSizeHint());
      item->setData(Qt::UserRole, QVariant(map.map_id));
      parent->_continents_table->setItemWidget(item, map_list_item);
    }
    LogDebug << "BuildMapListComponent::buildMapList after create widgets" << std::endl;

    if (parent->_project->projectVersion != Noggit::Project::ProjectVersion::CLASSIC
        && !Noggit::Project::usesSynthesizedDbc(parent->_project->projectVersion))
    {
      parent->_project->ClientDatabase->UnloadTable("Map"); // only loaded on the DatabaseLib route above
    }

    LogDebug << "BuildMapListComponent::buildMapList end" << std::endl;
  }
  catch (std::exception const& exception)
  {
    LogError << "BuildMapListComponent::buildMapList caught std::exception: " << exception.what() << std::endl;
    throw;
  }
}
