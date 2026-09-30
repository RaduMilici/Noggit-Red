// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The NPC editor: a new NPC as a copy of an existing one, or changes to one made with it -- name, looks
// (with a live 3D preview), faction, what players can do with it, and what it says. Collects the choices
// only; NpcWorkflow writes them.

#include <mysql/content_db.h>
#include <noggit/ui/content/ContentLookups.hpp>

#include <QtWidgets/QDialog>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui::Npc
{
  struct NpcEditorSetup
  {
    bool editing = false;
    std::uint32_t source = 0; // the NPC copied / edited
    std::uint32_t entry = 0;  // the NPC written (the new ID for a copy)
    mysql::content::NpcEditorData const* data = nullptr;
    Content::ContentLookups* lookups = nullptr;
    std::function<QString(std::uint32_t display)> model_name; // "" when the client has no such model
    QWidget* preview = nullptr;                                // shown beside the form, re-parented
    std::function<void(std::uint32_t display, float scale)> show_look;
  };

  class NpcEditorDialog : public QDialog
  {
  public:
    NpcEditorDialog(NpcEditorSetup const& setup, QWidget* parent = nullptr);

    Noggit::Npc::NpcContent content() const;

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void lookChanged();
    void refresh();
    std::uint32_t selectedNpcFlags() const;

    NpcEditorSetup const& _setup;
    QLineEdit* _name = nullptr;
    QLineEdit* _subname = nullptr;
    QSpinBox* _level_min = nullptr;
    QSpinBox* _level_max = nullptr;
    QComboBox* _rank = nullptr;
    QSpinBox* _display = nullptr;
    QLabel* _display_info = nullptr;
    QDoubleSpinBox* _scale = nullptr;
    Content::EntryPicker* _faction = nullptr;
    std::map<std::uint32_t, QCheckBox*> _roles; // flag -> checkbox
    std::uint32_t _unmapped_npc_flags = 0;      // bits without a checkbox, kept as they are
    QPlainTextEdit* _greeting = nullptr;
    QPlainTextEdit* _quest_greeting = nullptr;
    std::map<std::string, QCheckBox*> _copy_related;
    QCheckBox* _keep_script = nullptr;
    QLabel* _header = nullptr;
    QLabel* _problem = nullptr;
    QPushButton* _ok = nullptr;
    bool _shown_once = false;
  };
}
