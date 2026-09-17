//Folder to contain all of the project related files
#pragma once

#include <map>
#include <memory>
#include <blizzard-archive-library/include/CASCArchive.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-archive-library/include/Exception.hpp>
#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/casc/BuildInfo.hpp>
#include <noggit/db2/ModernDBC.hpp>
#include <noggit/Log.h>
#include <noggit/ui/windows/downloadFileDialog/DownloadFileDialog.h>
#include <QJsonDocument>
#include <QMessageBox>
#include <QJsonObject>
#include <QFile>
#include <filesystem>
#include <fstream>
#include <vector>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QFile>
#include <QString>
#include <QObject>
#include <QtCore/QSettings>
#include <QString>
#include <thread>
#include <chrono>
#include <cassert>
#include <glm/vec3.hpp>

#include "ApplicationProjectReader.h"
#include "ApplicationProjectWriter.h"

namespace Noggit::Project
{
  enum class ProjectVersion
  {
    CLASSIC,
    BC,
    WOTLK,
    CATA,
    PANDARIA,
    WOD,
    LEGION,
    BFA,
    SL,
    // CASC "classic re-release" clients sharing the modern file formats (WDT MAID + split ADT, MD21,
    // GFID WMO, WDC5 DB2). The product code (NoggitProject::ClientProduct) selects the build inside the
    // shared store. See docs/client_re/41_modern_casc_client_support_research.md.
    CLASSIC_ERA,   // wow_classic_era (1.15.x)
    ANNIVERSARY    // wow_anniversary (2.5.x)
  };

  // CASC-backed clients: files are addressed by fileDataID and the modern file formats apply.
  inline bool isModernCascVersion(ProjectVersion version)
  {
    return version == ProjectVersion::SL || version == ProjectVersion::CLASSIC_ERA || version == ProjectVersion::ANNIVERSARY;
  }

  // Versions whose DB2 tables are served through the fixed-column DBCFile API (gMapDB, gLightDB, ...)
  // by re-emitting them in the WotLK layout (noggit/db2/ModernDBC). Shadowlands keeps the older
  // DatabaseLib route; there is no DB2 writer for either yet.
  inline bool usesSynthesizedDbc(ProjectVersion version)
  {
    return version == ProjectVersion::CLASSIC_ERA || version == ProjectVersion::ANNIVERSARY;
  }

  struct ClientVersionFactory
  {
    static ProjectVersion mapToEnumVersion(std::string const& projectVersion)
    {
      if (projectVersion == "Turtle WoW" || projectVersion == "Vanilla")
        return ProjectVersion::CLASSIC;
      if (projectVersion == "Wrath Of The Lich King")
        return ProjectVersion::WOTLK;
      if (projectVersion == "Shadowlands")
        return ProjectVersion::SL;
      if (projectVersion == "Classic Era")
        return ProjectVersion::CLASSIC_ERA;
      if (projectVersion == "Anniversary")
        return ProjectVersion::ANNIVERSARY;

      assert(false);
      return ProjectVersion::WOTLK;
    }

    static std::string MapToStringVersion(ProjectVersion const& projectVersion)
    {
      if (projectVersion == ProjectVersion::CLASSIC)
        return std::string("Turtle WoW");
      if (projectVersion == ProjectVersion::WOTLK)
        return std::string("Wrath Of The Lich King");
      if (projectVersion == ProjectVersion::SL)
        return std::string("Shadowlands");
      if (projectVersion == ProjectVersion::CLASSIC_ERA)
        return std::string("Classic Era");
      if (projectVersion == ProjectVersion::ANNIVERSARY)
        return std::string("Anniversary");

      assert(false);
      return std::string("Wrath Of The Lich King");
    }

    // Default .build.info product code for a CASC version (used when a project file predates the field).
    static std::string defaultProduct(ProjectVersion const& projectVersion)
    {
      if (projectVersion == ProjectVersion::CLASSIC_ERA)
        return std::string("wow_classic_era");
      if (projectVersion == ProjectVersion::ANNIVERSARY)
        return std::string("wow_anniversary");
      return std::string("wow");
    }
  };

  struct NoggitProjectBookmarkMap
  {
    int map_id;
    std::string name;
    glm::vec3 position;
    float camera_yaw;
    float camera_pitch;
  };

