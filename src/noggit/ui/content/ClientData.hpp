// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// What the content editors take from the game client (DBC files): zone names, factions, item icons, model
// names. The only editor code that reads client data, so the editors themselves can be previewed without
// it (test/ui_preview).

#include <noggit/ui/content/ContentLookups.hpp>

#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtCore/QString>

#include <cstdint>
#include <vector>

namespace Noggit::Ui::Content::ClientData
{
  // True for 1.12 clients (vmangos / Turtle): NPC flags, races and classes differ from 3.3.5a.
  bool vanillaClient();

  // Top-level zones (quest log headings).
  std::vector<NamedEntry> zones();
  // Factions players gain reputation with (Faction.dbc IDs).
  std::vector<NamedEntry> reputationFactions();
  // Faction templates NPCs belong to, labelled with how players see them ("Stormwind -- friendly to Alliance,
  // hostile to Horde").
  std::vector<NamedEntry> factionTemplates();

  // The inventory icon of an item display ID (empty icon when unknown).
  QIcon itemIcon(std::uint32_t display);
  // A spell's icon (SpellIcon.dbc ID; empty icon when unknown).
  QIcon spellIcon(std::uint32_t icon);
  // A client texture ("Interface\\TalentFrame\\WarriorArms-TopLeft.blp") at this size; null when the client lacks it.
  QPixmap texture(QString const& path, int width, int height);
  // The model file name of a creature display ID ("" when the client has no such display).
  QString creatureModelName(std::uint32_t display);
}
