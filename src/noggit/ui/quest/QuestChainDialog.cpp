// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/quest/QuestChainDialog.hpp>

#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/quest/QuestWorkflow.hpp>

#include <QtCore/QtMath>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsEllipseItem>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QGraphicsPathItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyleOptionGraphicsItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace Noggit::Ui::Quest
{
  namespace QuestChainDetail
  {
    constexpr qreal NODE_WIDTH = 240.0;
    constexpr qreal NODE_HEIGHT = 86.0;
    constexpr qreal COLUMN_SPACING = 320.0;
    constexpr qreal ROW_SPACING = 120.0;

    // The drag handle on a quest's right edge.
    class HandleItem : public QGraphicsEllipseItem
    {
    public:
      explicit HandleItem(QGraphicsItem* parent)
        : QGraphicsEllipseItem(NODE_WIDTH - 8.0, NODE_HEIGHT / 2.0 - 8.0, 16.0, 16.0, parent)
      {
        setCursor(Qt::CrossCursor);
        setToolTip("Drag to another quest: that quest becomes the next step.");
      }
    };

    class QuestNodeItem : public QGraphicsItem
    {
    public:
      QuestNodeItem(Noggit::Quest::ChainQuest const& quest, QString givers, bool focus, QPalette const& palette)
        : _entry(quest.entry)
        , _title(QString::fromStdString(quest.title))
        , _subtitle(QString("Level %1  ·  #%2  ·  %3").arg(quest.level).arg(quest.entry)
                      .arg(quest.editable ? "your quest" : "game quest"))
        , _givers(std::move(givers))
        , _editable(quest.editable)
        , _focus(focus)
        , _group(quest.exclusive_group > 0 ? quest.exclusive_group : 0)
        , _palette(palette)
      {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setCursor(Qt::OpenHandCursor);
        setToolTip(quest.editable ? QString("Double-click to edit.")
                                  : QString("A quest from the game: it cannot be changed, but your quests can "
                                            "follow it. Double-click to make an editable copy."));
        _handle = new HandleItem(this);
        _handle->setBrush(_palette.color(QPalette::Highlight));
        _handle->setPen(QPen(_palette.color(QPalette::Base), 2.0));
      }

      std::uint32_t entry() const { return _entry; }
      std::function<void()> on_moved;

      QRectF boundingRect() const override { return QRectF(-2.0, -11.0, NODE_WIDTH + 4.0, NODE_HEIGHT + 13.0); }

      void paint(QPainter* painter, QStyleOptionGraphicsItem const*, QWidget*) override
      {
        painter->setRenderHint(QPainter::Antialiasing);
        QColor const accent = _palette.color(QPalette::Highlight);
        QColor const border = _editable ? accent : _palette.color(QPalette::Mid);
        painter->setPen(QPen(border, isSelected() ? 3.0 : (_focus ? 2.0 : 1.2),
                             _editable ? Qt::SolidLine : Qt::DashLine));
        painter->setBrush(_palette.color(_editable ? QPalette::Base : QPalette::AlternateBase));
        painter->drawRoundedRect(QRectF(0.0, 0.0, NODE_WIDTH, NODE_HEIGHT), 8.0, 8.0);

        qreal const text_width = NODE_WIDTH - 24.0;
        QFont title_font = painter->font();
        title_font.setBold(true);
        painter->setFont(title_font);
        painter->setPen(_palette.color(QPalette::Text));
        painter->drawText(QRectF(10.0, 8.0, text_width, 20.0), Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(title_font).elidedText(_title, Qt::ElideRight, static_cast<int>(text_width)));

        QFont small = painter->font();
        small.setBold(false);
        small.setPointSizeF(small.pointSizeF() * 0.9);
        painter->setFont(small);
        QColor muted = _palette.color(QPalette::Text);
        muted.setAlphaF(0.65);
        painter->setPen(muted);
        painter->drawText(QRectF(10.0, 32.0, text_width, 18.0), Qt::AlignLeft | Qt::AlignVCenter, _subtitle);
        QString const givers = _givers.isEmpty() ? QString("Nobody gives this quest yet") : "Given by " + _givers;
        painter->drawText(QRectF(10.0, 54.0, text_width, 18.0), Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(small).elidedText(givers, Qt::ElideRight, static_cast<int>(text_width)));

        if (_group)
        {
          // Either/or: quests sharing this colour close each other.
          QColor const chip = QColor::fromHsv(static_cast<int>((static_cast<std::uint32_t>(_group) * 67u) % 360u), 150, 200);
          QFont tiny = small;
          tiny.setBold(true);
          tiny.setPointSizeF(small.pointSizeF() * 0.85);
          painter->setFont(tiny);
          QString const text = "EITHER/OR";
          qreal const width = QFontMetrics(tiny).horizontalAdvance(text) + 12.0;
          QRectF const pill(NODE_WIDTH - width - 10.0, -9.0, width, 18.0);
          painter->setPen(Qt::NoPen);
          painter->setBrush(chip);
          painter->drawRoundedRect(pill, 9.0, 9.0);
          painter->setPen(Qt::white);
          painter->drawText(pill, Qt::AlignCenter, text);
        }
      }

    protected:
      QVariant itemChange(GraphicsItemChange change, QVariant const& value) override
      {
        if (change == ItemPositionHasChanged && on_moved)
        {
          on_moved();
        }
        return QGraphicsItem::itemChange(change, value);
      }

    private:
      std::uint32_t _entry;
      QString _title, _subtitle, _givers;
      bool _editable, _focus;
      std::int32_t _group;
      QPalette _palette;
      HandleItem* _handle = nullptr;
    };

    // An arrow A -> B. Solid: B needs A done AND is offered right after A. Dashed: B only needs A done.
    // Dotted: A offers B, but B does not need A (unusual).
    class LinkItem : public QGraphicsPathItem
    {
    public:
      // `group`: "ALL" / "ANY" when the quest this arrow leads to needs several quests, "" otherwise.
      LinkItem(QuestNodeItem* from, QuestNodeItem* to, Noggit::Quest::ChainLink const& link, QColor const& color,
               QString group)
        : _from(from), _to(to), _link(link), _color(color), _group(std::move(group))
      {
        setZValue(-1.0);
        Qt::PenStyle const style = link.needs_done && link.offers ? Qt::SolidLine
                                 : link.needs_done ? Qt::DashLine : Qt::DotLine;
        setPen(QPen(color, 2.0, style, Qt::RoundCap));
        setCursor(Qt::PointingHandCursor);
        QString const group_tip = _group == "ALL" ? QString("\nIt is one of several quests that are ALL needed.")
                                : _group == "ANY" ? QString("\nAny ONE of the quests leading here is enough.") : QString();
        setToolTip((link.needs_done && link.offers
                     ? QString("Needs the previous quest done, and is offered right after it is handed in.\n"
                               "Right-click to change.")
                     : link.needs_done ? QString("Needs the previous quest done (not offered automatically).\n"
                                                 "Right-click to change.")
                                       : QString("Offered after the previous quest, but does not need it done.\n"
                                                 "Right-click to change.")) + group_tip);
        updatePath();
      }

      Noggit::Quest::ChainLink const& link() const { return _link; }

      void updatePath()
      {
        QPointF const start = _from->scenePos() + QPointF(NODE_WIDTH, NODE_HEIGHT / 2.0);
        QPointF const end = _to->scenePos() + QPointF(0.0, NODE_HEIGHT / 2.0);
        qreal const bend = std::max<qreal>(60.0, std::abs(end.x() - start.x()) / 2.0);
        QPainterPath path(start);
        path.cubicTo(start + QPointF(bend, 0.0), end - QPointF(bend, 0.0), end);
        setPath(path);
      }

      QPainterPath shape() const override
      {
        QPainterPathStroker stroker;
        stroker.setWidth(12.0);
        return stroker.createStroke(path());
      }

      QRectF boundingRect() const override
      {
        return QGraphicsPathItem::boundingRect().adjusted(-12.0, -12.0, 12.0, 12.0);
      }

      void paint(QPainter* painter, QStyleOptionGraphicsItem const* option, QWidget* widget) override
      {
        painter->setRenderHint(QPainter::Antialiasing);
        QGraphicsPathItem::paint(painter, option, widget);
        // Arrow head along the end tangent.
        QPointF const tip = path().pointAtPercent(1.0);
        qreal const angle = qDegreesToRadians(-path().angleAtPercent(1.0));
        QPointF const back(std::cos(angle) * 12.0, std::sin(angle) * 12.0);
        QPointF const side(-back.y() * 0.5, back.x() * 0.5);
        painter->setPen(Qt::NoPen);
        painter->setBrush(_color);
        painter->drawPolygon(QPolygonF({tip, tip - back + side, tip - back - side}));

        if (!_group.isEmpty())
        {
          // Near the arrow head: all of the incoming quests are needed, or any one of them.
          QFont tiny = painter->font();
          tiny.setBold(true);
          tiny.setPointSizeF(tiny.pointSizeF() * 0.8);
          painter->setFont(tiny);
          QPointF const at = path().pointAtPercent(0.8);
          qreal const width = QFontMetrics(tiny).horizontalAdvance(_group) + 10.0;
          QRectF const pill(at.x() - width / 2.0, at.y() - 8.0, width, 16.0);
          painter->setBrush(_group == "ALL" ? QColor(196, 64, 64) : QColor(56, 150, 80));
          painter->drawRoundedRect(pill, 8.0, 8.0);
          painter->setPen(Qt::white);
          painter->drawText(pill, Qt::AlignCenter, _group);
        }
      }

    private:
      QuestNodeItem* _from;
      QuestNodeItem* _to;
      Noggit::Quest::ChainLink _link;
      QColor _color;
      QString _group;
    };

    QuestNodeItem* nodeAt(QGraphicsView* view, QPoint const& pos)
    {
      for (auto* item : view->items(pos))
      {
        for (auto* current = item; current; current = current->parentItem())
        {
          if (auto* node = dynamic_cast<QuestNodeItem*>(current))
          {
            return node;
          }
        }
      }
      return nullptr;
    }

    class ChainView : public QGraphicsView
    {
    public:
      ChainView(QGraphicsScene* scene, QuestChainDialog* dialog)
        : QGraphicsView(scene, dialog), _dialog(dialog)
      {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
      }

    protected:
      void mousePressEvent(QMouseEvent* event) override
      {
        if (event->button() == Qt::LeftButton)
        {
          for (auto* item : items(event->pos()))
          {
            if (dynamic_cast<HandleItem*>(item))
            {
              _drag_from = static_cast<QuestNodeItem*>(item->parentItem());
              QPointF const start = mapToScene(event->pos());
              _drag_line = scene()->addLine(QLineF(start, start),
                                            QPen(palette().color(QPalette::Highlight), 2.0, Qt::DashLine));
              event->accept();
              return;
            }
          }
        }
        QGraphicsView::mousePressEvent(event);
      }

      void mouseMoveEvent(QMouseEvent* event) override
      {
        if (_drag_line)
        {
          _drag_line->setLine(QLineF(_drag_line->line().p1(), mapToScene(event->pos())));
          return;
        }
        QGraphicsView::mouseMoveEvent(event);
      }

      void mouseReleaseEvent(QMouseEvent* event) override
      {
        if (_drag_line)
        {
          delete _drag_line;
          _drag_line = nullptr;
          auto* from = _drag_from;
          _drag_from = nullptr;
          if (auto* to = nodeAt(this, event->pos()); to && from)
          {
            _dialog->requestLink(from->entry(), to->entry());
          }
          return;
        }
        QGraphicsView::mouseReleaseEvent(event);
      }

      void mouseDoubleClickEvent(QMouseEvent* event) override
      {
        if (auto* node = nodeAt(this, event->pos()))
        {
          _dialog->openQuest(node->entry());
          return;
        }
        QGraphicsView::mouseDoubleClickEvent(event);
      }

      void contextMenuEvent(QContextMenuEvent* event) override
      {
        if (auto* node = nodeAt(this, event->pos()))
        {
          _dialog->nodeMenu(node->entry(), event->globalPos());
          return;
        }
        for (auto* item : items(event->pos()))
        {
          if (auto* link = dynamic_cast<LinkItem*>(item))
          {
            _dialog->linkMenu(link->link().from, link->link().to, event->globalPos());
            return;
          }
        }
      }

      void wheelEvent(QWheelEvent* event) override
      {
        if (event->modifiers() & Qt::ControlModifier)
        {
          qreal const factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
          scale(factor, factor);
          return;
        }
        QGraphicsView::wheelEvent(event);
      }

    private:
      QuestChainDialog* _dialog;
      QuestNodeItem* _drag_from = nullptr;
      QGraphicsLineItem* _drag_line = nullptr;
    };
  }

  using namespace QuestChainDetail;
  namespace Q = Noggit::Quest;

  QuestChainDialog::QuestChainDialog(Content::ContentSession& session, std::uint32_t focus_quest, QWidget* parent)
    : QDialog(parent)
    , _session(session)
  {
    setWindowTitle("Quest chain");
    resize(1160, 680);
    Content::applyEditorStyle(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);

    auto* toolbar = new QHBoxLayout();
    toolbar->addWidget(new QLabel("Add a quest:", this));
    _add_picker = new Content::EntryPicker(session.lookups().quests.get(), this);
    _add_picker->setToolTip("Bring another quest (with its own chain) into view, e.g. to make it follow one of these.");
    toolbar->addWidget(_add_picker, 1);
    auto* add = new QPushButton("Add", this);
    toolbar->addWidget(add);
    auto* arrange = new QPushButton("Tidy up", this);
    arrange->setToolTip("Lay the quests out again, left to right in the order they are done.");
    toolbar->addWidget(arrange);
    root->addLayout(toolbar);

    _scene = new QGraphicsScene(this);
    _view = new ChainView(_scene, this);
    root->addWidget(_view, 1);

    root->addWidget(Content::hintLabel(
      "Drag from the <b>●</b> on a quest's right edge to another quest to make it the next step. <b>Solid</b> arrow: "
      "offered right after the previous quest; <b>dashed</b>: only unlocked by it. <b>ALL</b> / <b>ANY</b>: the quest "
      "needs all, or any one, of the quests leading to it. Quests with the same <b>EITHER/OR</b> colour close each "
      "other. Right-click for more; double-click to edit; Ctrl + wheel zooms.", this));

    auto* footer = new QHBoxLayout();
    _state = Content::hintLabel({}, this);
    footer->addWidget(_state, 1);
    _save = new QPushButton("Save changes", this);
    auto* close = new QPushButton("Close", this);
    footer->addWidget(_save);
    footer->addWidget(close);
    root->addLayout(footer);

    connect(add, &QPushButton::clicked, this, [this]
    {
      if (auto const quest = _add_picker->entry())
      {
        addToView(quest);
        _add_picker->setCurrentIndex(0);
      }
    });
    connect(arrange, &QPushButton::clicked, this, [this] { rebuildScene(true); });
    connect(_save, &QPushButton::clicked, this, [this] { save(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    reloadModel();
    if (_model->find(focus_quest))
    {
      _shown = _model->chainOf(focus_quest);
    }
    else
    {
      // Nothing selected: every chain the user's own quests are part of.
      for (auto const& quest : session.quests().quests)
      {
        if (quest.editable)
        {
          auto const chain = _model->chainOf(quest.entry);
          _shown.insert(chain.begin(), chain.end());
        }
      }
    }
    rebuildScene(true);
    if (auto found = _nodes.find(focus_quest); found != _nodes.end())
    {
      found->second->setSelected(true);
      _view->centerOn(found->second);
    }
  }

  QuestChainDialog::~QuestChainDialog() = default;

  QString QuestChainDialog::titleOf(std::uint32_t quest) const
  {
    auto const* found = _model->find(quest);
    return found ? QString::fromStdString(found->title) : QString("#%1").arg(quest);
  }

  void QuestChainDialog::reloadModel()
  {
    auto const& list = _session.quests();
    _model = std::make_unique<Q::ChainModel>(list.quests);
    std::map<std::uint32_t, QStringList> givers;
    for (auto const& link : list.links)
    {
      if (link.starts)
      {
        auto const& lookups = _session.lookups();
        QString const name = link.giver.kind == Q::Giver::Kind::Npc ? lookups.creatureName(link.giver.entry)
                                                                    : lookups.objectName(link.giver.entry);
        givers[link.quest] << name.section(" (#", 0, 0);
      }
    }
    _givers.clear();
    for (auto const& [quest, names] : givers)
    {
      _givers[quest] = names.join(", ");
    }
    for (auto it = _shown.begin(); it != _shown.end();)
    {
      it = _model->find(*it) ? std::next(it) : _shown.erase(it);
    }
  }

  void QuestChainDialog::rebuildScene(bool arrange)
  {
    std::map<std::uint32_t, QPointF> positions;
    std::uint32_t selected = 0;
    for (auto const& [entry, node] : _nodes)
    {
      if (!arrange)
      {
        positions[entry] = node->pos();
      }
      if (node->isSelected())
      {
        selected = entry;
      }
    }
    _links.clear();
    _nodes.clear();
    _scene->clear();

    // Column by chain depth, within a column by ID. Quests without a position yet go below the arranged ones.
    auto const layers = _model->layers(_shown);
    std::map<int, int> rows;
    qreal base_y = 0.0;
    for (auto const& [entry, position] : positions)
    {
      if (_shown.count(entry))
      {
        base_y = std::max(base_y, position.y() + ROW_SPACING);
      }
    }
    for (auto const entry : _shown)
    {
      auto const* quest = _model->find(entry);
      if (!quest)
      {
        continue;
      }
      auto* node = new QuestNodeItem(*quest, _givers.count(entry) ? _givers[entry] : QString(), entry == selected,
                                     _view->palette());
      int const layer = layers.count(entry) ? layers.at(entry) : 0;
      node->setPos(positions.count(entry) ? positions[entry] : QPointF(layer * COLUMN_SPACING, base_y + rows[layer]++ * ROW_SPACING));
      node->on_moved = [this] { for (auto* link : _links) link->updatePath(); };
      _scene->addItem(node);
      _nodes[entry] = node;
      node->setSelected(entry == selected);
    }
    refreshLinks();
    _scene->setSceneRect(_scene->itemsBoundingRect().adjusted(-200.0, -150.0, 200.0, 150.0));
  }

  void QuestChainDialog::refreshLinks()
  {
    for (auto* link : _links)
    {
      _scene->removeItem(link);
      delete link;
    }
    _links.clear();
    QColor const color = _view->palette().color(QPalette::Text);
    for (auto const& link : _model->links(_shown))
    {
      auto const prerequisites = _model->prerequisitesOf(link.to);
      QString const group = link.needs_done && prerequisites.quests.size() >= 2
                          ? (prerequisites.mode == Q::Prerequisites::Mode::All ? QString("ALL") : QString("ANY"))
                          : QString();
      auto* item = new LinkItem(_nodes.at(link.from), _nodes.at(link.to), link, color, group);
      _scene->addItem(item);
      _links.push_back(item);
    }
    for (auto const& [entry, node] : _nodes)
    {
      node->update();
    }
    updateState();
  }

  void QuestChainDialog::addToView(std::uint32_t quest)
  {
    if (!_model->find(quest))
    {
      return;
    }
    auto const chain = _model->chainOf(quest);
    if (!std::all_of(chain.begin(), chain.end(), [this](auto id) { return _shown.count(id) > 0; }))
    {
      _shown.insert(chain.begin(), chain.end());
      rebuildScene(false);
    }
    if (auto found = _nodes.find(quest); found != _nodes.end())
    {
      _scene->clearSelection();
      found->second->setSelected(true);
      _view->centerOn(found->second);
    }
  }

  bool QuestChainDialog::report(Q::ChainResult const& result)
  {
    if (!result)
    {
      QMessageBox::information(this, "Quest chain", QString::fromStdString(Q::describe(result, _session.names())));
      return false;
    }
    return true;
  }

  void QuestChainDialog::requestLink(std::uint32_t from, std::uint32_t to)
  {
    if (from == to || !_model->find(from) || !_model->find(to))
    {
      return;
    }
    auto const current = _model->prerequisitesOf(to).quests;
    if (std::find(current.begin(), current.end(), from) != current.end())
    {
      return;
    }
    Q::ChainResult result;
    if (current.empty())
    {
      result = _model->addPrerequisite(to, from);
    }
    else
    {
      // Already needs something: both, either one, or instead?
      QStringList names;
      for (auto const id : current)
      {
        names << "\"" + titleOf(id) + "\"";
      }
      QMessageBox box(QMessageBox::Question, "Quest chain",
                      QString("\"%1\" already needs %2. What should players do before it?")
                        .arg(titleOf(to), names.join(" and ")), QMessageBox::NoButton, this);
      auto* all = box.addButton(QString("All of them"), QMessageBox::AcceptRole);
      auto* any = box.addButton(QString("Any one of them"), QMessageBox::AcceptRole);
      auto* instead = box.addButton(QString("Only \"%1\"").arg(titleOf(from)), QMessageBox::AcceptRole);
      box.addButton(QMessageBox::Cancel);
      box.exec();
      if (box.clickedButton() == all || box.clickedButton() == any)
      {
        auto prerequisites = _model->prerequisitesOf(to);
        prerequisites.quests.push_back(from);
        prerequisites.mode = box.clickedButton() == all ? Q::Prerequisites::Mode::All : Q::Prerequisites::Mode::Any;
        result = _model->setPrerequisites(to, prerequisites);
      }
      else if (box.clickedButton() == instead)
      {
        result = _model->setPrerequisites(to, {{from}, Q::Prerequisites::Mode::Any});
      }
      else
      {
        return;
      }
    }
    if (!report(result))
    {
      return;
    }
    // A single, simple step: offer it right away too, when the previous quest can and offers nothing yet.
    auto const* source = _model->find(from);
    if (_model->prerequisitesOf(to).quests.size() == 1 && source->editable && !source->next_in_chain)
    {
      _model->setOffers(from, to);
    }
    _shown.insert(from);
    _shown.insert(to);
    rebuildScene(false);
  }

  void QuestChainDialog::nodeMenu(std::uint32_t quest, QPoint const& screen_pos)
  {
    auto const* node = _model->find(quest);
    if (!node)
    {
      return;
    }
    QMenu menu(this);
    menu.addAction(node->editable ? "Edit quest..." : "Copy into an editable quest...", [this, quest] { openQuest(quest); });
    menu.addAction("New quest after this one...", [this, quest] { newQuestAfter(quest); });
    menu.addSeparator();
    if (node->editable)
    {
      auto* either = menu.addMenu("Only one of this and...");
      for (auto const id : _shown)
      {
        auto const* other = _model->find(id);
        if (id == quest || !other || !other->editable)
        {
          continue;
        }
        auto const partners = _model->exclusiveWith(quest);
        bool const already = std::find(partners.begin(), partners.end(), id) != partners.end();
        auto* action = either->addAction(QString::fromStdString(other->title), [this, quest, id]
        {
          auto members = _model->exclusiveWith(quest);
          members.push_back(quest);
          members.push_back(id);
          if (report(_model->makeExclusive(members)))
          {
            rebuildScene(false);
          }
        });
        action->setCheckable(true);
        action->setChecked(already);
        action->setEnabled(!already);
      }
      either->setEnabled(!either->actions().isEmpty());
      if (!_model->exclusiveWith(quest).empty())
      {
        menu.addAction("Leave its either/or choice", [this, quest]
        {
          if (report(_model->leaveExclusiveGroup(quest)))
          {
            rebuildScene(false);
          }
        });
      }
      if (!_model->prerequisitesOf(quest).quests.empty())
      {
        menu.addAction("Remove what comes before", [this, quest]
        {
          if (report(_model->setPrerequisites(quest, {})))
          {
            rebuildScene(false);
          }
        });
      }
      menu.addSeparator();
    }
    menu.addAction("Hide from view", [this, quest]
    {
      _shown.erase(quest);
      rebuildScene(false);
    });
    menu.exec(screen_pos);
  }

  void QuestChainDialog::linkMenu(std::uint32_t from, std::uint32_t to, QPoint const& screen_pos)
  {
    auto const* source = _model->find(from);
    auto const* target = _model->find(to);
    if (!source || !target)
    {
      return;
    }
    QMenu menu(this);
    auto* offers = menu.addAction(QString("Offer \"%1\" right after \"%2\" is handed in")
                                    .arg(titleOf(to), titleOf(from)));
    offers->setCheckable(true);
    offers->setChecked(source->next_in_chain == to);
    offers->setEnabled(source->editable);
    connect(offers, &QAction::toggled, this, [this, from, to](bool on)
    {
      if (report(_model->setOffers(from, on ? to : 0)))
      {
        rebuildScene(false);
      }
    });
    auto const prerequisites = _model->prerequisitesOf(to);
    if (prerequisites.quests.size() >= 2 && target->editable)
    {
      bool const all = prerequisites.mode == Q::Prerequisites::Mode::All;
      menu.addAction(all ? "Any one of the earlier quests is enough" : "All of the earlier quests are needed", [this, to, all]
      {
        auto changed = _model->prerequisitesOf(to);
        changed.mode = all ? Q::Prerequisites::Mode::Any : Q::Prerequisites::Mode::All;
        if (report(_model->setPrerequisites(to, changed)))
        {
          rebuildScene(false);
        }
      });
    }
    menu.addAction("Remove this link", [this, from, to]
    {
      auto const needed = _model->prerequisitesOf(to).quests;
      bool const needs = std::find(needed.begin(), needed.end(), from) != needed.end();
      if (needs && !report(_model->removePrerequisite(to, from)))
      {
        return;
      }
      if (_model->find(from)->next_in_chain == to)
      {
        report(_model->setOffers(from, 0));
      }
      rebuildScene(false);
    });
    menu.exec(screen_pos);
  }

  bool QuestChainDialog::ensureNoPendingChanges()
  {
    if (!_model->hasChanges())
    {
      return true;
    }
    auto const answer = QMessageBox::question(this, "Quest chain", "Save your link changes first?",
                                              QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Save && save();
  }

  void QuestChainDialog::openQuest(std::uint32_t quest)
  {
    if (!ensureNoPendingChanges())
    {
      return;
    }
    auto const* node = _model->find(quest);
    if (node && editQuest(_session, this, node->editable ? EditorMode::Edit : EditorMode::Copy, quest))
    {
      reloadModel();
      rebuildScene(false);
    }
  }

  void QuestChainDialog::newQuestAfter(std::uint32_t quest)
  {
    if (!ensureNoPendingChanges())
    {
      return;
    }
    // Given by whoever takes the previous quest back.
    std::uint32_t giver = 0;
    for (auto const& link : _session.quests().links)
    {
      if (link.quest == quest && !link.starts && link.giver.kind == Q::Giver::Kind::Npc)
      {
        giver = link.giver.entry;
        break;
      }
    }
    if (auto const created = editQuest(_session, this, EditorMode::Create, 0, quest, giver))
    {
      reloadModel();
      _shown.insert(quest);
      addToView(created);
      if (auto from = _nodes.find(quest), to = _nodes.find(created); from != _nodes.end() && to != _nodes.end())
      {
        to->second->setPos(from->second->pos() + QPointF(COLUMN_SPACING, 0.0));
      }
    }
  }

  bool QuestChainDialog::save()
  {
    if (!saveChainChanges(_session, this, _model->changes()))
    {
      return false;
    }
    reloadModel();
    rebuildScene(false);
    QMessageBox::information(this, "Quest chain", "Chain saved.\n\nRestart the world server (mangosd) so it loads the "
                                                  "changes. The changed quests' copies in sql_exports/quests/ are updated.");
    return true;
  }

  void QuestChainDialog::updateState()
  {
    auto const pending = _model ? _model->changes().size() : 0;
    _save->setEnabled(pending > 0);
    _state->setText(pending ? QString("%1 quest(s) with unsaved link changes.").arg(pending)
                            : QString("%1 quest(s) shown.").arg(_shown.size()));
  }

  void QuestChainDialog::done(int result)
  {
    if (_model && _model->hasChanges())
    {
      auto const answer = QMessageBox::question(this, "Quest chain", "Save your link changes before closing?",
                                                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                                QMessageBox::Save);
      if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save()))
      {
        return;
      }
    }
    QDialog::done(result);
  }
}

#endif
