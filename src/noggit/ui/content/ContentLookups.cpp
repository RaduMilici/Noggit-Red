// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/content/ContentLookups.hpp>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QHBoxLayout>

#include <QtCore/QRegularExpression>
#include <QtWidgets/QCompleter>

namespace Noggit::Ui::Content
{
  QColor qualityColor(std::uint32_t quality)
  {
    switch (quality)
    {
      case 0: return QColor(157, 157, 157);
      case 2: return QColor(30, 200, 0);
      case 3: return QColor(0, 112, 221);
      case 4: return QColor(163, 53, 238);
      case 5: return QColor(255, 128, 0);
      default: return QColor();
    }
  }

  LookupModel::LookupModel(QString const& none_label)
  {
    auto* none = new QStandardItem(none_label);
    none->setData(0u, EntryRole);
    appendRow(none);
  }

  void LookupModel::add(std::uint32_t entry, QString const& label, std::uint32_t display, std::uint32_t kind,
                        QColor const& color)
  {
    auto* item = new QStandardItem(label);
    item->setData(entry, EntryRole);
    item->setData(display, DisplayRole);
    item->setData(kind, KindRole);
    if (color.isValid())
    {
      item->setForeground(color);
    }
    _rows[entry] = rowCount();
    appendRow(item);
  }

  int LookupModel::rowOf(std::uint32_t entry) const
  {
    if (!entry)
    {
      return 0;
    }
    auto const found = _rows.find(entry);
    return found == _rows.end() ? -1 : found->second;
  }

  QString LookupModel::labelOf(std::uint32_t entry) const
  {
    int const row = rowOf(entry);
    return row > 0 ? item(row)->text() : QString("#%1").arg(entry);
  }

  std::uint32_t LookupModel::kindOf(std::uint32_t entry) const
  {
    int const row = rowOf(entry);
    return row > 0 ? item(row)->data(KindRole).toUInt() : 0u;
  }

  QVariant LookupModel::data(QModelIndex const& index, int role) const
  {
    if (role == Qt::DecorationRole && icon_for && index.row() > 0)
    {
      auto const display = QStandardItemModel::data(index, DisplayRole).toUInt();
      if (display)
      {
        auto found = _icons.find(display);
        if (found == _icons.end())
        {
          found = _icons.emplace(display, icon_for(display)).first;
        }
        return found->second;
      }
    }
    return QStandardItemModel::data(index, role);
  }

