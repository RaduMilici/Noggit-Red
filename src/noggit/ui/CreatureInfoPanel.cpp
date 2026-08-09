// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/CreatureInfoPanel.hpp>

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/TextureManager.h>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>
#ifdef USE_MYSQL_UID_STORAGE
  #include <mysql/mysql.h>
#endif

#include <QtCore/QEvent>
#include <QtCore/QRegularExpression>
#include <QtGui/QCursor>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QTextDocument>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace
{
  QString rank_name(std::uint32_t rank)
  {
    switch (rank)
    {
      case 1: return "Elite";
      case 2: return "Rare Elite";
      case 3: return "Boss";
      case 4: return "Rare";
      default: return "Normal";
    }
  }

  QString school_name(std::uint32_t school)
  {
    switch (school)
    {
      case 0: return "Physical";
      case 1: return "Holy";
      case 2: return "Fire";
      case 3: return "Nature";
      case 4: return "Frost";
      case 5: return "Shadow";
      case 6: return "Arcane";
      default: return QString();
    }
  }

  QString range_text(std::uint32_t lo, std::uint32_t hi)
  {
    if (hi > lo)
    {
      return QString("%L1 - %L2").arg(lo).arg(hi);
    }
    return QString("%L1").arg(lo);
  }

  // gold_min/max are copper: render like the client money display.
  QString wealth_text(std::uint32_t copper_lo, std::uint32_t copper_hi)
  {
    auto const fmt = [](std::uint32_t copper)
    {
      QString out;
      std::uint32_t const g = copper / 10000;
      std::uint32_t const s = (copper % 10000) / 100;
      std::uint32_t const c = copper % 100;
      if (g) out += QString("<span style='color:#ffd100'>%1g</span> ").arg(g);
      if (s || g) out += QString("<span style='color:#c0c0c0'>%1s</span> ").arg(s);
      out += QString("<span style='color:#b87333'>%1c</span>").arg(c);
      return out;
    };
    if (copper_hi > copper_lo)
    {
      return fmt(copper_lo) + " - " + fmt(copper_hi);
    }
    return fmt(copper_lo);
  }

  // Alliance/Horde reaction letters from FactionTemplate friendly/hostile masks (mask 0x2 =
  // Alliance players, 0x4 = Horde players): red hostile, green friendly, yellow neutral.
  QString react_html(std::uint32_t faction_template_id)
  {
    std::uint32_t friendly_mask = 0;
    std::uint32_t hostile_mask = 0;
    try
    {
      auto row = gFactionTemplateDB.getByID(faction_template_id);
      friendly_mask = row.getUInt(FactionTemplateDB::FriendlyMask);
      hostile_mask = row.getUInt(FactionTemplateDB::HostileMask);
    }
    catch (DBCFile::NotFound const&)
    {
      return "? ?";
    }

    auto const letter = [&](std::uint32_t mask, QChar ch)
    {
      char const* color = (hostile_mask & mask) ? "#ff4040"
                        : (friendly_mask & mask) ? "#40ff40"
                        : "#ffd100";
      return QString("<span style='color:%1'>%2</span>").arg(color).arg(ch);
    };
    return letter(0x2, 'A') + " " + letter(0x4, 'H');
  }

  QString faction_name(std::uint32_t faction_template_id)
  {
    try
    {
      auto ft = gFactionTemplateDB.getByID(faction_template_id);
      auto fac = gFactionDB.getByID(ft.getUInt(FactionTemplateDB::Faction));
      auto const* name = fac.getLocalizedString(FactionDB::Name);
      if (name && *name)
      {
        return QString::fromUtf8(name);
      }
    }
    catch (DBCFile::NotFound const&)
    {
    }
    return QString();
  }

#ifdef USE_MYSQL_UID_STORAGE
  // Fill in spells the world DB could not supply, from the CLIENT's Spell.dbc (see lookupSpellDbcInfo).
  // Turtle/vmangos ship a spell_template table, but the 3.3.5a cores do not -- they read the DBC -- so
  // every aura there used to render as a bare "Spell ID N (no spell_template row)". Producing the same
  // record shape as the SQL path leaves the tooltip/icon/macro-resolver code below unchanged.
  void fill_missing_spell_infos_from_dbc(std::set<std::uint32_t> const& ids,
                                         std::map<std::uint32_t, mysql::SpellInfoRecord>& infos)
  {
    for (auto const id : ids)
    {
      if (!id || infos.count(id))
      {
        continue;
      }

      SpellDbcInfo dbc;
      if (!lookupSpellDbcInfo(id, dbc))
      {
        continue; // not in the client's DBC either -- the caller still shows the raw id
      }

      mysql::SpellInfoRecord info;
      info.entry = dbc.entry;
      info.name = dbc.name;
      info.description = dbc.description;
      info.icon_id = dbc.icon_id;
      info.spell_visual = dbc.spell_visual;
      info.mana_cost = dbc.mana_cost;
      info.power_type = dbc.power_type;
      info.range_index = dbc.range_index;
      info.casting_time_index = dbc.casting_time_index;
      info.duration_index = dbc.duration_index;
      info.proc_chance = dbc.proc_chance;
      info.proc_charges = dbc.proc_charges;
      info.stack_amount = dbc.stack_amount;

      for (size_t i = 0; i < 3; ++i)
      {
        info.effect_base_points[i] = dbc.effect_base_points[i];
        info.effect_die_sides[i] = dbc.effect_die_sides[i];
        info.effect_amplitude[i] = dbc.effect_amplitude[i];
        info.effect_chain_target[i] = dbc.effect_chain_target[i];
        info.effect_radius_index[i] = dbc.effect_radius_index[i];
        info.effect_multiple_value[i] = dbc.effect_multiple_value[i];
      }

      infos.emplace(id, info);
    }
  }

  // Collect every spell id referenced by cross-spell description macros ($17466s1 etc.) so the
  // caller can fetch those spells' data before resolving.
  void collect_referenced_spell_ids(std::string const& description, std::set<std::uint32_t>& out)
  {
    QRegularExpression re("\\$(\\d+)[a-zA-Z]");
    auto it = re.globalMatch(QString::fromStdString(description));
    while (it.hasNext())
    {
      out.insert(it.next().captured(1).toUInt());
    }
  }

  // General resolver for the vanilla spell-description macro grammar, matching the client:
  //   $[spellId]<letter>[index] -- spellId optional (defaults to this spell), index optional (1).
  //   s/S value (range if dieSides>1)   m min value          M max value
  //   o/O periodic total                t/T tick seconds     d/D duration
  //   a/A radius yards                  x/X chain targets    i max affected targets
  //   u stack amount                    n proc charges       h proc chance
  //   e effect multiple value           v max target level
  //   $lsingular:plural; picks by the preceding number; $gmale:female; picks male form.
  QString resolve_spell_description(mysql::SpellInfoRecord const& base_info,
                                    std::map<std::uint32_t, mysql::SpellInfoRecord> const& all_infos)
  {
    auto const duration_ms_of = [](mysql::SpellInfoRecord const& info) -> std::int32_t
    {
      try
      {
        if (info.duration_index)
        {
          return gSpellDurationDB.getByID(info.duration_index).getInt(SpellDurationDB::Duration);
        }
      }
      catch (DBCFile::NotFound const&)
      {
      }
      return 0;
    };

    auto const duration_text_of = [&](mysql::SpellInfoRecord const& info) -> QString
    {
      auto const ms = duration_ms_of(info);
      if (ms <= 0) return "until cancelled";
      if (ms % 60000 == 0 && ms >= 60000) return QString("%1 min").arg(ms / 60000);
      return QString("%1 sec").arg(ms / 1000.0, 0, 'g', 4);
    };

    auto const value_text = [](mysql::SpellInfoRecord const& info, int idx, char letter) -> QString
    {
      idx = std::clamp(idx, 1, 3) - 1;
      std::int32_t const base = info.effect_base_points[idx];
      std::int32_t const dice = info.effect_die_sides[idx];
      if (letter == 'm') return QString::number(std::abs(base + 1));            // min
      if (letter == 'M') return QString::number(std::abs(base + std::max(dice, 1))); // max
      if (dice > 1)
      {
        return QString("%1 to %2").arg(std::abs(base + 1)).arg(std::abs(base + dice));
      }
      return QString::number(std::abs(base + dice));
    };

    QString out = QString::fromStdString(base_info.description);

    // Pass 1: numeric/value macros. Iterate until no tokens remain (replacements never introduce '$').
    QRegularExpression token_re("\\$(\\d*)([a-zA-Z])(\\d?)");
    int guard = 0;
    for (auto match = token_re.match(out); match.hasMatch() && guard < 200; match = token_re.match(out), ++guard)
    {
      // $l / $g are grammar forms handled in pass 2 -- skip past them here.
      QChar const letter_qc = match.captured(2).at(0);
      char const letter = letter_qc.toLatin1();
      if (letter == 'l' || letter == 'L' || letter == 'g' || letter == 'G')
      {
        // temporarily mask so the loop can proceed; restored after the loop.
        out.replace(match.capturedStart(0), 1, QChar(0x01));
        continue;
      }

      mysql::SpellInfoRecord const* info = &base_info;
      if (!match.captured(1).isEmpty())
      {
        auto const ref_it = all_infos.find(match.captured(1).toUInt());
        if (ref_it != all_infos.end())
        {
          info = &ref_it->second;
        }
      }
      int const idx = match.captured(3).isEmpty() ? 1 : match.captured(3).toInt();
      int const eff = std::clamp(idx, 1, 3) - 1;

      QString replacement;
      switch (letter)
      {
        case 's': case 'S':
        case 'm': case 'M':
          replacement = value_text(*info, idx, letter);
          break;
        case 'o': case 'O':
        {
          std::int32_t const amplitude = info->effect_amplitude[eff];
          std::int32_t const dur = duration_ms_of(*info);
          std::int32_t const per_tick = std::abs(info->effect_base_points[eff] + std::max(info->effect_die_sides[eff], 1));
          replacement = QString::number((amplitude > 0 && dur > 0) ? per_tick * (dur / amplitude) : per_tick);
          break;
        }
        case 't': case 'T':
          replacement = QString::number(info->effect_amplitude[eff] / 1000);
          break;
        case 'd': case 'D':
          replacement = duration_text_of(*info);
          break;
        case 'a': case 'A':
        {
          float radius = 0.0f;
          try
          {
            if (info->effect_radius_index[eff])
            {
              radius = gSpellRadiusDB.getByID(info->effect_radius_index[eff]).getFloat(SpellRadiusDB::Radius);
            }
          }
          catch (DBCFile::NotFound const&)
          {
          }
          replacement = QString::number(static_cast<int>(radius));
          break;
        }
        case 'x': case 'X':
          replacement = QString::number(std::max(info->effect_chain_target[eff], 1));
          break;
        case 'i': case 'I':
          replacement = QString::number(info->max_affected_targets);
          break;
        case 'u': case 'U':
          replacement = QString::number(std::max<std::uint32_t>(info->stack_amount, 1));
          break;
        case 'n': case 'N':
          replacement = QString::number(info->proc_charges);
          break;
        case 'h': case 'H':
          replacement = QString::number(info->proc_chance);
          break;
        case 'e': case 'E':
          replacement = QString::number(info->effect_multiple_value[eff], 'g', 3);
          break;
        case 'v': case 'V':
          replacement = QString::number(info->max_target_level);
          break;
        default:
          // unknown macro: drop the '$' so it reads as plain text instead of looping forever
          replacement = match.captured(0).mid(1);
          break;
      }
      out.replace(match.capturedStart(0), match.capturedLength(0), replacement);
    }
    out.replace(QChar(0x01), "$"); // restore masked $l/$g markers

    // Pass 2: plural forms -- "N $lsecond:seconds;" picks by the nearest preceding number.
    QRegularExpression plural_re("\\$[lL]([^:;$]*):([^;$]*);");
    for (auto match = plural_re.match(out); match.hasMatch(); match = plural_re.match(out))
    {
      QString const before = out.left(match.capturedStart(0));
      QRegularExpression last_num_re("(\\d+)(?!.*\\d)");
      auto const num_match = last_num_re.match(before);
      bool const plural = !num_match.hasMatch() || num_match.captured(1).toInt() != 1;
      out.replace(match.capturedStart(0), match.capturedLength(0),
                  plural ? match.captured(2) : match.captured(1));
    }

    // Gender forms: creatures get the first (male) form.
    QRegularExpression gender_re("\\$[gG]([^:;$]*):([^;$]*);");
    for (auto match = gender_re.match(out); match.hasMatch(); match = gender_re.match(out))
    {
      out.replace(match.capturedStart(0), match.capturedLength(0), match.captured(1));
    }

    return out;
  }
#endif

  // Load the client's tooltip font (Fonts\FRIZQT__.TTF, straight from the MPQs) once and return its
  // family name; empty string when unavailable (falls back to the app font).
  QString wow_tooltip_font_family()
  {
    static QString family = []() -> QString
    {
      try
      {
        BlizzardArchive::ClientFile f("Fonts\\FRIZQT__.TTF",
                                      Noggit::Application::NoggitApplication::instance()->clientData());
        QByteArray const data(f.getBuffer(), static_cast<int>(f.getSize()));
        int const id = QFontDatabase::addApplicationFontFromData(data);
        auto const families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
        {
          return families.front();
        }
      }
      catch (std::exception const& e)
      {
        LogError << "Failed to load FRIZQT__.TTF for spell tooltips: " << e.what() << std::endl;
      }
      return QString();
    }();
    return family;
  }

  // Custom Blizzard-style tooltip popup: Qt's native QToolTip can't do real per-pixel translucency
  // on Windows, so this is a frameless always-on-top widget with WA_TranslucentBackground that
  // paints the client's tooltip frame itself -- 70%-opaque dark navy, grey border, rounded corners.
  class WowTooltip : public QWidget
  {
  public:
    static WowTooltip& instance()
    {
      static WowTooltip* tip = new WowTooltip();
      return *tip;
    }

    void showTip(QPoint const& global_pos, QString const& html)
    {
      // Size like the client's GameTooltip: natural width for short content, wrapping long lines at
      // the max tooltip width (scaled with the 15px body font).
      _doc.setHtml(html);
      _doc.setTextWidth(-1);
      qreal const natural = _doc.idealWidth();
      qreal const width = std::min<qreal>(natural, 380.0);
      _label->setFixedWidth(static_cast<int>(std::ceil(width)));
      _label->setText(html);
      _label->adjustSize();
      adjustSize();
      move(global_pos + QPoint(14, 18));
      show();
      raise();
    }

    void hideTip()
    {
      hide();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing);
      QRectF const r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
      QPainterPath path;
      path.addRoundedRect(r, 6.0, 6.0);
      painter.fillPath(path, QColor(6, 8, 15, 179)); // 70% opacity
      painter.setPen(QPen(QColor(0x8a, 0x8a, 0x99), 1.0));
      painter.drawPath(path);
    }

  private:
    WowTooltip()
      : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
      setAttribute(Qt::WA_TranslucentBackground);
      setAttribute(Qt::WA_ShowWithoutActivating);
      setAttribute(Qt::WA_TransparentForMouseEvents);

      auto* layout = new QVBoxLayout(this);
      layout->setContentsMargins(6, 7, 6, 7);
      _label = new QLabel(this);
      _label->setTextFormat(Qt::RichText);
      _label->setWordWrap(true);

      // IMPORTANT: the font must live in the STYLESHEET -- Qt ignores QWidget::setFont once a
      // stylesheet is set on the widget, which is why earlier pixel-size changes had no effect.
      auto const family = wow_tooltip_font_family();
      _label->setStyleSheet(QString("color: #ffffff; background: transparent;%1 font-size: 15px;")
                              .arg(family.isEmpty() ? QString()
                                                    : QString(" font-family: '%1';").arg(family)));

      QFont font = _label->font();
      if (!family.isEmpty())
      {
        font.setFamily(family);
      }
      font.setPixelSize(15);
      _doc.setDefaultFont(font);

      layout->addWidget(_label);
    }

    QLabel* _label = nullptr;
    QTextDocument _doc; // scratch for measuring the content's natural width
  };

  QString spell_icon_path(std::uint32_t icon_id)
  {
    try
    {
      auto row = gSpellIconDB.getByID(icon_id);
      std::string path = row.getString(SpellIconDB::TextureFilename);
      if (!path.empty())
      {
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch)
        {
          return ch == '\\' ? '/' : static_cast<char>(std::tolower(ch));
        });
        return QString::fromStdString(path + ".blp");
      }
    }
    catch (DBCFile::NotFound const&)
    {
    }
    return QString();
  }
}

