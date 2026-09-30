// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/quest/QuestEditorDialog.hpp>

#include <noggit/ui/content/ContentStyle.hpp>
#include <noggit/ui/quest/QuestEventsPage.hpp>
#include <noggit/ui/quest/QuestGiversPage.hpp>
#include <noggit/ui/quest/QuestObjectivesPage.hpp>
#include <noggit/ui/quest/QuestRequirementsPage.hpp>
#include <noggit/ui/quest/QuestRewardsPage.hpp>
#include <noggit/ui/quest/QuestStoryPage.hpp>

#include <QtGui/QPainter>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Quest
{
  using namespace Noggit::Ui::Content;
  namespace Q = Noggit::Quest;

  namespace
  {
    constexpr int SubtitleRole = Qt::UserRole;
    constexpr int ProblemRole = Qt::UserRole + 1;

    // Sidebar entry: the page title, a muted one-line description, and a red dot while it has problems.
    class SidebarDelegate : public QStyledItemDelegate
    {
    public:
      using QStyledItemDelegate::QStyledItemDelegate;

      // The background only: the text is drawn below, as a title and a subtitle.
      void initStyleOption(QStyleOptionViewItem* option, QModelIndex const& index) const override
      {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text.clear();
      }

      void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
      {
        QStyledItemDelegate::paint(painter, option, index);

        bool const selected = option.state & QStyle::State_Selected;
        QRect const area = option.rect.adjusted(14, 7, -12, -7);
        QColor const text = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor muted = text;
        muted.setAlphaF(0.65);
        painter->save();
        QFont title_font = option.font;
        title_font.setBold(true);
        painter->setFont(title_font);
        painter->setPen(text);
        QRect const title_rect(area.left(), area.top(), area.width() - 12, QFontMetrics(title_font).height());
        painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
        QFont small = option.font;
        small.setPointSizeF(small.pointSizeF() * 0.88);
        painter->setFont(small);
        painter->setPen(muted);
        painter->drawText(QRect(area.left(), title_rect.bottom() + 2, area.width(), QFontMetrics(small).height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(small).elidedText(index.data(SubtitleRole).toString(), Qt::ElideRight, area.width()));
        if (index.data(ProblemRole).toBool())
        {
          painter->setRenderHint(QPainter::Antialiasing);
          painter->setPen(Qt::NoPen);
          painter->setBrush(QColor(224, 96, 96));
          painter->drawEllipse(QPoint(area.right() - 2, title_rect.center().y()), 4, 4);
        }
        painter->restore();
      }

      QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const&) const override
      {
        return QSize(200, QFontMetrics(option.font).height() * 2 + 20);
      }
    };
  }

  QuestEditorDialog::QuestEditorDialog(QuestEditorSetup const& setup, QWidget* parent)
    : QDialog(parent)
    , _setup(setup)
  {
    setWindowTitle(setup.mode == EditorMode::Edit ? "Edit quest" : "New quest");
    resize(1040, 760);
    applyEditorStyle(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(10);

    _header = new QLabel(this);
    _header->setObjectName("ContentHeader");
    _subheader = new QLabel(this);
    _subheader->setObjectName("ContentSubheader");
    root->addWidget(_header);
    root->addWidget(_subheader);

    auto* body = new QHBoxLayout();
    body->setSpacing(12);
    _sidebar = new QListWidget(this);
    _sidebar->setObjectName("ContentSidebar");
    _sidebar->setItemDelegate(new SidebarDelegate(_sidebar));
    _sidebar->setFixedWidth(230);
    _sidebar->setFrameShape(QFrame::NoFrame);
    _stack = new QStackedWidget(this);
    body->addWidget(_sidebar);
    body->addWidget(_stack, 1);
    root->addLayout(body, 1);

    _story = new QuestStoryPage(setup);
    _rewards = new QuestRewardsPage(setup);
    _pages = {_story, new QuestRequirementsPage(setup), new QuestObjectivesPage(setup), _rewards,
              new QuestGiversPage(setup), new QuestEventsPage(setup)};

    Q::QuestContent const& stored = setup.data->content;
    for (auto* page : _pages)
    {
      page->load(stored);
      page->on_changed = [this] { refresh(); };
      auto* scroll = new QScrollArea(_stack);
      scroll->setWidgetResizable(true);
      scroll->setFrameShape(QFrame::NoFrame);
      scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      scroll->setWidget(page);
      _stack->addWidget(scroll);
      auto* item = new QListWidgetItem(page->title(), _sidebar);
      item->setData(SubtitleRole, page->subtitle());
    }
    connect(_sidebar, &QListWidget::currentRowChanged, _stack, &QStackedWidget::setCurrentIndex);
    _sidebar->setCurrentRow(0);

    auto* footer = new QHBoxLayout();
    _problem = problemLabel(this);
    footer->addWidget(_problem, 1);
    auto* buttons = new QDialogButtonBox(this);
    _ok = buttons->addButton(setup.mode == EditorMode::Edit ? "Save changes" : "Create quest", QDialogButtonBox::AcceptRole);
    _ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    footer->addWidget(buttons);
    root->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refresh();
  }

  void QuestEditorDialog::showPage(int index)
  {
    _sidebar->setCurrentRow(index);
  }

  void QuestEditorDialog::refresh()
  {
    QString const title = _story->questTitle();
    _header->setText(title.isEmpty() ? QString("Untitled quest") : title);
    QString const what = _setup.mode == EditorMode::Edit ? QString("Your quest") : QString("New quest");
    _subheader->setText(QString("%1  ·  ID %2  ·  level %3").arg(what).arg(_setup.entry).arg(_story->questLevel()));
    _rewards->setLevel(_story->questLevel());

    QStringList problems;
    for (int i = 0; i < static_cast<int>(_pages.size()); ++i)
    {
      auto const page_problems = _pages[i]->problems();
      _sidebar->item(i)->setData(ProblemRole, !page_problems.isEmpty());
      problems << page_problems;
    }
    _problem->setText(problems.isEmpty() ? QString() : problems.front()
                      + (problems.size() > 1 ? QString("  (+%1 more)").arg(problems.size() - 1) : QString()));
    _problem->setToolTip(problems.join("\n"));
    _problem->setVisible(!problems.isEmpty());
    _ok->setEnabled(problems.isEmpty());
  }

  void QuestEditorDialog::accept()
  {
    if (_setup.validate)
    {
      auto const problems = _setup.validate(content());
      if (!problems.isEmpty())
      {
        QMessageBox::warning(this, windowTitle(), "This cannot be saved yet:\n\n\u2022 " + problems.join("\n\u2022 "));
        return;
      }
    }
    QDialog::accept();
  }

  Q::QuestContent QuestEditorDialog::content() const
  {
    Q::QuestContent content = _setup.data->content;
    for (auto const* page : _pages)
    {
      page->store(content);
    }
    return content;
  }
}
