#include "NpcStudio.hpp"
#include <noggit/ui/FontAwesome.hpp>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
namespace Noggit::Creator {
namespace {
using Icon = Ui::FontAwesome::Icons;
QToolButton* tile(QString const& text, Icon icon, QString const& tooltip, std::function<void()> const& action, QWidget* parent) {
  auto button = new QToolButton(parent);
  button->setText(text); button->setToolTip(tooltip);
  button->setIcon(Ui::FontAwesomeIcon(icon)); button->setIconSize(QSize(14, 14));
  // Text beside a small icon: the bottom panel is short, and taller tiles clipped their labels.
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setMinimumSize(0, 28); button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  QObject::connect(button, &QToolButton::clicked, parent, [action] { if (action) action(); });
  return button;
}
QLabel* heading(QString const& text, QWidget* parent) {
  auto label = new QLabel(text.toUpper(), parent); label->setObjectName("StudioHeading"); return label;
}
QWidget* row(std::initializer_list<QToolButton*> buttons, QWidget* parent) {
  auto widget = new QWidget(parent); auto layout = new QHBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
  for (auto* button : buttons) layout->addWidget(button);
  return widget;
}
}
NpcStudio::NpcStudio(Actions actions, QWidget* parent) : QWidget(parent), _actions(std::move(actions)) {
  setMinimumWidth(330);
  setStyleSheet(
    "#StudioHeading { font-size: 9pt; font-weight: bold; letter-spacing: 1px; color: palette(mid); margin-top: 6px; }"
    "#StudioName { font-size: 15pt; font-weight: bold; }"
    "#StudioBadge { border-radius: 8px; padding: 2px 8px; font-size: 8pt; font-weight: bold; color: white; }"
    "QToolButton { border: 1px solid palette(mid); border-radius: 5px; padding: 3px 6px; }"
    "QToolButton:hover:enabled { background: palette(highlight); color: palette(highlighted-text); }");
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 6); layout->setSpacing(4);

  auto header = new QHBoxLayout;
  _name = new QLabel(this); _name->setObjectName("StudioName"); _name->setTextFormat(Qt::PlainText);
  _badge = new QLabel(this); _badge->setObjectName("StudioBadge");
  header->addWidget(_name, 1); header->addWidget(_badge, 0, Qt::AlignTop);
  layout->addLayout(header);
  _details = new QLabel(this); _details->setTextFormat(Qt::PlainText); _details->setWordWrap(true);
  layout->addWidget(_details);
  _hint = new QLabel(this); _hint->setWordWrap(true); _hint->setStyleSheet("color: palette(mid); font-style: italic;");
  layout->addWidget(_hint);

  layout->addWidget(heading("Create", this));
  _clone = tile("Clone", Icon::clone, "A new NPC that starts as a copy of the selected one. The original stays unchanged.", _actions.clone, this);
  layout->addWidget(row({tile("Humanoid", Icon::male, "Design a new humanoid NPC: race, face, hair and outfit, with a 3D preview.", _actions.newHumanoid, this),
                         tile("Creature", Icon::paw, "Design a new creature or monster from an existing model, with a 3D preview.", _actions.newCreature, this),
                         _clone}, this));

  layout->addWidget(heading("This NPC", this));
  _place = tile("Place", Icon::mapmarkeralt, "Click in the world to place this NPC there. Esc cancels.", _actions.place, this);
  _edit = tile("Edit", Icon::edit, "Change looks, combat and role, or delete it.", _actions.edit, this);
  _quests = tile("Quests", Icon::exclamation, "The quests this NPC gives or takes: make, change or copy them.", _actions.quests, this);
  _chain = tile("Chain", Icon::sitemap, "This NPC's quest chain as a diagram: link quests by drawing arrows.", _actions.chain, this);
  layout->addWidget(row({_place, _edit, _quests, _chain}, this));

  _placementRow = new QWidget(this);
  auto placement = new QVBoxLayout(_placementRow); placement->setContentsMargins(0, 0, 0, 0); placement->setSpacing(4);
  placement->addWidget(heading("Selected placement", _placementRow));
  placement->addWidget(row({tile("Duplicate", Icon::copy, "Click in the world to place another one, with the same settings.", _actions.duplicatePlacement, _placementRow),
                            tile("Remove", Icon::trash, "Remove this placement from the world (the NPC itself stays).", _actions.deletePlacement, _placementRow),
                            tile("Locate", Icon::crosshairs, "Move the camera to this placement.", _actions.locatePlacement, _placementRow)}, _placementRow));
  layout->addWidget(_placementRow);

  layout->addWidget(tile("Edit Patrol", Icon::edit, "Click terrain to draw a walk or run path.", _actions.patrol, this));
  layout->addWidget(heading("Test in game", this));
  _testNpc = tile("At this NPC", Icon::play, "Saves, restarts the local server and launches WoW standing in front of this NPC.", _actions.testAtNpc, this);
  layout->addWidget(row({_testNpc, tile("At a spot", Icon::locationarrow, "Click anywhere in the world to start testing there. Esc cancels.", _actions.testAtSpot, this)}, this));
  layout->addStretch();
  refresh();
}
void NpcStudio::setNpc(std::optional<Npc> npc) { _npc = std::move(npc); refresh(); }
void NpcStudio::setPlacement(std::optional<Id> guid) { _placement = guid; refresh(); }
void NpcStudio::refresh() {
  bool const chosen = _npc.has_value(), own = chosen && _npc->own;
  _name->setText(chosen ? _npc->name : "No NPC selected");
  _details->setText(chosen ? _npc->details : QString());
  _details->setVisible(chosen);
  _badge->setVisible(chosen);
  if (chosen) {
    _badge->setText(own ? "YOUR NPC" : "GAME NPC");
    _badge->setStyleSheet(own ? "background: #3d9a5b;" : "background: #6b7280;");
  }
  _hint->setText(!chosen ? "Pick an NPC in the list or click one in the world, or design a new one."
                 : own ? QString() : "Game NPCs stay as they are: clone it to make your own version you can change and place.");
  _hint->setVisible(!_hint->text().isEmpty());
  _clone->setEnabled(chosen);
  _place->setEnabled(own); _edit->setEnabled(own);
  _quests->setEnabled(chosen); _chain->setEnabled(chosen);
  _testNpc->setEnabled(chosen && _npc->placed);
  _testNpc->setToolTip(chosen && !_npc->placed ? "Place this NPC first, or select one of its placements."
                       : "Saves, restarts the local server and launches WoW standing in front of this NPC.");
  _placementRow->setVisible(_placement.has_value());
}
}
