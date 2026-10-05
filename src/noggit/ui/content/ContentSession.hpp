// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// One editing session of the quest and item editors: the pick lists, the quest list, which quests and items
// were made in Noggit, and what the map view provides (the cursor position). Open one per user action (a
// browser window, a menu entry). Reads and writes go through the local Creator database (ContentStore).

#include <noggit/creator/ContentStore.hpp>
#include <noggit/ui/content/ContentLookups.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>

class QWidget;

namespace Noggit::Ui::Content
{
  class ContentSession
  {
  public:
    // Loads the lists; shows what went wrong and returns null on failure (e.g. the local server is stopped).
    static std::unique_ptr<ContentSession> open(QWidget* parent);

    ContentLookups& lookups() { return *_lookups; }
    Creator::Layouts& layouts() { return _layouts; }
    Creator::QuestList const& quests() const { return _quests; }
    bool reloadQuests();

    // Made in Noggit, so the editors may change and delete them.
    bool ownsQuest(std::uint32_t quest) const { return _own_quests.count(quest) > 0; }
    bool ownsItem(std::uint32_t item) const { return _own_items.count(item) > 0; }
    void setOwnQuest(std::uint32_t quest, bool own);
    void setOwnItem(std::uint32_t item, bool own);

    // Names for the save plans' summaries.
    Noggit::Quest::NameLookup names() const;

    // --- provided by the map view ---
    std::function<std::optional<WorldPosition>()> cursor_position;

  private:
    ContentSession() = default;
    Creator::Layouts _layouts;
    std::unique_ptr<ContentLookups> _lookups;
    Creator::QuestList _quests;
    std::set<std::uint32_t> _own_quests, _own_items;
  };
}