  struct NoggitProjectPinnedMap
  {
    int MapId;
    std::string MapName;
  };

  struct NoggitProjectObjectPalette
  {
      int MapId;
      std::vector<std::string> Filepaths;
  };

  struct NoggitProjectTexturePalette
  {
      int MapId;
      std::vector<std::string> Filepaths;
  };

  struct NoggitProjectSelectionGroups
  {
      int MapId;
      // Might let the user name them later if they get some list UI
      std::vector<std::vector<unsigned int>> SelectionGroups;
  };

  class NoggitProject
  {
    std::shared_ptr<ApplicationProjectWriter> _projectWriter;
  public:
    std::string ProjectPath;
    std::string ProjectName;
    std::string ClientPath;
    // CASC clients only: the .build.info "Product" code (wow_classic_era, wow_anniversary, wow, ...)
    // that selects the build inside a shared store. Empty = ClientVersionFactory::defaultProduct().
    std::string ClientProduct;
    // Archive basenames this project must NOT mount (json "IgnoredArchives"). For clients that
    // gate content archives at runtime (Ascension HD models in patch-CHA.MPQ: mounted by the game
    // only with the HD toggle on) -- noggit otherwise mounts everything and renders models the
    // player's client never uses (doc 40 sec 14). Applied via NOGGIT_EXCLUDE_ARCHIVES before
    // ClientData construction in loadProject.
    std::vector<std::string> IgnoredArchives;
    ProjectVersion projectVersion;
    std::vector<NoggitProjectPinnedMap> PinnedMaps;
    std::vector<NoggitProjectBookmarkMap> Bookmarks;
    std::shared_ptr<BlizzardDatabaseLib::BlizzardDatabase> ClientDatabase;
    std::shared_ptr<BlizzardArchive::ClientData> ClientData;
    std::vector<NoggitProjectObjectPalette> ObjectPalettes;
    std::vector<NoggitProjectTexturePalette> TexturePalettes;
    std::vector<NoggitProjectSelectionGroups> ObjectSelectionGroups;

    NoggitProject()
    {
      PinnedMaps = std::vector<NoggitProjectPinnedMap>();
      Bookmarks = std::vector<NoggitProjectBookmarkMap>();
      ObjectPalettes = std::vector<NoggitProjectObjectPalette>();
      TexturePalettes = std::vector<NoggitProjectTexturePalette>();
      ObjectSelectionGroups = std::vector<NoggitProjectSelectionGroups>();

      _projectWriter = std::make_shared<ApplicationProjectWriter>();
    }

