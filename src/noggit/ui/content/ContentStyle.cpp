// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace Noggit::Ui::Content
{
  namespace
  {
    QString css(QColor const& color, double alpha = 1.0)
    {
      return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
    }

    // `color` moved towards white (dark themes) or black (light themes) by `amount`.
    QColor shade(QColor const& color, double amount)
    {
      bool const dark = color.lightnessF() < 0.5;
      QColor const target = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
      return QColor::fromRgbF(color.redF() + (target.redF() - color.redF()) * amount,
                              color.greenF() + (target.greenF() - color.greenF()) * amount,
                              color.blueF() + (target.blueF() - color.blueF()) * amount);
    }
  }

  void applyEditorStyle(QWidget* dialog)
  {
    // Noggit's themes are an application style sheet, which reaches the palette only once the widget is
    // polished: polish first, or the colours below come from Qt's default (light) palette.
    dialog->ensurePolished();
    QPalette const palette = dialog->palette();
    QColor const window = palette.color(QPalette::Window);
    QColor const text = palette.color(QPalette::WindowText);
    QColor const accent = palette.color(QPalette::Highlight);
    QColor const card = shade(window, 0.05);
    QColor const border = shade(window, 0.14);

    dialog->setStyleSheet(QString(R"(
      QFrame#ContentCard { background: %1; border: 1px solid %2; border-radius: 8px; }
      QFrame#ContentCard .QWidget, QFrame#ContentCard QLabel, QFrame#ContentCard QCheckBox,
      QFrame#ContentCard QRadioButton, QFrame#ContentRow .QWidget, QFrame#ContentRow QLabel,
      QFrame#ContentRow QCheckBox, QFrame#ContentRow QRadioButton { background: transparent; }
      QLabel#ContentCardTitle { color: %8; font-weight: 600; font-size: 10.5pt; padding-bottom: 2px; }
      QLabel#ContentHint { color: %3; }
      QLabel#ContentProblem { color: #e06060; font-weight: 600; }
      QLabel#ContentHeader { font-size: 14pt; font-weight: 600; }
      QLabel#ContentSubheader { color: %3; }
      QListWidget#ContentSidebar { border: none; background: transparent; outline: none; }
      QListWidget#ContentSidebar::item { padding: 9px 10px; margin: 1px 4px; border-radius: 6px; }
      QListWidget#ContentSidebar::item:selected { background: %4; color: %5; }
      QListWidget#ContentSidebar::item:hover:!selected { background: %6; }
      QPushButton#ContentAdd { border: 1px dashed %2; border-radius: 6px; padding: 6px 12px; text-align: left; }
      QPushButton#ContentAdd:hover { border-color: %4; }
      QToolButton#ContentRemove { border: none; font-size: 13pt; color: %3; padding: 0 4px; }
      QToolButton#ContentRemove:hover { color: #e06060; }
      QFrame#ContentRow { background: %7; border-radius: 6px; }
    )")
      .arg(css(card), css(border), css(text, 0.6), css(accent), css(palette.color(QPalette::HighlightedText)),
           css(border, 0.6), css(shade(window, 0.09)), css(text)));
  }

  Card::Card(QString const& title, QString const& description, QWidget* parent)
    : QFrame(parent)
  {
    setObjectName("ContentCard");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(6);
    _title = new QLabel(title, this);
    _title->setObjectName("ContentCardTitle");
    layout->addWidget(_title);
    if (!description.isEmpty())
    {
      layout->addWidget(hintLabel(description, this));
    }
    _body = new QVBoxLayout();
    _body->setSpacing(8);
    layout->addLayout(_body);
  }

  void Card::setTitle(QString const& title)
  {
    _title->setText(title);
  }

  QLabel* hintLabel(QString const& text, QWidget* parent)
  {
    auto* label = new QLabel(text, parent);
    label->setObjectName("ContentHint");
    label->setWordWrap(true);
    return label;
  }

  QLabel* problemLabel(QWidget* parent)
  {
    auto* label = new QLabel(parent);
    label->setObjectName("ContentProblem");
    label->setWordWrap(true);
    label->hide();
    return label;
  }

  QPushButton* addButton(QString const& text, QWidget* parent)
  {
    auto* button = new QPushButton(text, parent);
    button->setObjectName("ContentAdd");
    button->setCursor(Qt::PointingHandCursor);
    return button;
  }

  QToolButton* removeButton(QWidget* parent)
  {
    auto* button = new QToolButton(parent);
    button->setObjectName("ContentRemove");
    button->setText(QString::fromUtf8("\xc3\x97")); // ×
    button->setToolTip("Remove");
    button->setCursor(Qt::PointingHandCursor);
    return button;
  }

  QLabel* badge(QString const& text, QColor const& color, QWidget* parent)
  {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QString("background: %1; color: white; border-radius: 4px; padding: 3px 7px; "
                                 "font-weight: 600; font-size: 8pt;").arg(color.name()));
    label->setAlignment(Qt::AlignCenter);
    label->setFixedWidth(label->sizeHint().width() < 72 ? 72 : label->sizeHint().width());
    label->setFixedHeight(label->sizeHint().height());
    return label;
  }

  RowList::RowList(QWidget* parent)
    : QWidget(parent)
  {
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(0, 0, 0, 0);
    _layout->setSpacing(6);
  }

  void RowList::addRow(QWidget* row)
  {
    auto* wrapper = new QFrame(this);
    wrapper->setObjectName("ContentRow");
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(8, 6, 4, 6);
    row->setParent(wrapper);
    layout->addWidget(row, 1);
    auto* remove = removeButton(wrapper);
    layout->addWidget(remove, 0, Qt::AlignTop);
    _layout->addWidget(wrapper);
    _rows.emplace_back(wrapper, row);
    connect(remove, &QToolButton::clicked, this, [this, wrapper]
    {
      _rows.erase(std::remove_if(_rows.begin(), _rows.end(), [wrapper](auto const& r) { return r.first == wrapper; }),
                  _rows.end());
      wrapper->deleteLater();
      if (on_changed)
      {
        on_changed();
      }
    });
    if (on_changed)
    {
      on_changed();
    }
  }

  std::vector<QWidget*> RowList::rows() const
  {
    std::vector<QWidget*> out;
    for (auto const& [wrapper, row] : _rows)
    {
      out.push_back(row);
    }
    return out;
  }

  void RowList::clear()
  {
    for (auto const& [wrapper, row] : _rows)
    {
      delete wrapper;
    }
    _rows.clear();
    if (on_changed)
    {
      on_changed();
    }
  }
}
