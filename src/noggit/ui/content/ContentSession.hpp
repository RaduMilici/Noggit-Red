// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// One editing session of the content editors: an open database connection, the pick lists, the quest list,
// and the hooks the map view provides (cursor position, 3D preview, refreshing after changes). Open one per
// user action (a browser window, a button click). MySQL builds only.

#include <mysql/content_db.h>
#include <noggit/quest/QuestSavePlan.hpp>
#include <noggit/ui/content/ContentLookups.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QWidget;

namespace Noggit::Ui::Content
{
  class ContentSession
  {
  public:
    // Connects and loads the lists; shows what went wrong and returns null on failure.
    static std::unique_ptr<ContentSession> open(QWidget* parent);

    mysql::content::Database& db() { return *_db; }
    ContentLookups& lookups() { return *_lookups; }
    mysql::content::QuestList const& quests() const { return _quests; }
    bool reloadQuests();

    // Names for the save plans' summaries.
    Noggit::Quest::NameLookup names() const;

    // --- provided by the map view ---
    std::function<std::optional<WorldPosition>()> cursor_position;
    std::function<QWidget*()> make_npc_preview;                           // a 3D model view for the NPC editor
    std::function<void(QWidget* preview, std::uint32_t display, float scale)> show_npc_look;
    std::function<void()> on_world_changed; // NPCs / spawns changed: reload what the map shows

  private:
    ContentSession() = default;
    std::unique_ptr<mysql::content::Database> _db;
    std::unique_ptr<ContentLookups> _lookups;
    mysql::content::QuestList _quests;
  };
}
