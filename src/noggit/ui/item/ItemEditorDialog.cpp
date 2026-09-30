// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/item/ItemEditorDialog.hpp>

#include <noggit/ui/content/ContentStyle.hpp>

#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace Noggit::Ui::Item
{
  using namespace Noggit::Ui::Content;
  namespace I = Noggit::Item;

  ItemEditorDialog::ItemEditorDialog(ItemEditorSetup const& setup, QWidget* parent)
    : QDialog(parent)
    , _setup(setup)
  {
    setWindowTitle(setup.editing ? "Edit item" : "New item");
    resize(760, 480);
    applyEditorStyle(this);
    auto const& schema = setup.data->schema;
    auto const& values = setup.data->values;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    auto* header = new QLabel(setup.editing ? "Edit item" : "New item", this);
    header->setObjectName("ContentHeader");
    root->addWidget(header);
    auto* subheader = new QLabel(QString("%1  ·  ID %2").arg(setup.editing ? "Your item" : "A new item").arg(setup.entry), this);
    subheader->setObjectName("ContentSubheader");
    root->addWidget(subheader);

    auto* body = new QHBoxLayout();
    body->setSpacing(14);
    root->addLayout(body, 1);

    // --- the tooltip preview ---
    auto* preview = new Card("Preview", {}, this);
    preview->setFixedWidth(260);
    auto* tooltip = new QFrame(preview);
    tooltip->setStyleSheet("QFrame { background: rgba(8, 12, 30, 235); border: 1px solid rgb(120, 120, 140); border-radius: 6px; }"
                           "QLabel { border: none; background: transparent; }");
    auto* tooltip_layout = new QVBoxLayout(tooltip);
    tooltip_layout->setContentsMargins(10, 8, 10, 10);
    auto* top = new QHBoxLayout();
    _icon = new QLabel(tooltip);
    _icon->setFixedSize(44, 44);
    _icon->setStyleSheet("border: 1px solid rgb(90, 90, 100); border-radius: 4px;");
    _icon->setAlignment(Qt::AlignCenter);
    _preview_name = new QLabel(tooltip);
    _preview_name->setWordWrap(true);
    QFont name_font = _preview_name->font();
    name_font.setBold(true);
    name_font.setPointSizeF(name_font.pointSizeF() * 1.1);
    _preview_name->setFont(name_font);
    top->addWidget(_icon);
    top->addWidget(_preview_name, 1);
    tooltip_layout->addLayout(top);
    _preview_kind = new QLabel(tooltip);
    _preview_kind->setStyleSheet("color: white;");
    tooltip_layout->addWidget(_preview_kind);
    _preview_description = new QLabel(tooltip);
    _preview_description->setWordWrap(true);
    _preview_description->setStyleSheet("color: rgb(255, 209, 0);");
    tooltip_layout->addWidget(_preview_description);
    preview->body()->addWidget(tooltip);
    preview->body()->addStretch();
    body->addWidget(preview);

    // --- the fields ---
    auto* details = new Card("Item", {}, this);
    auto* form = new QFormLayout();
    details->body()->addLayout(form);
    _name = new QLineEdit(details);
    _name->setPlaceholderText("e.g. Wolf Pelt");
    _name->setMaxLength(120);
    form->addRow("Name", _name);

    _kind = new QComboBox(details);
    for (auto const& kind : I::itemKinds())
    {
      _kind->addItem(kind.label);
      _kind->setItemData(_kind->count() - 1, kind.tooltip, Qt::ToolTipRole);
    }
    _kind->setEnabled(!schema.class_col.empty());
    form->addRow("Kind", _kind);

    _quality = new QComboBox(details);
    for (auto const& quality : I::qualities())
    {
      _quality->addItem(quality.label, quality.id);
      if (auto const color = qualityColor(quality.id); color.isValid())
      {
        _quality->setItemData(_quality->count() - 1, color, Qt::ForegroundRole);
      }
    }
    _quality->setEnabled(!schema.quality_col.empty());
    form->addRow("Quality", _quality);

    _look = new EntryPicker(setup.lookups->items.get(), details);
    _look->setEnabled(!schema.display_col.empty());
    _look->setToolTip("Pick an item that looks right: the new item uses its icon and model.");
    form->addRow("Looks like", _look);

    _description = new QPlainTextEdit(details);
    _description->setPlaceholderText("Flavour text, shown in yellow (optional).");
    _description->setMaximumHeight(_description->fontMetrics().lineSpacing() * 3 + 14);
    _description->setTabChangesFocus(true);
    _description->setEnabled(!schema.description_col.empty());
    form->addRow("Description", _description);

    _stack = new QSpinBox(details);
    _stack->setRange(1, 250);
    _stack->setToolTip("How many fit in one bag slot.");
    _stack->setEnabled(!schema.stackable_col.empty());
    form->addRow("Stacks up to", _stack);

    _price_row = new QWidget(details);
    auto* price = new QHBoxLayout(_price_row);
    price->setContentsMargins(0, 0, 0, 0);
    _gold = new QSpinBox(_price_row);
    _gold->setRange(0, 9999);
    _gold->setSuffix(" g");
    _silver = new QSpinBox(_price_row);
    _silver->setRange(0, 99);
    _silver->setSuffix(" s");
    _copper = new QSpinBox(_price_row);
    _copper->setRange(0, 99);
    _copper->setSuffix(" c");
    for (auto* spin : {_gold, _silver, _copper})
    {
      spin->setEnabled(!schema.sell_price_col.empty());
      price->addWidget(spin);
    }
    price->addStretch();
    form->addRow("Vendors pay", _price_row);
    details->body()->addStretch();
    body->addWidget(details, 1);

    // --- values ---
    _name->setText(setup.editing ? QString::fromStdString(values.name.value_or(std::string())) : setup.suggested_name);
    int kind = 0;
    if (setup.editing)
    {
      kind = -1;
      auto const& kinds = I::itemKinds();
      for (std::size_t i = 0; i < kinds.size(); ++i)
      {
        if (kinds[i].item_class == values.item_class.value_or(0) && kinds[i].subclass == values.subclass.value_or(0))
        {
          kind = static_cast<int>(i);
        }
      }
      if (kind < 0)
      {
        _kind->addItem("Other (kept as it is)");
        kind = _kind->count() - 1;
      }
    }
    _kind->setCurrentIndex(kind);
    _quality->setCurrentIndex(std::max(0, _quality->findData(values.quality.value_or(1))));
    _description->setPlainText(QString::fromStdString(values.description.value_or(std::string())));
    _stack->setValue(std::max<int>(values.stackable.value_or(20), 1));
    std::uint32_t const sell = values.sell_price.value_or(0);
    _gold->setValue(static_cast<int>(sell / 10000));
    _silver->setValue(static_cast<int>((sell / 100) % 100));
    _copper->setValue(static_cast<int>(sell % 100));

    root->addSpacing(6);
    auto* footer = new QHBoxLayout();
    if (setup.editing)
    {
      auto* remove = new QPushButton("Delete item...", this);
      remove->setEnabled(Noggit::Content::isCustom(setup.entry));
      footer->addWidget(remove);
      connect(remove, &QPushButton::clicked, this, [this]
      {
        _delete_requested = true;
        accept();
      });
    }
    _problem = problemLabel(this);
    footer->addWidget(_problem, 1);
    auto* buttons = new QDialogButtonBox(this);
    _ok = buttons->addButton(setup.editing ? "Save changes" : "Create item", QDialogButtonBox::AcceptRole);
    _ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    footer->addWidget(buttons);
    root->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(_name, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(_kind, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    connect(_quality, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    connect(_look, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    connect(_description, &QPlainTextEdit::textChanged, this, [this] { refresh(); });
    refresh();
  }

  std::uint32_t ItemEditorDialog::displayId() const
  {
    if (auto const look = _look->entry())
    {
      auto* model = _setup.lookups->items.get();
      int const row = model->rowOf(look);
      return row > 0 ? model->item(row)->data(LookupModel::DisplayRole).toUInt() : 0;
    }
    return _setup.data->values.display_id.value_or(0);
  }

  void ItemEditorDialog::refresh()
  {
    QString const name = _name->text().trimmed();
    auto const quality = _quality->currentData().toUInt();
    QColor const color = qualityColor(quality).isValid() ? qualityColor(quality) : QColor(Qt::white);
    _preview_name->setText(name.isEmpty() ? QString("Unnamed item") : name);
    _preview_name->setStyleSheet(QString("color: %1;").arg(color.name()));
    bool const quest_item = _kind->currentIndex() == 0;
    _preview_kind->setText(quest_item ? "Quest Item" : QString());
    _preview_kind->setVisible(quest_item);
    QString const description = _description->toPlainText().trimmed();
    _preview_description->setText(description.isEmpty() ? QString() : "\"" + description + "\"");
    _preview_description->setVisible(!description.isEmpty());
    auto const display = displayId();
    auto const& icon_for = _setup.lookups->items->icon_for;
    QIcon const icon = display && icon_for ? icon_for(display) : QIcon();
    _icon->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(40, 40));
    _icon->setText(icon.isNull() ? QString("?") : QString());
    // Quest items cannot be sold.
    _price_row->setEnabled(!quest_item);

    QString problem;
    if (name.isEmpty())
    {
      problem = "The item needs a name.";
    }
    else if (!_setup.editing && !display && _look->isEnabled())
    {
      problem = "Pick an item it looks like (its icon).";
    }
    _problem->setText(problem);
    _problem->setVisible(!problem.isEmpty());
    _ok->setEnabled(problem.isEmpty());
  }

  I::ItemFields ItemEditorDialog::fields() const
  {
    auto const& schema = _setup.data->schema;
    I::ItemFields fields;
    fields.name = _name->text().trimmed().toStdString();
    if (_description->isEnabled()) fields.description = _description->toPlainText().trimmed().toStdString();
    if (_look->isEnabled() && (_look->entry() || !_setup.editing)) fields.display_id = displayId();
    if (_quality->isEnabled()) fields.quality = _quality->currentData().toUInt();
    auto const& kinds = I::itemKinds();
    if (_kind->isEnabled() && _kind->currentIndex() < static_cast<int>(kinds.size()))
    {
      auto const& kind = kinds[static_cast<std::size_t>(_kind->currentIndex())];
      fields.item_class = kind.item_class;
      fields.subclass = kind.subclass;
      if (!schema.bonding_col.empty()) fields.bonding = kind.bonding;
    }
    if (_stack->isEnabled()) fields.stackable = static_cast<std::uint32_t>(_stack->value());
    if (!schema.sell_price_col.empty())
    {
      bool const quest_item = _kind->currentIndex() == 0;
      fields.sell_price = quest_item ? 0u : static_cast<std::uint32_t>(_gold->value() * 10000 + _silver->value() * 100 + _copper->value());
    }
    return fields;
  }
}
