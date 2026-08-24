// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/ui/tools/ActionHistoryNavigator/ActionHistoryNavigator.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/ZoneMusicPlayer.hpp>
#include <QSlider>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QFontDatabase>
#include <map>
#include <QLabel>
#include <QVBoxLayout>
#include <QPushButton>
#include <QtCore/QSettings>

using namespace Noggit::Ui;
using namespace Noggit::Ui::Tools::ViewToolbar::Ui;

namespace
{
  // Draws an icon-font glyph as a VECTOR path (crisp at any size -- no rasterize/downscale, so no
  // pixely/jittery edges), measured to its TIGHT bounds and scaled so its longest side fills a
  // fixed fraction of the button, centred. This makes glyphs from ANY font (FontAwesome vs the
  // noggit font, with very different metrics) render at a consistent size and position. The tint
  // follows the toolbar palette (blue when the toggle is active) via the QIcon state.
  class GlyphIconEngine : public QIconEngine
  {
  public:
    GlyphIconEngine(QString font_resource, ushort glyph, float fill, float dx, float dy, int px, int py)
      : _font_resource(std::move(font_resource)), _glyph(glyph), _fill(fill), _dx(dx), _dy(dy), _px(px), _py(py) {}

    void paint(QPainter* p, QRect const& rect, QIcon::Mode, QIcon::State state) override
    {
      QString const family = family_for(_font_resource);
      if (family.isEmpty()) return;

      float const box = static_cast<float>(std::min(rect.width(), rect.height()));
      if (box <= 0.f) return;

      // Measure the glyph's tight bounds at a reference size, then pick a pixel size so its HEIGHT
      // fills _fill of the button (matching the neighbouring noggit-font icons, which fill by
      // height) -- but clamp so a WIDE glyph (the cloud) can't exceed ~0.92 of the button width.
      QFont ref(family);
      ref.setPixelSize(256);
      QRect const rtb = QFontMetrics(ref).tightBoundingRect(QString(QChar(_glyph)));
      if (rtb.isEmpty()) return;
      float const by_h = _fill * box / static_cast<float>(rtb.height());
      float const by_w = 0.92f * box / static_cast<float>(rtb.width());
      int const px = std::max(1, static_cast<int>(256.f * std::min(by_h, by_w)));

      QFont font(family);
      font.setPixelSize(px);
      QFontMetrics const fm(font);
      QRect const tb = fm.tightBoundingRect(QString(QChar(_glyph)));

      FontNoggitButtonStyle style;
      style.ensurePolished();
      QColor const color = (state == QIcon::On)
        ? style.palette().color(QPalette::WindowText)
        : style.palette().color(QPalette::Disabled, QPalette::WindowText);

      // drawText positions the glyph by its baseline origin; offset so the TIGHT bbox centre lands
      // on the button centre, plus a per-icon nudge (fraction of the button) for glyphs whose tight
      // metrics don't perfectly reflect their visual centre.
      int const bx = rect.center().x() - (tb.x() + tb.width() / 2) + _px + static_cast<int>(_dx * box);
      int const by = rect.center().y() - (tb.y() + tb.height() / 2) + _py + static_cast<int>(_dy * box);
      p->setRenderHint(QPainter::Antialiasing, true);
      p->setRenderHint(QPainter::TextAntialiasing, true);
      p->setRenderHint(QPainter::SmoothPixmapTransform, true);
      p->setFont(font);
      p->setPen(color);
      p->drawText(QPoint(bx, by), QString(QChar(_glyph)));
    }

    QPixmap pixmap(QSize const& size, QIcon::Mode mode, QIcon::State state) override
    {
      QPixmap pm(size);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      paint(&p, QRect(QPoint(0, 0), size), mode, state);
      return pm;
    }

    QIconEngine* clone() const override { return new GlyphIconEngine(_font_resource, _glyph, _fill, _dx, _dy, _px, _py); }

  private:
    static QString family_for(QString const& resource)
    {
      static std::map<QString, QString> cache;
      auto it = cache.find(resource);
      if (it != cache.end()) return it->second;
      int const id = QFontDatabase::addApplicationFont(resource);
      QString family = id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()
        ? QFontDatabase::applicationFontFamilies(id).at(0) : QString();
      cache[resource] = family;
      return family;
    }

