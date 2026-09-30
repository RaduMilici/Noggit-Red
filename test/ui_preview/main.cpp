// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Renders the quest editor's pages to PNG files with sample content (see CMakeLists.txt).

#include "VmangosSchema.hpp"

#include <noggit/ui/content/ContentLookups.hpp>
#include <noggit/ui/item/ItemEditorDialog.hpp>
#include <noggit/ui/quest/QuestEditorDialog.hpp>

#include <QtCore/QDir>

#include <cstdio>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

using namespace Noggit::Ui;
namespace Q = Noggit::Quest;

namespace
{
  // Close to Noggit's dark theme.
  void darkPalette(QApplication& app)
  {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(43, 45, 49));
    p.setColor(QPalette::WindowText, QColor(220, 221, 222));
    p.setColor(QPalette::Base, QColor(32, 34, 37));
    p.setColor(QPalette::AlternateBase, QColor(47, 49, 54));
    p.setColor(QPalette::Text, QColor(220, 221, 222));
    p.setColor(QPalette::Button, QColor(54, 57, 63));
    p.setColor(QPalette::ButtonText, QColor(220, 221, 222));
    p.setColor(QPalette::Highlight, QColor(88, 101, 242));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Mid, QColor(80, 82, 88));
    app.setPalette(p);
  }

  Content::LookupSource sampleLists()
  {
    Content::LookupSource source;
    source.creatures = {{197, "Marshal McBride"}, {299, "Young Wolf"}, {1423, "Stormwind Guard"}, {6, "Kobold Vermin"},
                        {90005, "Mira Stonehand"}};
    source.objects = {{32, "Sunken Chest", 3}, {1721, "Locked ball and chain", 10}, {180000, "Wanted Poster", 2}};
    source.items = {{750, "Tough Wolf Meat", 1, 1116}, {1309, "Oslow's Toolbox", 1}, {1468, "Murloc Fin", 1},
                    {2589, "Linen Cloth", 1}, {2075, "Priest's Mace", 2}, {6953, "Verigan's Fist", 3}, {90100, "Wolf Pelt", 1}};
    source.spells = {{133, "Fireball"}, {3366, "Opening"}, {6245, "Force Target Salute"}};
    source.quests = {{7, "Kobold Camp Cleanup", 2}, {15, "Investigate Echo Ridge", 3}, {90010, "Into the Den", 5},
                     {90011, "Part One", 5}, {90012, "Part Two", 5}};
    source.reputation_factions = {{72, "Stormwind"}, {47, "Ironforge"}, {69, "Darnassus"}, {54, "Gnomeregan Exiles"}};
    source.zones = {{12, "Elwynn Forest"}, {40, "Westfall"}, {9, "Northshire Valley"}};
    source.area_triggers = {{171, "Loch Modan, The Loch - Quest Resupplying the Excavation", 0, -5500, -3000, 330, 15},
                            {2946, "Stonetalon Mountains - Quest Boulderslide Ravine", 1, 960, 180, 20, 45, 6421},
                            {1667, "Dustwallow Marsh, Sentry Point", 1, -3100, -3200, 30, 10}};
    return source;
  }

  Q::QuestContent sampleQuest()
  {
    Q::QuestContent content;
    auto& f = content.fields;
    f.title = "Into the Den";
    f.details = "The wolves have grown bold, $N. Their den lies past the old mill -- thin them out, and bring me proof.";
    f.objectives = "Kill 6 Young Wolves, free the prisoner and bring 4 Tough Wolf Meat to Marshal McBride.";
    f.request_items = "Back already, $N?";
    f.offer_reward = "Well done. Northshire sleeps easier tonight.";
    f.level = 5;
    f.min_level = 3;
    f.zone = 9;
    f.type = 1;
    f.suggested_players = 3;
    f.time_limit = 1800;
    f.quest_flags = Q::FLAG_SHARABLE;
    f.special_flags = Q::SPECIAL_REPEATABLE;
    f.classes = 1 | 8;
    f.skill = 185;
    f.skill_value = 50;
    f.min_rep = Q::Reputation{72, 3000};
    f.xp = 450;
    f.money = 125;
    f.reward_spell = 133;
    f.targets = std::vector<Q::Target>{{Q::Target::Kind::Creature, 299, 6, "", 0},
                                       {Q::Target::Kind::Object, 1721, 1, "Prisoner freed", 0}};
    f.collect = std::vector<Q::ItemCount>{{750, 4}, {1309, 1}};
    f.rewards = std::vector<Q::ItemCount>{{2589, 5}};
    f.choices = std::vector<Q::ItemCount>{{2075, 1}, {6953, 1}};
    f.rep_rewards = std::vector<Q::Reputation>{{72, 250}};
    f.source_item = Q::ItemCount{1468, 1};
    content.links.starters = {{Q::Giver::Kind::Npc, 197}, {Q::Giver::Kind::Object, 180000}};
    content.links.enders = {{Q::Giver::Kind::Npc, 197}};
    content.links.area_trigger = 171;
    content.drops = {{Q::QuestDrop::Source::Creature, 299, 750, 40.0f, 299}, {Q::QuestDrop::Source::Object, 32, 1309, 100.0f, 1683}};
    content.prerequisites = {{90011, 90012}, Q::Prerequisites::Mode::All};
    content.offers_next = 15;
    Q::ScriptAction say, emote, spawn, complete;
    say.text = "Listen closely, $N.";
    emote.kind = Q::ScriptAction::Kind::Emote;
    emote.wait = 3;
    emote.id = 25;
    spawn.kind = Q::ScriptAction::Kind::SummonCreature;
    spawn.wait = 2;
    spawn.id = 299;
    spawn.count = 120;
    spawn.x = -8913.2f;
    spawn.y = -137.5f;
    spawn.z = 81.9f;
    complete.kind = Q::ScriptAction::Kind::CompleteQuest;
    complete.wait = 4;
    Q::ScriptAction thanks;
    thanks.kind = Q::ScriptAction::Kind::Yell;
    thanks.text = "Northshire thanks you, $N!";
    content.on_accept = {say, emote, spawn, complete};
    content.on_complete = {thanks};
    return content;
  }
}

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  darkPalette(app);
  QDir const out(argc > 1 ? argv[1] : ".");

  auto lookups = Content::buildLookups(sampleLists());
  mysql::content::QuestEditorData data;
  data.db = Q::detectQuestDatabase(vmangosColumns);
  data.content = sampleQuest();
  data.xp_by_level = {{5, {400, 450, 500}}};

  Quest::QuestEditorSetup setup;
  setup.mode = Quest::EditorMode::Edit;
  setup.entry = 90010;
  setup.data = &data;
  setup.lookups = lookups.get();
  setup.cursor_position = [] { return std::optional<Content::WorldPosition>(Content::WorldPosition{0, -5480, -2990, 331, 1.5f}); };
  setup.create_item = [] { return 0u; };

  Quest::QuestEditorDialog dialog(setup);
  dialog.show();

  // Round trip: opening the sample and handing it back unchanged must lose nothing.
  int mismatches = 0;
  {
    auto const back = dialog.content();
    auto const& a = data.content;
    auto const check = [&](char const* what, bool same)
    {
      if (!same)
      {
        std::fprintf(stderr, "round trip changed: %s\n", what);
        ++mismatches;
      }
    };
    auto const& fa = a.fields;
    auto const& fb = back.fields;
    check("title", fa.title == fb.title);
    check("details", fa.details == fb.details);
    check("objectives", fa.objectives == fb.objectives);
    check("request_items", fa.request_items == fb.request_items);
    check("offer_reward", fa.offer_reward == fb.offer_reward);
    check("level", fa.level == fb.level);
    check("min_level", fa.min_level == fb.min_level);
    check("zone", fa.zone == fb.zone);
    check("type", fa.type == fb.type);
    check("suggested_players", fa.suggested_players == fb.suggested_players);
    check("time_limit", fa.time_limit == fb.time_limit);
    check("quest_flags", fa.quest_flags == fb.quest_flags);
    check("special_flags", fa.special_flags == fb.special_flags);
    check("classes", fa.classes == fb.classes);
    check("skill", fa.skill == fb.skill && fa.skill_value == fb.skill_value);
    check("min_rep", fa.min_rep->faction == fb.min_rep->faction && fa.min_rep->value == fb.min_rep->value);
    check("xp", fa.xp == fb.xp);
    check("money", fa.money == fb.money);
    check("reward_spell", fa.reward_spell == fb.reward_spell);
    check("source_item", fa.source_item->id == fb.source_item->id && fa.source_item->count == fb.source_item->count);
    auto const same_items = [](auto const& x, auto const& y)
    {
      if (x->size() != y->size()) return false;
      for (std::size_t i = 0; i < x->size(); ++i)
      {
        if ((*x)[i].id != (*y)[i].id || (*x)[i].count != (*y)[i].count) return false;
      }
      return true;
    };
    check("collect", same_items(fa.collect, fb.collect));
    check("rewards", same_items(fa.rewards, fb.rewards));
    check("choices", same_items(fa.choices, fb.choices));
    check("targets", fa.targets->size() == fb.targets->size()
                     && (*fa.targets)[1].kind == (*fb.targets)[1].kind && (*fa.targets)[1].text == (*fb.targets)[1].text);
    check("rep_rewards", fa.rep_rewards->size() == fb.rep_rewards->size()
                         && (*fa.rep_rewards)[0].value == (*fb.rep_rewards)[0].value);
    check("starters", a.links.starters == back.links.starters);
    check("enders", a.links.enders == back.links.enders);
    check("area_trigger", a.links.area_trigger == back.links.area_trigger);
    check("drops", a.drops.size() == back.drops.size() && a.drops[1].source_entry == back.drops[1].source_entry
                   && a.drops[0].chance == back.drops[0].chance);
    check("prerequisites", a.prerequisites.quests == back.prerequisites.quests && a.prerequisites.mode == back.prerequisites.mode);
    check("offers_next", a.offers_next == back.offers_next);
    check("on_accept", a.on_accept == back.on_accept);
    check("on_complete", a.on_complete == back.on_complete);
  }
  for (int page = 0; page < 6; ++page)
  {
    dialog.showPage(page);
    app.processEvents();
    dialog.grab().save(out.filePath(QString("quest_page_%1.png").arg(page)));
  }

  mysql::content::QuestEditorData empty;
  empty.db = data.db;
  Quest::QuestEditorSetup create = setup;
  create.mode = Quest::EditorMode::Create;
  create.entry = 90014;
  create.data = &empty;
  create.default_npc = 197;
  Quest::QuestEditorDialog fresh(create);
  fresh.show();
  for (int page : {0, 2, 4})
  {
    fresh.showPage(page);
    app.processEvents();
    fresh.grab().save(out.filePath(QString("new_quest_page_%1.png").arg(page)));
  }

  // Item editor: a new quest item, icons drawn as coloured squares (no client data here).
  lookups->items->icon_for = [](std::uint32_t display)
  {
    QPixmap pixmap(40, 40);
    pixmap.fill(QColor::fromHsv(static_cast<int>(display * 47 % 360), 120, 170));
    return QIcon(pixmap);
  };
  mysql::content::ItemEditorData item;
  item.schema = Noggit::Item::detectItemSchema(vmangosColumns("item_template"));
  item.next_entry = 90101;
  Item::ItemEditorSetup item_setup;
  item_setup.entry = 90101;
  item_setup.data = &item;
  item_setup.lookups = lookups.get();
  item_setup.suggested_name = "Wolf Pelt";
  Item::ItemEditorDialog item_dialog(item_setup);
  item_dialog.show();
  app.processEvents();
  item_dialog.grab().save(out.filePath("item_new.png"));
  std::fprintf(stderr, "round trip: %d mismatch(es)\n", mismatches);
  return mismatches ? 1 : 0;
}
