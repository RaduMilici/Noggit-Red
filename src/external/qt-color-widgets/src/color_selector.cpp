/**
 * \file
 *
 * \author Mattia Basaglia
 *
 * \copyright Copyright (C) 2013-2017 Mattia Basaglia
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "qt-color-widgets/color_selector.hpp"
#include "qt-color-widgets/color_dialog.hpp"
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QMimeData>

namespace color_widgets {

class ColorSelector::Private
{
public:
    UpdateMode update_mode;
    ColorDialog *dialog;
    QColor old_color;

    Private()
        : dialog(nullptr)
    {
    }
};

ColorSelector::ColorSelector(QWidget *parent) :
    ColorPreview(parent), p(new Private())
{
    setUpdateMode(Continuous);
    p->old_color = color();

    connect(this,&ColorPreview::clicked,this,&ColorSelector::showDialog);
    connect(this,SIGNAL(colorChanged(QColor)),this,SLOT(update_old_color(QColor)));

    setAcceptDrops(true);
}

ColorSelector::~ColorSelector()
{
    delete p;
}

ColorSelector::UpdateMode ColorSelector::updateMode() const
{
    return p->update_mode;
}

void ColorSelector::ensureDialog()
{
    if (p->dialog)
        return;

    p->dialog = new ColorDialog(this);
    p->dialog->setButtonMode(ColorDialog::OkCancel);

    connect(p->dialog, &QDialog::rejected, this, &ColorSelector::reject_dialog);
    connect(p->dialog, &ColorDialog::colorSelected, this, &ColorSelector::accept_dialog);
    connect(p->dialog, &ColorDialog::wheelFlagsChanged,
            this, &ColorSelector::wheelFlagsChanged);
}

void ColorSelector::setUpdateMode(UpdateMode m)
{
    p->update_mode = m;
}

Qt::WindowModality ColorSelector::dialogModality() const
{
    return p->dialog ? p->dialog->windowModality() : Qt::NonModal;
}

void ColorSelector::setDialogModality(Qt::WindowModality m)
{
    ensureDialog();
    p->dialog->setWindowModality(m);
}

ColorWheel::DisplayFlags ColorSelector::wheelFlags() const
{
    return p->dialog ? p->dialog->wheelFlags() : ColorWheel::DisplayFlags();
}

void ColorSelector::showDialog()
{
    ensureDialog();
    p->old_color = color();
    p->dialog->setColor(color());
    connect_dialog();
    p->dialog->show();
}

void ColorSelector::closeDialog()
{
  if (p->dialog)
    p->dialog->close();
}

void ColorSelector::setWheelFlags(ColorWheel::DisplayFlags flags)
{
    ensureDialog();
    p->dialog->setWheelFlags(flags);
}

void ColorSelector::connect_dialog()
{
    ensureDialog();
    if (p->update_mode == Continuous)
        connect(p->dialog, SIGNAL(colorChanged(QColor)), this, SLOT(setColor(QColor)), Qt::UniqueConnection);
    else
        disconnect_dialog();
}

void ColorSelector::disconnect_dialog()
{
    if (p->dialog)
        disconnect(p->dialog, SIGNAL(colorChanged(QColor)), this, SLOT(setColor(QColor)));
}

void ColorSelector::accept_dialog()
{
    setColor(p->dialog->color());
    p->old_color = color();
}

void ColorSelector::reject_dialog()
{
    setColor(p->old_color);
}

void ColorSelector::update_old_color(const QColor &c)
{
    if (!p->dialog || !p->dialog->isVisible())
        p->old_color = c;
}

void ColorSelector::dragEnterEvent(QDragEnterEvent *event)
{
    if ( event->mimeData()->hasColor() ||
         ( event->mimeData()->hasText() && QColor(event->mimeData()->text()).isValid() ) )
        event->acceptProposedAction();
}


void ColorSelector::dropEvent(QDropEvent *event)
{
    if ( event->mimeData()->hasColor() )
    {
        setColor(event->mimeData()->colorData().value<QColor>());
        event->accept();
    }
    else if ( event->mimeData()->hasText() )
    {
        QColor col(event->mimeData()->text());
        if ( col.isValid() )
        {
            setColor(col);
            event->accept();
        }
    }
}

} // namespace color_widgets