namespace Noggit
{
  namespace Ui
  {
    CreatureInfoPanel::CreatureInfoPanel(QWidget* parent)
      : QFrame(parent)
    {
      // Plain noggit-style panel: default app palette, standard frame -- like the coordinate editor.
      setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
      setAutoFillBackground(true);
      setMinimumWidth(240);
      setMaximumWidth(320);

      auto outer = new QVBoxLayout(this);
      outer->setContentsMargins(8, 6, 8, 8);
      outer->setSpacing(3);

      // (No inner title label -- the panel is shown as a floating tool window whose title bar
      // already says "Quick Facts".)
      _content = new QWidget(this);
      _layout = new QVBoxLayout(_content);
      _layout->setContentsMargins(0, 2, 0, 0);
      _layout->setSpacing(2);
      outer->addWidget(_content);

      clearCreature();
    }

    bool CreatureInfoPanel::eventFilter(QObject* watched, QEvent* event)
    {
      if (event->type() == QEvent::Enter)
      {
        if (auto* widget = qobject_cast<QWidget*>(watched))
        {
          auto const tip = widget->property("wowtip").toString();
          if (!tip.isEmpty())
          {
            WowTooltip::instance().showTip(QCursor::pos(), tip);
          }
        }
      }
      else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide)
      {
        WowTooltip::instance().hideTip();
      }
      return QFrame::eventFilter(watched, event);
    }