    QString _font_resource;
    ushort _glyph;
    float _fill;
    float _dx, _dy; // per-icon centre nudge, fraction of the button
    int _px, _py;   // per-icon centre nudge, absolute pixels
  };

  // fill ~0.72 of the button HEIGHT with the glyph's tight height -- matches the noggit-font icons
  QIcon noggit_glyph_icon(FontNoggit::Icons glyph, float fill = 0.72f, float dx = 0.f, float dy = 0.f, int px = 0, int py = 0)
  {
    return QIcon(new GlyphIconEngine(":/fonts/noggit_font.ttf", static_cast<ushort>(glyph), fill, dx, dy, px, py));
  }
  QIcon awesome_glyph_icon(FontAwesome::Icons glyph, float fill = 0.72f, float dx = 0.f, float dy = 0.f, int px = 0, int py = 0)
  {
    return QIcon(new GlyphIconEngine(":/fonts/font_awesome.otf", static_cast<ushort>(glyph), fill, dx, dy, px, py));
  }

  // Time-of-day slider that WRAPS: arrow key past 23:59 rolls over to 0:00 (and back), so the
  // day/night cycle keeps advancing instead of pinning at the end of the range.
  class WrapAroundSlider : public QSlider
  {
  public:
    using QSlider::QSlider;

  protected:
    void keyPressEvent(QKeyEvent* e) override
    {
      bool const fwd  = e->key() == Qt::Key_Right || e->key() == Qt::Key_Up;
      bool const back = e->key() == Qt::Key_Left  || e->key() == Qt::Key_Down;
      if (fwd && value() >= maximum())
      {
        setValue(minimum());
        return;
      }
      if (back && value() <= minimum())
      {
        setValue(maximum());
        return;
      }
      QSlider::keyPressEvent(e);
    }
  };
}

