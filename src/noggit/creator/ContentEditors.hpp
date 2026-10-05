#pragma once
#include "Services.hpp"
#include <optional>
class QWidget;
namespace Noggit::Creator {
// The Item Editor and Spell Editor. Each returns what was saved (nullopt: nothing), so the editor that opened
// it can use the new item or spell right away. Game items and spells open read-only, with Clone.

// Asks how to start: Clone Existing Item (the main way), or one of the item templates.
std::optional<Id> createItem(QWidget* parent);
std::optional<Id> cloneItem(QWidget* parent, Id source = 0); // 0: choose the item to clone
std::optional<Id> editItem(QWidget* parent, Id item);

std::optional<Id> createSpell(QWidget* parent); // Clone Existing Spell, or Create Spell from an effect template
std::optional<Id> cloneSpell(QWidget* parent, Id source = 0);
std::optional<Id> editSpell(QWidget* parent, Id spell);
// A searchable spell list with icons, ranks and New / Clone / Edit (for item spells, triggers, quest rewards).
std::optional<Id> pickSpell(QWidget* parent, QString const& title, Id current = 0);

struct SpellDesign;
// The spell's description as players read it, the client's $s1 / $d / $o1 tokens filled in (rich text).
QString spellTooltipText(SpellDesign const&);
// The searchable grid of the client's spell icons (SpellIcon.dbc IDs).
std::optional<Id> pickSpellIcon(QWidget* parent, Id current = 0);

// Browsers of every item / spell with New, Clone, Edit, Test and Delete.
void openItemLibrary(QWidget* parent);
void openSpellLibrary(QWidget* parent);

// The talent trees as the game's talent window, with every part editable, a level simulator and a build preview.
void openTalentEditor(QWidget* parent);

// Local test: the character gets the item (Test Item) or learns the spell and its earlier ranks (Test Spell).
void testItem(QWidget* parent, Id item);
void testSpell(QWidget* parent, Id spell);
}
