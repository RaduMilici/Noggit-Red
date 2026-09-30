// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/item/ItemWorkflow.hpp>

#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/content/SqlApply.hpp>
#include <noggit/ui/item/ItemEditorDialog.hpp>

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Item
{
  namespace I = Noggit::Item;

  namespace
  {
    QString exportPath(std::uint32_t item)
    {
      return QString("items/item_%1.sql").arg(item);
    }

    void recordItem(Content::ContentSession& session, mysql::content::ItemEditorData const& data, std::uint32_t item,
                    I::ItemFields const& fields)
    {
      writeSqlExport(exportPath(item), mysql::content::itemSnapshot(session.db(), data.schema, item), {});
      session.lookups().addItem(item, QString::fromStdString(fields.name.value_or(std::string())),
                                fields.quality.value_or(1), fields.display_id.value_or(0));
    }
  }

  std::uint32_t createItem(Content::ContentSession& session, QWidget* parent, QString const& suggested_name)
  {
    mysql::content::ItemEditorData data;
    std::string error;
    if (!mysql::content::loadItem(session.db(), 0, data, &error))
    {
      QMessageBox::critical(parent, "New item", QString::fromStdString(error));
      return 0;
    }
    ItemEditorSetup setup;
    setup.entry = data.next_entry;
    setup.data = &data;
    setup.lookups = &session.lookups();
    setup.suggested_name = suggested_name;
    ItemEditorDialog dialog(setup, parent);
    if (dialog.exec() != QDialog::Accepted)
    {
      return 0;
    }
    auto const fields = dialog.fields();
    QString const name = QString::fromStdString(fields.name.value_or(std::string()));
    if (!applyPlan(parent, session.db(), "Create item", QString("Create the item \"%1\" (ID %2)?").arg(name).arg(setup.entry),
                   {}, {I::buildItemInsert(data.schema, setup.entry, fields)}, {}, true))
    {
      return 0;
    }
    recordItem(session, data, setup.entry, fields);
    return setup.entry;
  }

  bool editItem(Content::ContentSession& session, QWidget* parent, std::uint32_t item)
  {
    mysql::content::ItemEditorData data;
    std::string error;
    if (!mysql::content::loadItem(session.db(), item, data, &error))
    {
      QMessageBox::critical(parent, "Edit item", QString::fromStdString(error));
      return false;
    }
    ItemEditorSetup setup;
    setup.editing = true;
    setup.entry = item;
    setup.data = &data;
    setup.lookups = &session.lookups();
    ItemEditorDialog dialog(setup, parent);
    if (dialog.exec() != QDialog::Accepted)
    {
      return false;
    }
    QString const name = QString::fromStdString(data.values.name.value_or(std::string()));
    if (dialog.deleteRequested())
    {
      auto const plan = I::planItemDelete(data.schema, item, data.uses);
      if (!plan.problems.empty())
      {
        showProblems(parent, "Delete item", toQStringList(plan.problems));
        return false;
      }
      if (!applyPlan(parent, session.db(), "Delete item", QString("Delete the item \"%1\" (ID %2)? This cannot be undone.")
                                                            .arg(name).arg(item),
                     toQStringList(plan.summary), plan.statements))
      {
        return false;
      }
      removeSqlExport(exportPath(item));
      return true;
    }
    auto const fields = dialog.fields();
    auto const update = I::buildItemUpdate(data.schema, item, fields);
    if (update.empty() || !applyPlan(parent, session.db(), "Save item", QString("Save the changes to \"%1\" (ID %2)?")
                                                                          .arg(name).arg(item), {}, {update}))
    {
      return false;
    }
    recordItem(session, data, item, fields);
    return true;
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
        if (Noggit::Content::isCustom(entry))
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
    auto const edit_selected = [&]
    {
      if (auto* item = list->currentItem(); item && editItem(session, &dialog, item->data(Qt::UserRole).toUInt()))
      {
        fill();
      }
    };
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] { if (createItem(session, &dialog)) fill(); });
    QObject::connect(edit, &QPushButton::clicked, &dialog, edit_selected);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, edit_selected);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
  }
}

#endif
