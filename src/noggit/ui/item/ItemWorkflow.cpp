// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/item/ItemWorkflow.hpp>

#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/item/ItemEditorDialog.hpp>
#include <noggit/creator/ContentEditors.hpp>
#include <noggit/creator/ItemService.hpp>

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <optional>
#include <stdexcept>

namespace Noggit::Ui::Item
{
  namespace I = Noggit::Item;
  using Content::confirmChange;
  using Content::showProblems;
  using Content::toQStringList;

  namespace
  {
    std::optional<Creator::ItemEditorData> load(Content::ContentSession& session, QWidget* parent, std::uint32_t item,
                                                QString const& title)
    {
      try
      {
        return Creator::loadItem(session.layouts(), item);
      }
      catch (std::exception const& e)
      {
        QMessageBox::warning(parent, title, QString::fromUtf8(e.what()));
        return std::nullopt;
      }
    }

    void listItem(Content::ContentSession& session, std::uint32_t item, I::ItemFields const& fields)
    {
      session.setOwnItem(item, true);
      session.lookups().addItem(item, QString::fromStdString(fields.name.value_or(std::string())),
                                fields.quality.value_or(1), fields.display_id.value_or(0));
    }
  }

  std::uint32_t createItem(Content::ContentSession& session, QWidget* parent, QString const& suggested_name)
  {
    auto data = load(session, parent, 0, "New item");
    if (!data)
    {
      return 0;
    }
    ItemEditorSetup setup;
    setup.entry = data->next_entry;
    setup.data = &*data;
    setup.lookups = &session.lookups();
    setup.suggested_name = suggested_name;
    ItemEditorDialog dialog(setup, parent);
    if (dialog.exec() != QDialog::Accepted)
    {
      return 0;
    }
    auto const fields = dialog.fields();
    try
    {
      auto const item = Creator::createItem(session.layouts(), *data, fields);
      listItem(session, item, fields);
      return item;
    }
    catch (std::exception const& e)
    {
      QMessageBox::warning(parent, "New item", QString::fromUtf8(e.what()));
      return 0;
    }
  }

  bool editItem(Content::ContentSession& session, QWidget* parent, std::uint32_t item)
  {
    auto data = load(session, parent, item, "Edit item");
    if (!data)
    {
      return false;
    }
    ItemEditorSetup setup;
    setup.editing = true;
    setup.deletable = session.ownsItem(item);
    setup.entry = item;
    setup.data = &*data;
    setup.lookups = &session.lookups();
    ItemEditorDialog dialog(setup, parent);
    if (dialog.exec() != QDialog::Accepted)
    {
      return false;
    }
    QString const name = QString::fromStdString(data->values.name.value_or(std::string()));
    try
    {
      if (dialog.deleteRequested())
      {
        auto const plan = I::planItemDelete(data->schema, item, data->uses);
        if (!plan.problems.empty())
        {
          showProblems(parent, "Delete item", toQStringList(plan.problems));
          return false;
        }
        if (!confirmChange(parent, "Delete item", QString("Delete the item \"%1\" from your local world?").arg(name),
                           toQStringList(plan.summary)))
        {
          return false;
        }
        Creator::deleteItem(session.layouts(), *data, item);
        session.setOwnItem(item, false);
        return true;
      }
      auto const fields = dialog.fields();
      Creator::updateItem(session.layouts(), *data, item, fields);
      listItem(session, item, fields);
      return true;
    }
    catch (std::exception const& e)
    {
      QMessageBox::warning(parent, dialog.deleteRequested() ? "Delete item" : "Edit item", QString::fromUtf8(e.what()));
      return false;
    }
  }

  void manageItems(Content::ContentSession& session, QWidget* parent)
  {
    QDialog dialog(parent);
    dialog.setWindowTitle("Your items");
    dialog.resize(520, 480);
    Content::applyEditorStyle(&dialog);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 14, 16, 12);
    auto* header = new QLabel("Your items", &dialog);
    header->setObjectName("ContentHeader");
    root->addWidget(header);
    root->addWidget(Content::hintLabel("Items made with the item editor. Game items cannot be changed; make a new one "
                                       "that looks like them instead.", &dialog));
    auto* list = new QListWidget(&dialog);
    list->setIconSize(QSize(28, 28));
    root->addWidget(list, 1);
    auto const fill = [&session, list]
    {
      list->clear();
      auto& items = *session.lookups().items;
      for (int row = 1; row < items.rowCount(); ++row)
      {
        auto const entry = items.item(row)->data(Content::LookupModel::EntryRole).toUInt();
        if (session.ownsItem(entry))
        {
          auto* item = new QListWidgetItem(items.data(items.index(row, 0), Qt::DecorationRole).value<QIcon>(),
                                           items.item(row)->text(), list);
          item->setData(Qt::UserRole, entry);
          item->setForeground(items.item(row)->foreground());
        }
      }
    };
    fill();
    auto* buttons = new QHBoxLayout();
    auto* create = new QPushButton("New item...", &dialog);
    auto* edit = new QPushButton("Edit...", &dialog);
    auto* close = new QPushButton("Close", &dialog);
    buttons->addWidget(create);
    buttons->addWidget(edit);
    buttons->addStretch();
    buttons->addWidget(close);
    root->addLayout(buttons);
    // The full Item Editor; what it saves is added to this session's lists.
    auto const listed = [&session](std::optional<std::uint32_t> saved)
    {
      if (!saved)
      {
        return false;
      }
      try
      {
        if (auto const info = Creator::ItemService::get(*saved))
        {
          session.lookups().addItem(*saved, info->name, std::uint32_t(info->quality), info->display);
          session.setOwnItem(*saved, true);
        }
        else
        {
          session.setOwnItem(*saved, false); // deleted
        }
      }
      catch (...)
      {
      }
      return true;
    };
    auto const edit_selected = [&]
    {
      if (auto* item = list->currentItem(); item && listed(Creator::editItem(&dialog, item->data(Qt::UserRole).toUInt())))
      {
        fill();
      }
    };
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] { if (listed(Creator::createItem(&dialog))) fill(); });
    QObject::connect(edit, &QPushButton::clicked, &dialog, edit_selected);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, edit_selected);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
  }
}
