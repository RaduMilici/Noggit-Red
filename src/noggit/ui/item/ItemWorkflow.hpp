// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Creating, editing and deleting items: the item editor, the confirmation, the write, the re-runnable copy
// in sql_exports/items/. MySQL builds only.

#include <cstdint>

#include <QtCore/QString>

class QWidget;

namespace Noggit::Ui::Content
{
  class ContentSession;
}

namespace Noggit::Ui::Item
{
  // A new item; returns its ID (0 when cancelled). Added to the session's item list.
  std::uint32_t createItem(Content::ContentSession& session, QWidget* parent, QString const& suggested_name = {});

  // Edits (or, if the user asks, deletes) an item made with the editor. True when something was written.
  bool editItem(Content::ContentSession& session, QWidget* parent, std::uint32_t item);

  // The list of items made with the editor, with New / Edit (and Delete inside the editor).
  void manageItems(Content::ContentSession& session, QWidget* parent);
}
