// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/npc/NpcWorkflow.hpp>

#include <noggit/ui/content/ClientData.hpp>
#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/SqlApply.hpp>
#include <noggit/ui/npc/NpcEditorDialog.hpp>

#include <QtWidgets/QMessageBox>

namespace Noggit::Ui::Npc
{
  namespace N = Noggit::Npc;

  namespace
  {
    QString exportPath(std::uint32_t npc)
    {
      return QString("npcs/npc_%1.sql").arg(npc);
    }
  }

  std::optional<NpcSaved> editNpc(Content::ContentSession& session, QWidget* parent, bool editing, std::uint32_t npc)
  {
    mysql::content::NpcEditorData data;
    std::string error;
    if (!mysql::content::loadNpc(session.db(), npc, data, &error))
    {
      QMessageBox::critical(parent, "NPC", QString::fromStdString(error));
      return std::nullopt;
    }
    std::uint32_t const entry = editing ? npc : data.next_entry;

    NpcEditorSetup setup;
    setup.editing = editing;
    setup.source = npc;
    setup.entry = entry;
    setup.data = &data;
    setup.lookups = &session.lookups();
    setup.model_name = &Content::ClientData::creatureModelName;
    setup.preview = session.make_npc_preview ? session.make_npc_preview() : nullptr;
    if (setup.preview && session.show_npc_look)
    {
      auto const show = session.show_npc_look;
      auto* preview = setup.preview;
      setup.show_look = [show, preview](std::uint32_t display, float scale) { show(preview, display, scale); };
    }
    NpcEditorDialog dialog(setup, parent);
    if (dialog.exec() != QDialog::Accepted)
    {
      return std::nullopt;
    }

    auto const content = dialog.content();
    auto const plan = N::planNpcSave(data.db, editing ? N::NpcSaveMode::Edit : N::NpcSaveMode::Create, entry, npc,
                                     content, data.dialogue, data.dialogue_ids);
    if (!plan.problems.empty())
    {
      showProblems(parent, "NPC", toQStringList(plan.problems));
      return std::nullopt;
    }
    QString const name = QString::fromStdString(content.fields.name.value_or(std::string()));
    QString const source = QString::fromStdString(data.values.name.value_or(std::string()));
    QString const question = editing ? QString("Save the changes to \"%1\" (ID %2)?").arg(name).arg(entry)
                                     : QString("Create \"%1\" (ID %2) as a copy of %3 (ID %4)?").arg(name).arg(entry)
                                         .arg(source).arg(npc);
    if (plan.statements.empty()
        || !applyPlan(parent, session.db(), editing ? "Save NPC" : "Create NPC", question, toQStringList(plan.summary),
                      plan.statements, plan.cleanup, plan.require_first_row))
    {
      return std::nullopt;
    }
    writeSqlExport(exportPath(entry), mysql::content::npcSnapshot(session.db(), data.db, entry), {});
    session.lookups().addCreature(entry, name, content.fields.npc_flags.value_or(data.values.npc_flags.value_or(0)));
    QMessageBox::information(parent, editing ? "NPC saved" : "NPC created",
      QString("%1 \"%2\" (ID %3).\n\n%4Restart the world server (mangosd) so it loads the %5. A re-runnable copy is in the "
              "project's sql_exports/npcs/ folder.")
        .arg(editing ? "Saved" : "Created").arg(name).arg(entry)
        .arg(editing ? QString() : QString("It is selected in the NPC list: use \"Add Pending Spawn\" to place it.\n\n"))
        .arg(editing ? "changes" : "new NPC"));
    return NpcSaved{entry, content, data.values};
  }

  bool deleteNpc(Content::ContentSession& session, QWidget* parent, std::uint32_t npc)
  {
    mysql::content::NpcEditorData data;
    std::string error;
    if (!mysql::content::loadNpc(session.db(), npc, data, &error))
    {
      QMessageBox::critical(parent, "Delete NPC", QString::fromStdString(error));
      return false;
    }
    auto const plan = N::planNpcDelete(data.db, npc, data.dialogue, data.spawns, data.quest_links);
    if (!plan.problems.empty())
    {
      showProblems(parent, "Delete NPC", toQStringList(plan.problems));
      return false;
    }
    QString const name = QString::fromStdString(data.values.name.value_or(std::string()));
    if (!applyPlan(parent, session.db(), "Delete NPC", QString("Delete \"%1\" (ID %2)? This cannot be undone.").arg(name).arg(npc),
                   toQStringList(plan.summary), plan.statements))
    {
      return false;
    }
    removeSqlExport(exportPath(npc));
    if (session.on_world_changed)
    {
      session.on_world_changed();
    }
    return true;
  }
}

#endif
