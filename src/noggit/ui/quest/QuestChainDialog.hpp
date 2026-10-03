// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Visual quest chain editor.

#include <noggit/quest/QuestChain.hpp>

#include <QtWidgets/QDialog>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

class QGraphicsScene;
class QLabel;
class QPushButton;

namespace Noggit::Ui::Content
{
  class ContentSession;
  class EntryPicker;
}

namespace Noggit::Ui::Quest
{
  namespace QuestChainDetail
  {
    class ChainView;
    class QuestNodeItem;
    class LinkItem;
  }

  // Shows quest chains as boxes joined by arrows, left to right. Drag from the handle on a quest's right
  // edge to another quest to make it the next step (asking "all / any / instead" when it already needs
  // something); right-click a quest for either/or choices, or an arrow for more. Link changes are kept until
  // "Save changes". Only quests made in Noggit are changed; game quests can still start one of their chains.
  class QuestChainDialog : public QDialog
  {
  public:
    QuestChainDialog(Content::ContentSession& session, std::uint32_t focus_quest, QWidget* parent = nullptr);
    ~QuestChainDialog() override;

    void done(int result) override;

  private:
    friend class QuestChainDetail::ChainView;

    void reloadModel();
    void rebuildScene(bool arrange);
    void refreshLinks();
    void addToView(std::uint32_t quest);
    void requestLink(std::uint32_t from, std::uint32_t to);
    void nodeMenu(std::uint32_t quest, QPoint const& screen_pos);
    void linkMenu(std::uint32_t from, std::uint32_t to, QPoint const& screen_pos);
    void openQuest(std::uint32_t quest);
    void newQuestAfter(std::uint32_t quest);
    bool ensureNoPendingChanges();
    bool save();
    void updateState();
    bool report(Noggit::Quest::ChainResult const& result); // shows a refusal; false when refused
    QString titleOf(std::uint32_t quest) const;

    Content::ContentSession& _session;
    std::unique_ptr<Noggit::Quest::ChainModel> _model;
    std::map<std::uint32_t, QString> _givers; // quest -> "Given by" text
    std::set<std::uint32_t> _shown;
    std::map<std::uint32_t, QuestChainDetail::QuestNodeItem*> _nodes;
    std::vector<QuestChainDetail::LinkItem*> _links;

    QGraphicsScene* _scene = nullptr;
    QuestChainDetail::ChainView* _view = nullptr;
    Content::EntryPicker* _add_picker = nullptr;
    QLabel* _state = nullptr;
    QPushButton* _save = nullptr;
  };
}
