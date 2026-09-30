// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The quest editor is a set of pages (Story, Who can take it, Objectives, Rewards, Givers, Events). Each is
// filled from and written back to the same QuestContent, so pages never talk to each other or to the
// database; the dialog and the workflow do that.

#include <mysql/content_db.h>
#include <noggit/quest/QuestSavePlan.hpp>
#include <noggit/ui/content/ContentLookups.hpp>

#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>
#include <optional>

class QVBoxLayout;

namespace Noggit::Ui::Quest
{
  enum class EditorMode
  {
    Create,
    Copy,
    Edit,
  };

  // What the editor works with, owned by the workflow for the dialog's lifetime.
  struct QuestEditorSetup
  {
    EditorMode mode = EditorMode::Create;
    std::uint32_t entry = 0;                    // the quest written (the new ID for create / copy)
    mysql::content::QuestEditorData const* data = nullptr; // schemas + stored content + typical XP
    Content::ContentLookups* lookups = nullptr;
    std::uint32_t default_npc = 0;              // pre-filled giver and ender of a new quest
    std::uint32_t follows = 0;                  // a new quest made to follow this one
    // Where the user's cursor is in the world (server coordinates), for "use my cursor position".
    std::function<std::optional<Content::WorldPosition>()> cursor_position;
    // Opens the item editor for a new item; returns its ID (0 when cancelled). The item is added to
    // lookups->items before it returns.
    std::function<std::uint32_t()> create_item;
    // Checks the whole quest before the dialog closes (e.g. the save plan's refusals); the dialog stays open
    // while it returns problems.
    std::function<QStringList(Noggit::Quest::QuestContent const&)> validate;
  };

  class QuestPage : public QWidget
  {
  public:
    QuestPage(QuestEditorSetup const& setup, QWidget* parent = nullptr);

    virtual QString title() const = 0;
    virtual QString subtitle() const = 0;
    virtual void load(Noggit::Quest::QuestContent const& content) = 0;
    virtual void store(Noggit::Quest::QuestContent& content) const = 0;
    // Why the quest cannot be saved yet (empty when fine).
    virtual QStringList problems() const { return {}; }

    // Called whenever the page's values change (the dialog refreshes its header and problems).
    std::function<void()> on_changed;

  protected:
    void changed() const;
    // Pages add their cards here.
    QVBoxLayout* cards() const { return _cards; }
    Noggit::Quest::QuestSchema const& schema() const { return _setup.data->db.quest; }
    Content::ContentLookups& lookups() const { return *_setup.lookups; }

    QuestEditorSetup const& _setup;

  private:
    QVBoxLayout* _cards = nullptr;
  };
}
