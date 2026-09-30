// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The item editor: new quest items ("Wolf Pelt") and changes to items made with it, with a live preview
// of the item's tooltip. Collects the choices only; ItemWorkflow writes them.

#include <mysql/content_db.h>
#include <noggit/ui/content/ContentLookups.hpp>

#include <QtWidgets/QDialog>

#include <cstdint>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui::Item
{
  struct ItemEditorSetup
  {
    bool editing = false;
    std::uint32_t entry = 0; // the item written (the new ID for a new item)
    mysql::content::ItemEditorData const* data = nullptr;
    Content::ContentLookups* lookups = nullptr;
    QString suggested_name; // pre-filled name of a new item
  };

  class ItemEditorDialog : public QDialog
  {
  public:
    ItemEditorDialog(ItemEditorSetup const& setup, QWidget* parent = nullptr);

    Noggit::Item::ItemFields fields() const;
    // True when the user asked to delete the item (edit mode, custom items) instead of saving.
    bool deleteRequested() const { return _delete_requested; }

  private:
    void refresh();
    std::uint32_t displayId() const;

    ItemEditorSetup const& _setup;
    QLabel* _icon = nullptr;
    QLabel* _preview_name = nullptr;
    QLabel* _preview_kind = nullptr;
    QLabel* _preview_description = nullptr;
    QLineEdit* _name = nullptr;
    QComboBox* _kind = nullptr;
    QComboBox* _quality = nullptr;
    Content::EntryPicker* _look = nullptr;
    QPlainTextEdit* _description = nullptr;
    QSpinBox* _stack = nullptr;
    QSpinBox* _gold = nullptr;
    QSpinBox* _silver = nullptr;
    QSpinBox* _copper = nullptr;
    QWidget* _price_row = nullptr;
    QLabel* _problem = nullptr;
    QPushButton* _ok = nullptr;
    bool _delete_requested = false;
  };
}
