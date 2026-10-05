// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// The lists the content editors let people pick from by name -- creatures, objects, items, spells,
// quests, factions, zones -- as shared, searchable models, plus the picker widget over them. Built once
// per editing session from plain lists (the database and client data are read elsewhere), so the
// editors themselves stay free of both.

#include <QtGui/QIcon>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Noggit::Ui::Content
{
  // Row 0 is "(none)" (entry 0). Each row keeps its entry in Qt::UserRole; with an icon source, rows
  // with a display id (UserRole + 1) get their icon on first use.
  class LookupModel : public QStandardItemModel
  {
  public:
    static constexpr int EntryRole = Qt::UserRole;
    static constexpr int DisplayRole = Qt::UserRole + 1;
    static constexpr int KindRole = Qt::UserRole + 2;

    explicit LookupModel(QString const& none_label = "(none)");

    void add(std::uint32_t entry, QString const& label, std::uint32_t display = 0, std::uint32_t kind = 0,
             QColor const& color = QColor());
    int rowOf(std::uint32_t entry) const; // -1 when absent
    QString labelOf(std::uint32_t entry) const;
    std::uint32_t kindOf(std::uint32_t entry) const;

    std::function<QIcon(std::uint32_t display)> icon_for;
    // A visual picker for this list (the Item Browser for items); empty when there is none.
    std::function<std::optional<std::uint32_t>(QWidget* parent, std::uint32_t current)> browse;

    QVariant data(QModelIndex const& index, int role) const override;

  private:
    std::map<std::uint32_t, int> _rows;
    mutable std::map<std::uint32_t, QIcon> _icons;
  };

  // Searchable picker: type any part of a name or an ID. entry() is 0 for "(none)" or unknown text.
  class EntryPicker : public QComboBox
  {
  public:
    EntryPicker(LookupModel* model, QWidget* parent = nullptr);
    void setEntry(std::uint32_t entry);
    std::uint32_t entry() const;
    LookupModel* lookup() const { return _model; }

  private:
    LookupModel* _model;
  };

  // The picker plus a browse button when its list has a visual picker (otherwise the picker itself).
  QWidget* browsable(EntryPicker* picker);

  struct NamedEntry
  {
    std::uint32_t entry = 0;
    std::string name;
    std::uint32_t kind = 0;    // items: quality; objects: gameobject type; creatures: npc flags
    std::uint32_t display = 0; // items: display id
  };

  struct AreaTriggerEntry
  {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
    std::uint32_t quest = 0;
    bool other_use = false;
  };

  // A point in the world in server coordinates (for "use my cursor position").
  struct WorldPosition
  {
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
  };

  struct LookupSource
  {
    std::vector<NamedEntry> creatures, objects, items, spells, quests; // quests: kind = level
    std::vector<NamedEntry> reputation_factions, zones;
    std::vector<NamedEntry> faction_templates; // what NPCs belong to, labelled with how players see them
    std::vector<AreaTriggerEntry> area_triggers;
    std::function<QIcon(std::uint32_t display)> item_icon; // may be empty
    bool vanilla = true;
  };

  // The models, owned together. Objects are also offered filtered: chests (type 3, can hold loot) and
  // usable objects (type 10, "goobers": levers, altars, ...).
  struct ContentLookups
  {
    std::unique_ptr<LookupModel> creatures, objects, chests, usable_objects, items, spells, quests;
    std::unique_ptr<LookupModel> reputation_factions, zones, faction_templates;
    std::vector<AreaTriggerEntry> area_triggers;
    bool vanilla = true;

    QString creatureName(std::uint32_t entry) const;
    QString objectName(std::uint32_t entry) const;
    QString itemName(std::uint32_t entry) const;
    QString questName(std::uint32_t entry) const;
    QString spellName(std::uint32_t entry) const;

    // Adds a newly created item / quest / NPC to its list.
    void addItem(std::uint32_t entry, QString const& name, std::uint32_t quality, std::uint32_t display);
    void addQuest(std::uint32_t entry, QString const& title, std::uint32_t level);
    void addCreature(std::uint32_t entry, QString const& name, std::uint32_t npc_flags);
    void addSpell(std::uint32_t entry, QString const& name);
  };

  std::unique_ptr<ContentLookups> buildLookups(LookupSource const& source);

  constexpr std::uint32_t CHEST_TYPE = 3;
  constexpr std::uint32_t USABLE_OBJECT_TYPE = 10;

  // Item quality colour (game colours; common is left to the theme's text colour).
  QColor qualityColor(std::uint32_t quality);
}