    void CreatureInfoPanel::setCreature(std::uint32_t entry)
    {
      // Only skip the rebuild when this entry is already SUCCESSFULLY shown -- a failed/incomplete
      // fetch must retry, otherwise a hiccup leaves the panel permanently empty for that creature.
      if (_current_entry && *_current_entry == entry)
      {
        return;
      }
      rebuild(entry);
    }

    void CreatureInfoPanel::clearCreature()
    {
      _current_entry.reset();
      QLayoutItem* item;
      while ((item = _layout->takeAt(0)) != nullptr)
      {
        delete item->widget();
        delete item;
      }
      addFactRow(_layout, "No creature selected");
      adjustSize();
    }

    void CreatureInfoPanel::addFactRow(QVBoxLayout* layout, QString const& html)
    {
      auto* label = new QLabel(_content);
      label->setTextFormat(Qt::RichText);
      label->setText(html);
      label->setWordWrap(true);
      layout->addWidget(label);
    }

    void CreatureInfoPanel::rebuild(std::uint32_t entry)
    {
      _current_entry.reset();

      QLayoutItem* item;
      while ((item = _layout->takeAt(0)) != nullptr)
      {
        delete item->widget();
        delete item;
      }

#ifndef USE_MYSQL_UID_STORAGE
      addFactRow(_layout, "Database unavailable");
#else
      std::string error;
      auto const d = mysql::getCreatureTemplateDetails(entry, &error);
      if (!d.ok)
      {
        addFactRow(_layout, QString("No template data%1")
                              .arg(error.empty() ? QString() : QString(" (%1)").arg(QString::fromStdString(error))));
        adjustSize();
        return;
      }

      // Fetch succeeded: remember the entry so repeated selection updates don't re-query.
      _current_entry = entry;

      addFactRow(_layout, QString("Level: <b>%1</b>").arg(range_text(d.level_min, d.level_max)));
      addFactRow(_layout, QString("Class: <b>%1</b>").arg(rank_name(d.rank)));
      addFactRow(_layout, QString("React: <b>%1</b>").arg(react_html(d.faction)));
      auto const fac_name = faction_name(d.faction);
      if (!fac_name.isEmpty())
      {
        addFactRow(_layout, QString("Faction: %1").arg(fac_name.toHtmlEscaped()));
      }
      addFactRow(_layout, QString("Faction ID: <b>%1</b>").arg(d.faction));
      addFactRow(_layout, QString("Health: <b>%1</b>").arg(range_text(d.health_min, d.health_max)));
      if (d.mana_max)
      {
        addFactRow(_layout, QString("Mana: <b>%1</b>").arg(range_text(d.mana_min, d.mana_max)));
      }
      if (d.gold_max)
      {
        addFactRow(_layout, QString("Wealth: %1").arg(wealth_text(d.gold_min, d.gold_max)));
      }
      addFactRow(_layout, QString("Damage: <b>%1 - %2</b>")
                            .arg(static_cast<int>(d.dmg_min)).arg(static_cast<int>(d.dmg_max)));
      addFactRow(_layout, QString("Armor: <b>%L1</b>").arg(d.armor));
      if (d.fire_res)   addFactRow(_layout, QString("Fire Resistance: <b>%1</b>").arg(d.fire_res));
      if (d.nature_res) addFactRow(_layout, QString("Nature Resistance: <b>%1</b>").arg(d.nature_res));
      if (d.frost_res)  addFactRow(_layout, QString("Frost Resistance: <b>%1</b>").arg(d.frost_res));
      if (d.shadow_res) addFactRow(_layout, QString("Shadow Resistance: <b>%1</b>").arg(d.shadow_res));
      if (d.arcane_res) addFactRow(_layout, QString("Arcane Resistance: <b>%1</b>").arg(d.arcane_res));
      if (d.holy_res)   addFactRow(_layout, QString("Holy Resistance: <b>%1</b>").arg(d.holy_res));
      addFactRow(_layout, QString("Display ID: <b>%1</b>").arg(d.display_id));
      if (d.equipment_id)
      {
        addFactRow(_layout, QString("Equipment ID: <b>%1</b>").arg(d.equipment_id));
      }
      addFactRow(_layout, QString("NPC flags: <b>%1</b>").arg(d.npc_flags));

      // Spell + aura icon rows with wow-style tooltips (name gold, school right, description below).
      std::set<std::uint32_t> all_ids(d.spells.begin(), d.spells.end());
      all_ids.insert(d.auras.begin(), d.auras.end());
      std::map<std::uint32_t, mysql::SpellInfoRecord> infos;
      if (!all_ids.empty())
      {
        infos = mysql::getSpellInfos(all_ids);
        // Schemas without spell_template (the 3.3.5a cores) return nothing -- fall back to the client DBC.
        fill_missing_spell_infos_from_dbc(all_ids, infos);

        // Descriptions may reference OTHER spells' values ($17466s1 etc.) -- fetch those too so the
        // resolver has their data.
        std::set<std::uint32_t> referenced;
        for (auto const& [id, info] : infos)
        {
          collect_referenced_spell_ids(info.description, referenced);
        }
        for (auto const& [id, info] : infos)
        {
          referenced.erase(id);
        }
        if (!referenced.empty())
        {
          auto ref_infos = mysql::getSpellInfos(referenced);
          infos.insert(ref_infos.begin(), ref_infos.end());
          fill_missing_spell_infos_from_dbc(referenced, infos);
        }
      }

      int constexpr icon_size = 36;

      auto const add_icon_row = [&](QString const& heading, std::vector<std::uint32_t> const& ids)
      {
        auto* heading_label = new QLabel(QString("<b>%1</b>").arg(heading), _content);
        _layout->addSpacing(4);
        _layout->addWidget(heading_label);

        if (ids.empty())
        {
          return;
        }

        auto* row_widget = new QWidget(_content);
        auto* row = new QHBoxLayout(row_widget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        for (auto const spell_id : ids)
        {
          auto* icon = new QLabel(row_widget);
          icon->setFixedSize(icon_size, icon_size);
          icon->setAlignment(Qt::AlignCenter);

          auto const info_it = infos.find(spell_id);
          QString tooltip;
          if (info_it != infos.end())
          {
            auto const& info = info_it->second;
            auto const icon_path = spell_icon_path(info.icon_id);
            if (!icon_path.isEmpty())
            {
              // Render at the icon's native 64x64 then scale down with the aspect LOCKED -- letting
              // the label stretch the content (setScaledContents) rendered them as rectangles.
              if (QPixmap* pm = BLPRenderer::getInstance().render_blp_to_pixmap(icon_path.toStdString(), 64, 64))
              {
                icon->setPixmap(pm->scaled(icon_size, icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
              }
            }
            // Blizzard tooltip layout: white name; cost left / range right; cast time; YELLOW
            // description; SpellID row. Values from spell_template + SpellRange/SpellCastTimes.dbc.
            QString cost_text;
            if (info.mana_cost)
            {
              switch (info.power_type)
              {
                case 1:  cost_text = QString("%1 Rage").arg(info.mana_cost / 10); break;
                case 2:  cost_text = QString("%1 Focus").arg(info.mana_cost); break;
                case 3:  cost_text = QString("%1 Energy").arg(info.mana_cost); break;
                default: cost_text = QString("%1 Mana").arg(info.mana_cost); break;
              }
            }

            QString range_text_str;
            try
            {
              if (info.range_index)
              {
                float const max_range = gSpellRangeDB.getByID(info.range_index).getFloat(SpellRangeDB::MaxRange);
                if (max_range > 0.0f)
                {
                  range_text_str = QString("%1 yd range").arg(static_cast<int>(max_range));
                }
              }
            }
            catch (DBCFile::NotFound const&)
            {
            }

            QString cast_text = "Instant cast";
            try
            {
              if (info.casting_time_index)
              {
                int const cast_ms = gSpellCastTimesDB.getByID(info.casting_time_index).getInt(SpellCastTimesDB::CastTime);
                if (cast_ms > 0)
                {
                  cast_text = QString("%1 sec cast").arg(cast_ms / 1000.0, 0, 'g', 3);
                }
              }
            }
            catch (DBCFile::NotFound const&)
            {
            }

            // Client GameTooltip layout: 15px white header, cost left / range right, cast time,
            // yellow wrapping description, SpellID row.
            QString const name_html =
              QString("<div style='font-size:18px; color:#ffffff;'>%1</div>")
                .arg(QString::fromStdString(info.name).toHtmlEscaped());

            QString cost_range_html;
            if (!cost_text.isEmpty() || !range_text_str.isEmpty())
            {
              cost_range_html =
                QString("<table width='100%' cellspacing='0' cellpadding='0'><tr>"
                        "<td style='color:#ffffff;'>%1</td>"
                        "<td align='right' style='color:#ffffff;'>%2</td></tr></table>")
                  .arg(cost_text).arg(range_text_str);
            }

            QString const cast_html =
              QString("<div style='color:#ffffff;'>%1</div>").arg(cast_text);

            QString const desc_html =
              QString("<div style='color:#ffd100;'>%1</div>")
                .arg(resolve_spell_description(info, infos).toHtmlEscaped());

            QString const id_html =
              QString("<table width='100%' cellspacing='0' cellpadding='0'><tr>"
                      "<td style='color:#ffffff;'>SpellID:</td>"
                      "<td align='right' style='color:#ffffff;'>%1</td></tr></table>")
                .arg(spell_id);

            tooltip = name_html + cost_range_html + cast_html + desc_html + id_html;
          }
          else
          {
            icon->setText(QString::number(spell_id));
            tooltip = QString("Spell ID %1 (no spell_template row)").arg(spell_id);
          }
          // Custom translucent WoW tooltip via hover events (native QToolTip can't be see-through).
          icon->setProperty("wowtip", tooltip);
          icon->installEventFilter(this);
          row->addWidget(icon);
        }
        row->addStretch(1);
        _layout->addWidget(row_widget);
      };

      add_icon_row("Spells:", d.spells);
      add_icon_row("Auras:", d.auras);
#endif

      adjustSize();
    }
  }
}