    void createBookmark(const NoggitProjectBookmarkMap& bookmark)
    {
      Bookmarks.push_back(bookmark);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void deleteBookmark()
    {

    }

    void pinMap(int map_id, const std::string& map_name)
    {
      auto pinnedMap = NoggitProjectPinnedMap();
      pinnedMap.MapName = map_name;
      pinnedMap.MapId = map_id;

      auto pinnedMapFound = std::find_if(std::begin(PinnedMaps), std::end(PinnedMaps),
                                         [&](Project::NoggitProjectPinnedMap pinnedMap)
                                         {
                                           return pinnedMap.MapId == map_id;
                                         });

      if (pinnedMapFound != std::end(PinnedMaps))
        return;

      PinnedMaps.push_back(pinnedMap);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void unpinMap(int mapId)
    {
      PinnedMaps.erase(std::remove_if(PinnedMaps.begin(), PinnedMaps.end(),
                                      [=](NoggitProjectPinnedMap pinnedMap)
                                      {
                                        return pinnedMap.MapId == mapId;
                                      }),
                       PinnedMaps.end());

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void saveTexturePalette(const NoggitProjectTexturePalette& new_texture_palette)
    {
        TexturePalettes.erase(std::remove_if(TexturePalettes.begin(), TexturePalettes.end(),
          [=](NoggitProjectTexturePalette texture_palette)
          {
              return texture_palette.MapId == new_texture_palette.MapId;
          }),
            TexturePalettes.end());

      TexturePalettes.push_back(new_texture_palette);

      _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void saveObjectPalette(const NoggitProjectObjectPalette& new_object_palette)
    {
        ObjectPalettes.erase(std::remove_if(ObjectPalettes.begin(), ObjectPalettes.end(),
            [=](NoggitProjectObjectPalette obj_palette)
            {
                return obj_palette.MapId == new_object_palette.MapId;
            }),
            ObjectPalettes.end());

        ObjectPalettes.push_back(new_object_palette);

        _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void saveObjectSelectionGroups(const NoggitProjectSelectionGroups& new_selection_groups)
    {
        ObjectSelectionGroups.erase(std::remove_if(ObjectSelectionGroups.begin(), ObjectSelectionGroups.end(),
            [=](NoggitProjectSelectionGroups proj_selection_group)
            {
                return proj_selection_group.MapId == new_selection_groups.MapId;
            }),
            ObjectSelectionGroups.end());

        ObjectSelectionGroups.push_back(new_selection_groups);

        _projectWriter->saveObjectSelectionGroups(this, std::filesystem::path(ProjectPath));
    }
  };

  class ApplicationProject
  {
    std::shared_ptr<NoggitProject> _active_project;
    std::shared_ptr<Application::NoggitApplicationConfiguration> _configuration;
  public:
    ApplicationProject(std::shared_ptr<Application::NoggitApplicationConfiguration> configuration)
    {
      _active_project = nullptr;
      _configuration = configuration;
    }

    void createProject(std::filesystem::path const& project_path, std::filesystem::path const& client_path,
                       std::string const& client_version, std::string const& project_name,
                       std::string const& client_product = std::string())
    {
      if (!std::filesystem::exists(project_path))
        std::filesystem::create_directory(project_path);

      auto project = NoggitProject();
      project.ProjectName = project_name;
      project.projectVersion = ClientVersionFactory::mapToEnumVersion(client_version);
      project.ClientPath = client_path.generic_string();
      project.ClientProduct = client_product;
      project.ProjectPath = project_path.generic_string();

      auto project_writer = ApplicationProjectWriter();
      project_writer.saveProject(&project, project_path);


    }

    std::shared_ptr<NoggitProject> loadProject(std::filesystem::path const& project_path)
    {
      ApplicationProjectReader project_reader{};
      auto project = project_reader.readProject(project_path);

      if(!project.has_value())
        return {};

      // Record the active project so per-project settings (e.g. MySQL connection, see MySqlSettings.hpp)
      // can be namespaced by it -- different projects (3.3.5a vs Turtle) keep their own settings.
      QSettings().setValue("project/current_path",
                           QString::fromStdString(project_path.generic_string()));

      project_reader.readPalettes(&project.value());
      project_reader.readObjectSelectionGroups(&project.value());

      std::string dbd_file_directory = _configuration->ApplicationDatabaseDefinitionsPath;

      BlizzardDatabaseLib::Structures::Build client_build("3.3.5.12340");
      auto client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
      auto client_archive_locale = BlizzardArchive::Locale::AUTO;
      if (project->projectVersion == ProjectVersion::CLASSIC)
      {
        client_archive_version = BlizzardArchive::ClientVersion::CLASSIC;
        client_build = BlizzardDatabaseLib::Structures::Build("1.12.1.5875");
        client_archive_locale = BlizzardArchive::Locale::AUTO;
      }

      if (project->projectVersion == ProjectVersion::SL)
      {
        client_archive_version = BlizzardArchive::ClientVersion::SL;
        client_build = BlizzardDatabaseLib::Structures::Build("9.1.0.39584");
        client_archive_locale = BlizzardArchive::Locale::enUS;
      }

      if (project->projectVersion == ProjectVersion::WOTLK)
      {
        client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
        client_build = BlizzardDatabaseLib::Structures::Build("3.3.5.12340");
        client_archive_locale = BlizzardArchive::Locale::AUTO;
      }

      // Modern CASC clients: the product picks the build inside the shared store, the build string comes
      // from the store's own .build.info (so a client patch does not need a noggit change), and the DB2
      // tables are re-emitted as WotLK DBCs (noggit/db2/ModernDBC) -- the DBD build is used there as the
      // fallback when a table's layout hash is unknown to the definitions.
      std::string client_build_string;
      if (usesSynthesizedDbc(project->projectVersion))
      {
        client_archive_version = project->projectVersion == ProjectVersion::CLASSIC_ERA
                               ? BlizzardArchive::ClientVersion::CLASSIC_ERA
                               : BlizzardArchive::ClientVersion::ANNIVERSARY;
        if (project->ClientProduct.empty())
          project->ClientProduct = ClientVersionFactory::defaultProduct(project->projectVersion);

        std::string const storage_root = Noggit::Casc::findStorageRoot(project->ClientPath);
        if (!storage_root.empty() && storage_root != project->ClientPath)
        {
          LogDebug << "ApplicationProject::loadProject: client path '" << project->ClientPath
                   << "' resolved to the CASC storage root '" << storage_root << "'" << std::endl;
          project->ClientPath = storage_root;
        }

        client_build_string = Noggit::Casc::productVersion(project->ClientPath, project->ClientProduct);
        if (client_build_string.empty())
        {
          client_build_string = project->projectVersion == ProjectVersion::CLASSIC_ERA ? "1.15.9.69722" : "2.5.6.69795";
          LogError << "ApplicationProject::loadProject: product '" << project->ClientProduct << "' not found in "
                   << project->ClientPath << "/.build.info; assuming build " << client_build_string << std::endl;
        }
        client_build = BlizzardDatabaseLib::Structures::Build(client_build_string);
        client_archive_locale = BlizzardArchive::Locale::enUS;
        Noggit::DB2::reset();
        Noggit::DB2::setClientBuild(client_build_string);
      }

      project->ClientDatabase = std::make_shared<BlizzardDatabaseLib::BlizzardDatabase>(dbd_file_directory, client_build);

      // Project-level archive exclusion (json "IgnoredArchives") -> handed to ClientData through
      // NOGGIT_EXCLUDE_ARCHIVES (its single mount choke point reads it; doc 40 sec 14). ALWAYS
      // (re)written per load -- empty when the project lists nothing -- so a previous project's
      // exclusion never leaks into the next one opened in the same session. A user-set env var is
      // overridden while a project is open (project setting wins), which is the intended precedence.
      {
        std::string joined;
        for (auto const& name : project->IgnoredArchives)
        {
          if (!joined.empty()) joined += ';';
          joined += name;
        }
        qputenv("NOGGIT_EXCLUDE_ARCHIVES", QByteArray::fromStdString(joined));
        if (!joined.empty())
          LogDebug << "Project ignores archives: " << joined << std::endl;
      }

      // CASC listfile: <project>/listfile.csv wins (ClientData), then the Settings > Paths choice, then the
      // application default next to the executable.
      std::string listfile_path = QSettings().value("casc/listfile_path").toString().toStdString();
      if (listfile_path.empty())
        listfile_path = _configuration->ApplicationListFilePath;

      // Texture fidelity (Settings > Paths > "Prefer HD texture variants"): the WoW Classic Forever 1.60.1
      // beta root lists 95,774 textures twice, content flag 0x1 marking the 4x-resolution variant
      // (512x1024 vs 128x256 for the same file id). Only records present in the local store are ever
      // used; the choice is resolved once when the storage opens, so it takes effect on project open.
      // docs/client_re/42 sec 18.
      {
        bool const prefer_hd = QSettings().value("casc/prefer_hd_textures", true).toBool();
        BlizzardArchive::Archive::CASCArchive::setPreferredContentFlags(0x1u, prefer_hd ? 0x1u : 0x0u);
      }

      try
      {
        project->ClientData = std::make_shared<BlizzardArchive::ClientData>(
            project->ClientPath, client_archive_version, client_archive_locale, project_path.generic_string()
            , project->ClientProduct, listfile_path);

        LogDebug << "ApplicationProject::loadProject project_path='" << project->ProjectPath
                 << "' client_path='" << project->ClientPath
                 << "' project_version=" << static_cast<int>(project->projectVersion)
                 << "' requested_archive_version=" << static_cast<int>(client_archive_version)
                 << "' resolved_archive_version=" << static_cast<int>(project->ClientData->version())
                 << std::endl;
      }
      catch (BlizzardArchive::Exceptions::Locale::LocaleNotFoundError&)
      {
        QMessageBox::critical(nullptr, "Error", "The client does not appear to be valid.");
        return {};
      }

      return std::make_shared<NoggitProject>(project.value());
    }
  };
}