ViewToolbar::ViewToolbar(MapView* mapView)
  : _tool_group(this)
{
  setContextMenuPolicy(Qt::PreventContextMenu);
  setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);

  IconAction* climb_icon = new IconAction(FontNoggitIcon{FontNoggit::VISIBILITY_CLIMB });

  CheckBoxAction* climb_use_output_color_angle = new CheckBoxAction(tr("Display all angle color"));
  climb_use_output_color_angle->checkbox()->setChecked(false);
  connect(climb_use_output_color_angle->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
          {
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_use_output_angle = checked;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  CheckBoxAction* climb_use_smooth_interpolation = new CheckBoxAction(tr("Smooth"));
  climb_use_smooth_interpolation->setChecked(false);
  connect(climb_use_smooth_interpolation->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
          {
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_use_smooth_interpolation = checked;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  SliderAction* climb_value = new SliderAction(tr("Climb maximum value"), 0, 1570, 856, tr("degrees"),
      std::function<int(int v)>() = [&](int v) {
          float radian = float(v) / 1000.f;
          float degrees = radian * (180.0 / 3.141592653589793238463);
          return int(degrees);
      });

  connect(climb_value->slider(), &QSlider::valueChanged, [mapView](int value)
          {
              float radian = float(value) / 1000.0f;
              mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->climb_value = radian;
              mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
          });

  PushButtonAction* climb_reset_slider = new PushButtonAction(tr("Reset"));
  connect(climb_reset_slider->pushbutton(), &QPushButton::clicked, [climb_value]()
          {
              climb_value->slider()->setValue(856);
          });

  _climb_secondary_tool.push_back(climb_icon);
  _climb_secondary_tool.push_back(climb_use_smooth_interpolation);
  _climb_secondary_tool.push_back(climb_use_output_color_angle);
  _climb_secondary_tool.push_back(climb_value);
  _climb_secondary_tool.push_back(climb_reset_slider);


  // Time toolbar
  IconAction* time_icon = new IconAction(FontNoggitIcon{ FontNoggit::TIME_PAUSE });

  PushButtonAction* pause_time = new PushButtonAction(tr("Pause Time"));
  connect(pause_time->pushbutton(), &QPushButton::clicked, [pause_time]()
      {

      });

  PushButtonAction* speed_up_time = new PushButtonAction(tr("Increase Time speed"));
  connect(climb_reset_slider->pushbutton(), &QPushButton::clicked, [pause_time]()
      {

      });

  _time_secondary_tool.push_back(time_icon);
  /*
  ADD_ACTION(view_menu, "Increase time speed", Qt::Key_N, [this] { mTimespeed += 90.0f; });
  ADD_ACTION(view_menu, "Decrease time speed", Qt::Key_B, [this] { mTimespeed = std::max(0.0f, mTimespeed - 90.0f); });
  ADD_ACTION(view_menu, "Pause time", Qt::Key_J, [this] { mTimespeed = 0.0f; });
  */

}

ViewToolbar::ViewToolbar(MapView *mapView, ViewToolbar *tb)
    : _tool_group(this)
{
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);

    add_tool_icon(mapView, &mapView->_draw_models, tr("Doodads"), FontNoggit::VISIBILITY_DOODADS, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo, tr("WMOs"), FontNoggit::VISIBILITY_WMO, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo_doodads, tr("WMO doodads"), FontNoggit::VISIBILITY_WMO_DOODADS, tb);
    add_tool_icon(mapView, &mapView->_draw_wmo_exterior, tr("WMO exterior"), FontNoggit::UI_TOGGLE, tb);
    add_tool_icon(mapView, &mapView->_draw_terrain, tr("Terrain"), FontNoggit::VISIBILITY_TERRAIN, tb);
    add_tool_icon(mapView, &mapView->_draw_water, tr("Water"), FontNoggit::VISIBILITY_WATER, tb);
    // All three draw as vector glyph paths at a consistent tight-bounds size, centred, so they
    // match the noggit-font icons regardless of each font's own metrics (cloud is a wide glyph).
    add_tool_icon(mapView, &mapView->_draw_clouds, tr("Clouds"),
                  awesome_glyph_icon(FontAwesome::cloud, 0.72f, 0.f, 0.f, 1, 0), tb); // nudge 1px right
    add_tool_icon(mapView, &mapView->_draw_sun, tr("Sun"),
                  noggit_glyph_icon(FontNoggit::SUN, 0.72f, 0.05f, 0.05f), tb); // nudge right+down
    add_tool_icon(mapView, &mapView->_draw_moon, tr("Moon"),
                  awesome_glyph_icon(FontAwesome::moon, 0.72f, 0.f, 0.f, 1, 1), tb); // nudge 1px right, 1px down
    add_tool_icon(mapView, &mapView->_draw_bloom, tr("Bloom"), FontNoggit::VISIBILITY_LIGHT, tb);
    add_tool_icon(mapView, &mapView->_draw_ground_clutter, tr("Ground Clutter"), FontNoggit::VISIBILITY_GROUNDEFFECTS, tb);

    addSeparator();

    add_tool_icon(mapView, &mapView->_draw_lines, tr("Lines"), FontNoggit::VISIBILITY_LINES, tb);
    add_tool_icon(mapView, &mapView->_draw_hole_lines, tr("Hole lines"), FontNoggit::VISIBILITY_HOLE_LINES, tb);
    add_tool_icon(mapView, &mapView->_draw_wireframe, tr("Wireframe"), FontNoggit::VISIBILITY_WIREFRAME, tb);
    add_tool_icon(mapView, &mapView->_draw_contour, tr("Contours"), FontNoggit::VISIBILITY_CONTOURS, tb);
    add_tool_icon(mapView, &mapView->_draw_climb, tr("Climb"), FontNoggit::VISIBILITY_CLIMB, tb, tb->_climb_secondary_tool);
    add_tool_icon(mapView, &mapView->_draw_vertex_color, tr("Vertex Color"), FontNoggit::VISIBILITY_VERTEX_PAINTER, tb);
    add_tool_icon(mapView, &mapView->_draw_baked_shadows, tr("Baked Shadows"), FontNoggit::VISIBILITY_BAKED_SHADOWS, tb); // TODO : better icon

    addSeparator();

    add_tool_icon(mapView, &mapView->_draw_model_animations, tr("Animations"), FontNoggit::VISIBILITY_ANIMATION, tb);
    add_tool_icon(mapView, &mapView->_draw_fog, tr("Fog"), FontNoggit::VISIBILITY_FOG, tb);
    add_tool_icon(mapView, &mapView->_draw_mfbo, tr("Flight bounds"), FontNoggit::VISIBILITY_FLIGHT_BOUNDS, tb);
    // add_tool_icon(mapView, &mapView->_draw_lights_zones, tr("Light zones"), FontNoggit::VISIBILITY_LIGHT, tb);
    addSeparator();

    // Hole lines always on
    add_tool_icon(mapView, &mapView->_draw_models_with_box, tr("Models with box"), FontNoggit::VISIBILITY_WITH_BOX, tb);
    add_tool_icon(mapView, &mapView->_draw_hidden_models, tr("Hidden models"), FontNoggit::VISIBILITY_HIDDEN_MODELS, tb);
    addSeparator();
    /*
    auto tablet_sensitivity = new QSlider(this);
    tablet_sensitivity->setOrientation(Qt::Horizontal);
    addWidget(tablet_sensitivity);
   */

    // some unused icons : 
    // VISIBILITY_LIGHT VISIBILITY_GROUNDEFFECTS CAMERA_TURN CAMERA_SPEED_FASTER.. INFO TIME_NORMAL VIEW_AXIS VISIBILITY_UNUSED SETTINGS

    // normal view mode icon, and make them only 1 at a time out of the 3 view modes? 
    // add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Normal view"), FontNoggit::VIEW_AXIS, tb);
    add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Game view"), FontNoggit::VIEW_MODE_GAME, tb);
    // add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Tile view"), FontNoggit::VIEW_MODE_2D, tb);
    addSeparator();

    add_tool_icon(mapView, &mapView->_show_minimap_window, tr("Show Minimap"),FontNoggit::TOOL_MINIMAP_EDITOR, tb);
    add_tool_icon(mapView, &mapView->_show_detail_info_window, tr("Details info"), FontNoggit::INFO, tb);

    // TODO : will open a panel with time controls, or use 2n toolbar
    //add_tool_icon(mapView, &mapView->_game_mode_camera, tr("Time speed"), FontNoggit::TIME_NORMAL, tb, _time_secondary_tool);

    /*
    auto tile_view_btn = new QPushButton(this);
    tile_view_btn->setIcon(FontNoggitIcon{ FontNoggit::VIEW_MODE_2D });
    tile_view_btn->setToolTip("2D View");
    addWidget(tile_view_btn);
    */

    // Time-of-day control moved to the Warcraft-3 time-of-day globe (Noggit::Ui::TimeGlobeWidget),
    // floated at the top-centre of the viewport by MapView. Click it to scrub World::time; it shows the
    // day/night cycle + the 8-stud loading bar. The old clock button + slider that lived here are gone.

    // Zone music controls: a dropdown showing the current zone's playlist (click a song to play it,
    // the playing track is highlighted) plus a volume slider. The Zone-music toggle button above
    // enables/disables automatic playback as you cross zone boundaries.
    auto music_btn = new QPushButton(this);
    music_btn->setIcon(FontAwesomeIcon(FontAwesome::music));
    music_btn->setToolTip("Zone music controls");
    addWidget(music_btn);

    connect(music_btn, &QPushButton::clicked,
            [this, music_btn, mapView]()
            {
                auto* panel = mapView->_zone_music_player;
                if (!panel)
                {
                    return;
                }
                QPoint new_pos = mapToGlobal(QPoint(music_btn->pos().x(), music_btn->pos().y() + 30));
                auto const hint = panel->sizeHint();
                panel->setGeometry(new_pos.x(), new_pos.y(),
                                   std::max(hint.width(), 280), std::max(hint.height(), 220));
                panel->setWindowFlags(Qt::Popup);
                panel->show();
            });

    auto undo_stack_btn = new QPushButton(this);
    undo_stack_btn->setIcon(FontAwesomeIcon(FontAwesome::undo));
    undo_stack_btn->setToolTip("History");
    addWidget(undo_stack_btn);


    auto undo_stack_popup = new QWidget(this);
    undo_stack_popup->setMinimumWidth(160);
    undo_stack_popup->setMinimumHeight(300);
    auto layout = new QVBoxLayout(undo_stack_popup);
    auto action_navigator = new Noggit::Ui::Tools::ActionHistoryNavigator(undo_stack_popup);
    action_navigator->setMinimumWidth(160);
    action_navigator->setMinimumHeight(300);
    layout->addWidget(undo_stack_popup);

    undo_stack_popup->updateGeometry();
    undo_stack_popup->adjustSize();
    undo_stack_popup->update();
    undo_stack_popup->repaint();
    undo_stack_popup->setVisible(false);

    connect(undo_stack_btn, &QPushButton::clicked,
            [=]()
            {
                QPoint new_pos = mapToGlobal(
                    QPoint(undo_stack_btn->pos().x(),
                           undo_stack_btn->pos().y() + 30));

                undo_stack_popup->setGeometry(new_pos.x(),
                                              new_pos.y(),
                                              undo_stack_popup->width(),
                                              undo_stack_popup->height());

                undo_stack_popup->setWindowFlags(Qt::Popup);
                undo_stack_popup->show();
            });
}

ViewToolbar::ViewToolbar(MapView* mapView, editing_mode mode)
    : _tool_group(this)
    , current_mode(mode)
{
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Minimum);
    setOrientation(Qt::Vertical);
    mapView->getLeftSecondaryToolbar()->hide();

    {
        /*
         * FLATTEN/BLUE SECONDARY TOOL 
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_FLATTEN_BLUR });

            CheckBoxAction* _raise = new CheckBoxAction(tr("Raise"), true);
            connect(_raise->checkbox(), &QCheckBox::stateChanged, [mapView](int state)
                {
                    mapView->getFlattenTool()->_flatten_mode.raise = state;
                });

            CheckBoxAction* _lower = new CheckBoxAction(tr("Lower"), true);
            connect(_lower->checkbox(), &QCheckBox::stateChanged, [mapView](int state)
                {
                    mapView->getFlattenTool()->_flatten_mode.lower = state;
                });


            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_raise); raise_index = 1;
            _toolbar->ADD_ACTION(_lower); lower_index = 2;
            _toolbar->SETUP_WIDGET(false);
        }

        _flatten_secondary_tool.push_back(_toolbar);
    }

    {
        /*
         * TEXTURE PAINTER SECONDARY TOOL
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_TEXTURE_PAINT });

            CheckBoxAction* _unpaintable_chunk = new CheckBoxAction(tr("Unpaintable chunk"));
            connect(_unpaintable_chunk->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
                    {
                        mapView->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = checked;
                        mapView->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
                    });

            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_unpaintable_chunk); unpaintable_chunk_index = 1;
            _toolbar->SETUP_WIDGET(false);
        }

        _texture_secondary_tool.push_back(_toolbar);
    }

    {
        /*
         * OBJECT SECONDARY TOOL 
         */

        SubToolBarAction* _up_toolbar = new SubToolBarAction();

        {
            QVector<QWidgetAction*> _up_temp;

            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_OBJECT_EDITOR });
            CheckBoxAction* _rotate_follow_cursor = new CheckBoxAction(tr("Rotate following cursor"), true);
            CheckBoxAction* _smooth_follow_rotation = new CheckBoxAction(tr("Smooth follow rotation"), true);
            CheckBoxAction* _random_all_on_rotation = new CheckBoxAction(tr("Random Rotation/Tilt/Scale on Rotation"));
            CheckBoxAction* _magnetic_to_ground = new CheckBoxAction(tr("Magnetic to ground when dragging"));

            _up_toolbar->ADD_ACTION(_icon);
            _up_toolbar->ADD_ACTION(_rotate_follow_cursor);
            _up_toolbar->ADD_ACTION(_smooth_follow_rotation);
            _up_toolbar->ADD_ACTION(_random_all_on_rotation);
            _up_toolbar->ADD_ACTION(_magnetic_to_ground);
            _up_toolbar->SETUP_WIDGET(false);
        }

        SubToolBarAction* _down_toolbar = new SubToolBarAction();

        {
            QVector<QWidgetAction*> _down_temp;

            CheckBoxAction* _magnetic_to_ground = new CheckBoxAction(tr("Magnetic to ground when dragging"));
            CheckBoxAction* _rotation_around_pivot = new CheckBoxAction(tr("Rotate around pivot"), true);

            _down_toolbar->ADD_ACTION(_magnetic_to_ground);
            _down_toolbar->ADD_ACTION(_rotation_around_pivot);
            _down_toolbar->SETUP_WIDGET(true);
        }


        _object_secondary_tool.push_back(_up_toolbar);
        _object_secondary_tool.push_back(_down_toolbar);
    }

    {
        /*
         * LIGHT SECONDARY TOOL 
         */

        SubToolBarAction* _toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_STAMP });
            CheckBoxAction* _draw_only_inside = new CheckBoxAction(tr("Draw current only"));
            CheckBoxAction* _draw_wireframe = new CheckBoxAction(tr("Draw wireframe"));
            SliderAction* _alpha_value = new SliderAction(tr("Alpha"), 0, 100, 30, "",
                std::function<float(float v)>() = [&](float v) {
                    return v / 100.f;
                });

            _toolbar->ADD_ACTION(_icon);
            _toolbar->ADD_ACTION(_draw_only_inside); sphere_light_inside_index = 1;
            _toolbar->ADD_ACTION(_draw_wireframe); sphere_light_wireframe_index = 2;
            _toolbar->ADD_ACTION(_alpha_value); sphere_light_alpha_index = 3;
            _toolbar->SETUP_WIDGET(false);
        }

        _light_secondary_tool.push_back(_toolbar);
    }

    {
        /*
         * CREATURE SECONDARY TOOL
         */

        SubToolBarAction* _up_toolbar = new SubToolBarAction();
        SubToolBarAction* _down_toolbar = new SubToolBarAction();

        {
            IconAction* _icon = new IconAction(FontNoggitIcon{ FontNoggit::TOOL_OBJECT_EDITOR });

            CheckBoxAction* _patrol_paths = new CheckBoxAction(tr("Patrol paths"));
            connect(_patrol_paths->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
                    {
                        mapView->getWorld()->setDrawCreaturePatrolPaths(checked);
                        mapView->requestRedraw();
                    });

            _up_toolbar->ADD_ACTION(_icon);
            _up_toolbar->ADD_ACTION(_patrol_paths);
            _up_toolbar->SETUP_WIDGET(false);
        }

        {
            // "Quick Facts" window for the selected creature (stats + spell/aura icons), toggled
            // like the patrol-paths checkbox; the window itself is free-floating/movable.
            CheckBoxAction* _creature_info = new CheckBoxAction(tr("Creature info"));
            connect(_creature_info->checkbox(), &QCheckBox::toggled, [mapView](bool checked)
                    {
                        mapView->setCreatureInfoPanelVisible(checked);
                    });

            _down_toolbar->ADD_ACTION(_creature_info);
            _down_toolbar->SETUP_WIDGET(true);
        }

        _creature_secondary_tool.push_back(_up_toolbar);
        _creature_secondary_tool.push_back(_down_toolbar);
    }
}