  EntryPicker::EntryPicker(LookupModel* model, QWidget* parent)
    : QComboBox(parent)
    , _model(model)
  {
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setModel(model);
    setMaxVisibleItems(18);
    setMinimumContentsLength(12);
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* completer = new QCompleter(model, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(18);
    setCompleter(completer);
    setCurrentIndex(0);
  }

  void EntryPicker::setEntry(std::uint32_t entry)
  {
    int row = _model->rowOf(entry);
    if (row < 0)
    {
      // Something the lists do not know (e.g. removed from the database): keep it selectable as is.
      _model->add(entry, QString("Unknown (#%1)").arg(entry));
      row = _model->rowOf(entry);
    }
    setCurrentIndex(row);
  }

  QWidget* browsable(EntryPicker* picker)
  {
    if (!picker->lookup()->browse)
    {
      return picker;
    }
    auto* row = new QWidget(picker->parentWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(picker, 1);
    auto* browse = new QToolButton(row);
    browse->setText("…");
    browse->setToolTip("Browse with icons and previews, or make a new one there");
    browse->setEnabled(picker->isEnabled());
    layout->addWidget(browse);
    QObject::connect(browse, &QToolButton::clicked, picker, [picker]
    {
      if (auto const chosen = picker->lookup()->browse(picker, picker->entry()))
      {
        picker->setEntry(*chosen);
      }
    });
    return row;
  }

  std::uint32_t EntryPicker::entry() const
  {
    QString const text = currentText().trimmed();
    if (text.isEmpty())
    {
      return 0;
    }
    int const index = findText(text);
    if (index >= 0)
    {
      return itemData(index, LookupModel::EntryRole).toUInt();
    }
    auto const match = QRegularExpression("^#?(\\d+)$").match(text);
    return match.hasMatch() ? match.captured(1).toUInt() : 0;
  }

  QString ContentLookups::creatureName(std::uint32_t entry) const { return creatures->labelOf(entry); }
  QString ContentLookups::objectName(std::uint32_t entry) const { return objects->labelOf(entry); }
  QString ContentLookups::itemName(std::uint32_t entry) const { return items->labelOf(entry); }
  QString ContentLookups::questName(std::uint32_t entry) const { return quests->labelOf(entry); }
  QString ContentLookups::spellName(std::uint32_t entry) const { return spells->labelOf(entry); }

  void ContentLookups::addItem(std::uint32_t entry, QString const& name, std::uint32_t quality, std::uint32_t display)
  {
    if (items->rowOf(entry) < 0)
    {
      items->add(entry, QString("%1 (#%2)").arg(name).arg(entry), display, quality, qualityColor(quality));
    }
    else
    {
      items->item(items->rowOf(entry))->setText(QString("%1 (#%2)").arg(name).arg(entry));
    }
  }

  void ContentLookups::addQuest(std::uint32_t entry, QString const& title, std::uint32_t level)
  {
    QString const label = QString("%1 (level %2, #%3)").arg(title).arg(level).arg(entry);
    if (quests->rowOf(entry) < 0)
    {
      quests->add(entry, label, 0, level);
    }
    else
    {
      quests->item(quests->rowOf(entry))->setText(label);
    }
  }

  void ContentLookups::addSpell(std::uint32_t entry, QString const& name)
  {
    QString const label = QString("%1 (#%2)").arg(name).arg(entry);
    if (spells->rowOf(entry) < 0)
    {
      spells->add(entry, label);
    }
    else
    {
      spells->item(spells->rowOf(entry))->setText(label);
    }
  }

  void ContentLookups::addCreature(std::uint32_t entry, QString const& name, std::uint32_t npc_flags)
  {
    QString const label = QString("%1 (#%2)").arg(name).arg(entry);
    if (creatures->rowOf(entry) < 0)
    {
      creatures->add(entry, label, 0, npc_flags);
    }
    else
    {
      creatures->item(creatures->rowOf(entry))->setText(label);
      creatures->item(creatures->rowOf(entry))->setData(npc_flags, LookupModel::KindRole);
    }
  }

  std::unique_ptr<ContentLookups> buildLookups(LookupSource const& source)
  {
    auto lookups = std::make_unique<ContentLookups>();
    lookups->vanilla = source.vanilla;
    auto const make = [](std::vector<NamedEntry> const& entries, auto&& label, auto&& accept)
    {
      auto model = std::make_unique<LookupModel>();
      for (auto const& entry : entries)
      {
        if (accept(entry))
        {
          model->add(entry.entry, label(entry), entry.display, entry.kind);
        }
      }
      return model;
    };
    auto const named = [](NamedEntry const& e) { return QString("%1 (#%2)").arg(QString::fromStdString(e.name)).arg(e.entry); };
    auto const all = [](NamedEntry const&) { return true; };

    lookups->creatures = make(source.creatures, named, all);
    lookups->objects = make(source.objects, named, all);
    lookups->chests = make(source.objects, named, [](NamedEntry const& e) { return e.kind == CHEST_TYPE; });
    lookups->usable_objects = make(source.objects, named, [](NamedEntry const& e) { return e.kind == USABLE_OBJECT_TYPE; });
    lookups->spells = make(source.spells, named, all);
    lookups->quests = make(source.quests, [](NamedEntry const& e)
    {
      return QString("%1 (level %2, #%3)").arg(QString::fromStdString(e.name)).arg(e.kind).arg(e.entry);
    }, all);
    lookups->reputation_factions = make(source.reputation_factions, [](NamedEntry const& e)
    {
      return QString::fromStdString(e.name);
    }, all);
    lookups->zones = make(source.zones, [](NamedEntry const& e) { return QString::fromStdString(e.name); }, all);
    lookups->faction_templates = make(source.faction_templates, [](NamedEntry const& e)
    {
      return QString("%1 (#%2)").arg(QString::fromStdString(e.name)).arg(e.entry);
    }, all);

    lookups->items = std::make_unique<LookupModel>();
    for (auto const& item : source.items)
    {
      lookups->items->add(item.entry, named(item), item.display, item.kind, qualityColor(item.kind));
    }
    lookups->items->icon_for = source.item_icon;
    lookups->area_triggers = source.area_triggers;
    return lookups;
  }
}
