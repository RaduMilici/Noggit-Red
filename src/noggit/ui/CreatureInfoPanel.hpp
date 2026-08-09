// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <cstdint>
#include <optional>

namespace Noggit
{
  namespace Ui
  {
    // "Quick Facts" dropdown for the selected creature spawn (wowhead-style): creature_template
    // stats + the creature's spells and permanent auras as hoverable icons with spell tooltips.
    // Data comes from the server DB (creature_template / creature_spells / spell_template) plus
    // Faction/FactionTemplate/SpellIcon DBCs.
    class CreatureInfoPanel : public QFrame
    {
      Q_OBJECT

    public:
      explicit CreatureInfoPanel(QWidget* parent = nullptr);

      // Rebuild the panel for a creature entry (no-op if it's already showing that entry).
      void setCreature(std::uint32_t entry);
      void clearCreature();

    protected:
      // Shows/hides the custom translucent WoW-style tooltip for the spell/aura icons.
      bool eventFilter(QObject* watched, QEvent* event) override;

    private:
      void rebuild(std::uint32_t entry);
      void addFactRow(QVBoxLayout* layout, QString const& html);

      std::optional<std::uint32_t> _current_entry;
      QVBoxLayout* _layout = nullptr;
      QWidget* _content = nullptr;
    };
  }
}