void ViewToolbar::setCurrentMode(MapView* mapView, editing_mode mode)
{
    mapView->getLeftSecondaryToolbar()->hide();
    current_mode = mode;

    QSettings settings;
    bool use_classic_ui = settings.value("classicUI", false).toBool();

    switch (current_mode)
    {
    case editing_mode::ground:
        break;
    case editing_mode::flatten_blur:
        if (_flatten_secondary_tool.size() > 0)
        {
            setupWidget(_flatten_secondary_tool);
            if (!use_classic_ui)
                mapView->getLeftSecondaryToolbar()->show();
            else
                mapView->getLeftSecondaryToolbar()->hide();
        }
        break;
    case editing_mode::paint:
        if (_texture_secondary_tool.size() > 0)
        {
            setupWidget(_texture_secondary_tool);
            if (!use_classic_ui)
                mapView->getLeftSecondaryToolbar()->show();
            else
                mapView->getLeftSecondaryToolbar()->hide();
        }
        break;
    case editing_mode::object:
        if (_object_secondary_tool.size() > 0)
        {
            //setupWidget(_object_secondary_tool, true);
            //mapView->getLeftSecondaryToolbar()->show();
        }
        break;
    case editing_mode::light:
        if (_light_secondary_tool.size() > 0)
        {
            setupWidget(_light_secondary_tool, true);
            mapView->getLeftSecondaryToolbar()->show();
        }
        break;
    case editing_mode::creature:
        if (_creature_secondary_tool.size() > 0)
        {
            setupWidget(_creature_secondary_tool);
            mapView->getLeftSecondaryToolbar()->show();
        }
        break;
    default:
        break;
    }
}

void ViewToolbar::add_tool_icon(MapView* mapView,
                                Noggit::BoolToggleProperty* view_state,
                                const QString& name,
                                const FontNoggit::Icons& icon,
                                ViewToolbar* sec_tool_bar,
                                QVector<QWidgetAction*> sec_action_bar)
{
  add_tool_icon(mapView, view_state, name, QIcon(FontNoggitIcon{icon}), sec_tool_bar, sec_action_bar);
}

void ViewToolbar::add_tool_icon(MapView* mapView,
                                Noggit::BoolToggleProperty* view_state,
                                const QString& name,
                                QIcon const& icon,
                                ViewToolbar* sec_tool_bar,
                                QVector<QWidgetAction*> sec_action_bar)
{
    auto action = addAction(icon, name);
    connect (action, &QAction::triggered, [action, view_state] () {
        action->setChecked(!view_state->get());
        view_state->set(!view_state->get());
    });

    connect (action, &QAction::hovered, [mapView, sec_tool_bar, sec_action_bar] () {
        sec_tool_bar->clear();
        mapView->getSecondaryToolBar()->hide();

        if (sec_action_bar.size() > 0)
        {
            sec_tool_bar->setupWidget(sec_action_bar);
            mapView->getSecondaryToolBar()->show();
        }
    });

    connect (view_state, &Noggit::BoolToggleProperty::changed, [action, view_state, mapView] () {
        if (action->text() == "Game view" && view_state->get())
        {
            // enter IN PLACE: no snap to the surface below -- the game tick's gravity takes over
            // and the character falls from the current camera height (or starts swimming if the
            // camera is underwater)
            mapView->setCameraDirty();
            mapView->enterGameModeInPlace();
        }


        action->setChecked(view_state->get());
    });

    action->setCheckable(true);
    action->setChecked(view_state->get());
}

void ViewToolbar::setupWidget(QVector<QWidgetAction *> _to_setup, bool ignoreSeparator)
{
    clear();
    for (int i = 0; i < _to_setup.size(); ++i)
    {
        addAction(_to_setup[i]);
        (i == _to_setup.size() - 1) ? NULL : (ignoreSeparator) ? NULL : addSeparator();
    }
}

bool ViewToolbar::showUnpaintableChunk()
{
    return static_cast<SubToolBarAction*>(_texture_secondary_tool[0])->GET<CheckBoxAction*>(unpaintable_chunk_index)->checkbox()->isChecked() && current_mode == editing_mode::paint;
}

void ViewToolbar::nextFlattenMode(MapView* mapView)
{
    mapView->getFlattenTool()->_flatten_mode.next();

    CheckBoxAction* _raise_option = static_cast<SubToolBarAction*>(_flatten_secondary_tool[0])->GET<CheckBoxAction*>(raise_index);
    CheckBoxAction* _lower_option = static_cast<SubToolBarAction*>(_flatten_secondary_tool[0])->GET<CheckBoxAction*>(lower_index);

    QSignalBlocker const raise_lock(_raise_option);
    QSignalBlocker const lower_lock(_lower_option);

    _raise_option->setChecked(true);
    _lower_option->setChecked(true);
}

bool ViewToolbar::drawOnlyInsideSphereLight()
{
    return static_cast<SubToolBarAction*>(_light_secondary_tool[0])->GET<CheckBoxAction*>(sphere_light_inside_index)->checkbox()->isChecked() && current_mode == editing_mode::light;
}

bool ViewToolbar::drawWireframeSphereLight()
{
    return static_cast<SubToolBarAction*>(_light_secondary_tool[0])->GET<CheckBoxAction*>(sphere_light_wireframe_index)->checkbox()->isChecked() && current_mode == editing_mode::light;
}

float ViewToolbar::getAlphaSphereLight()
{
    auto toolbar = static_cast<SubToolBarAction*>(_light_secondary_tool[0]);
    auto slider = toolbar->GET<SliderAction*>(sphere_light_alpha_index)->slider();

    return float(slider->value()) / 100.f;
}