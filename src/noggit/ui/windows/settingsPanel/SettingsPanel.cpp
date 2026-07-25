// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>
#include <noggit/Log.h>

#include <noggit/TextureManager.h>
#include <util/qt/overload.hpp>
#include <noggit/ui/FramelessWindow.hpp>
#include <noggit/MySqlSettings.hpp>
#include <sstream>

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QSlider>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QVBoxLayout>
#include <QDir>
#include <QApplication>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>
#endif

#include <ui_SettingsPanel.h>
#include <ui_TitleBar.h>


#include <algorithm>
#include <qmessagebox.h>


namespace Noggit
{
  namespace Ui
  {
    settings::settings(QWidget *parent) : QMainWindow(parent, Qt::Window), _settings(new QSettings(this))
    {
      auto body = new QWidget(this);
      ui = new ::Ui::SettingsPanel;
      ui->setupUi(body);
      setCentralWidget(body);
      setWindowTitle("Settings");

      auto titlebar = new QWidget(this);
      setupFramelessWindow(titlebar, this, minimumSize(), maximumSize(), false);
      setMenuWidget(titlebar);

      setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowStaysOnTopHint);

      connect(ui->importPathField, &QLineEdit::textChanged, [&](QString value)
              {
                _settings->setValue("project/import_file", value);
              }
      );


      connect(ui->importPathField_browse, &QPushButton::clicked, [=]
              {
                auto result(QFileDialog::getOpenFileName(
                    nullptr, "Import File Path", ui->importPathField->text()));

                if (!result.isNull())
                {
                  ui->importPathField->setText(result);
                }
              }
      );

      connect(ui->wmvLogPathField, &QLineEdit::textChanged, [&](QString value)
              {
                _settings->setValue("project/import_file", value);
              }
      );


      connect(ui->wmvLogPathField_browse, &QPushButton::clicked, [=]
              {
                auto result(QFileDialog::getOpenFileName(
                    nullptr, "WMV Log Path", ui->wmvLogPathField->text()));

                if (!result.isNull())
                {
                  ui->wmvLogPathField->setText(result);
                }
              }
      );


#ifdef USE_MYSQL_UID_STORAGE
      ui->MySQL_box->setEnabled(true);
      ui->MySQL_box->setCheckable(true);
      ui->mysql_warning->setVisible(false);
#endif

      ui->_theme->addItem("System");

      QDir theme_dir = QDir("./themes/");
      if (theme_dir.exists())
      {
        for (auto dir : theme_dir.entryList(QDir::AllDirs | QDir::NoDotAndDotDot))
        {
          if (QDir(theme_dir.path() + "/" + dir).exists("theme.qss"))
          {
            ui->_theme->addItem(dir);
          }
        }
      }
      else
      {
        LogError
            << "Failed to load themes. The \"themes/\" folder does not exist in Noggit directory. Using system theme."
            << std::endl;
      }

      connect(ui->_theme, &QComboBox::currentTextChanged, [&](QString s)
              {
                if (s == "System")
                {
                  qApp->setStyleSheet("");
                  return;
                }

                auto sstream = std::stringstream();
                sstream << "./themes/" << s.toStdString() << "/theme.qss";

                QFile file(sstream.str().c_str());
                if (file.open(QFile::ReadOnly))
                {
                  QString style_sheet = QLatin1String(file.readAll());
                  QString style_sheet_fixed = style_sheet.replace("@rpath", QCoreApplication::applicationDirPath());

                  if (style_sheet_fixed.endsWith("/"))
                    style_sheet_fixed.chop(1);
                  else if (style_sheet_fixed.endsWith("\\"))
                    style_sheet_fixed.chop(2);

                  qApp->setStyleSheet(style_sheet_fixed);
                }
              }
      );

      // Time-of-day globe skin (Appearance tab): which Warcraft-3 race frame the top-centre time globe
      // wears. Applied live -- the globe re-reads theme/race about once a second.
      {
        auto* globeBox = new QGroupBox("Time-of-day globe", ui->tab_6);
        globeBox->setAlignment(Qt::AlignCenter);
        auto* gv = new QVBoxLayout(globeBox);
        auto* row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(new QLabel("Skin"));
        auto* race = new QComboBox();
        race->addItem("Human", "human");
        race->addItem("Orc", "orc");
        race->addItem("Night Elf", "nightelf");
        race->addItem("Undead", "undead");
        race->setMinimumWidth(200);
        race->setCurrentIndex(std::max(0, race->findData(_settings->value("theme/race", "nightelf").toString())));
        connect(race, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, race](int idx)
        {
          _settings->setValue("theme/race", race->itemData(idx).toString());
          _settings->sync();
        });
        row->addWidget(race);
        row->addStretch(1);
        gv->addLayout(row);
        ui->verticalLayout_23->addWidget(globeBox);
      }

      connect(ui->_fps_limit_slider, &QSlider::valueChanged, [&](int value)
              {
                  ui->_fps_limit_current_label->setText(
                      QString(tr("FPS limitation, current : %1")).arg(value));
              });

      // Water opacity dev lever (live): 0 = fully transparent, 100 = current/opaque.
      // Writes water/transparency (0..1); WorldRender reads it each frame and feeds the
      // liquid shader, so dragging this updates the water immediately for tuning.
      {
        _perf_layout = new QVBoxLayout(); // owns the runtime-added sliders/toggles -> Performance tab

        auto* water_label = new QLabel(this);
        auto* water_slider = new QSlider(Qt::Horizontal, this);
        water_slider->setObjectName("_water_opacity_slider");
        water_slider->setMinimum(0);
        water_slider->setMaximum(100);
        int const init_val = static_cast<int>(_settings->value("water/transparency", 1.0f).toFloat() * 100.f + 0.5f);
        water_slider->setValue(std::clamp(init_val, 0, 100));
        water_label->setText(tr("Water opacity (dev): %1%").arg(water_slider->value()));
        _perf_layout->addWidget(water_label);
        _perf_layout->addWidget(water_slider);
        connect(water_slider, &QSlider::valueChanged, [this, water_label](int v)
                {
                  water_label->setText(tr("Water opacity (dev): %1%").arg(v));
                  _settings->setValue("water/transparency", v / 100.0f);
                  _settings->sync();
                });
      }

      // Object render distance (live): how far ADT objects + WMOs + models render. This is INDEPENDENT
      // of the "View Distance" field (view_distance) above -- view_distance drives terrain, horizon and
      // FOG, while this slider only caps objects. Keeping them separate means dragging this never moves
      // the fog wall. WorldRender reads object_render_distance every frame and clamps it to the view
      // distance. Capped at farZ (2048) since the far plane clips beyond.
      {
        auto* od_label = new QLabel(this);
        auto* od_slider = new QSlider(Qt::Horizontal, this);
        od_slider->setObjectName("_object_render_distance_slider");
        od_slider->setMinimum(200);
        od_slider->setMaximum(2048);
        int const init_od = static_cast<int>(
          _settings->value("object_render_distance", 925.0f).toFloat());
        od_slider->setValue(std::clamp(init_od, 200, 2048));
        od_label->setText(tr("Object render distance: %1").arg(od_slider->value()));
        _perf_layout->addWidget(od_label);
        _perf_layout->addWidget(od_slider);
        connect(od_slider, &QSlider::valueChanged, [this, od_label](int v)
                {
                  od_label->setText(tr("Object render distance: %1").arg(v));
                  _settings->setValue("object_render_distance", static_cast<float>(v));
                  _settings->sync();
                });
      }

      // Fog distance scale (live): multiplies the authored fog start/end distances. SEPARATE OUTDOOR (zone
      // fog) and INDOOR (WMO room fog) sliders (2026-07-25) -- the renderer applies the one matching whether
      // the camera is inside a WMO. 1.0 = client-authored distances; >1 pushes the fog band out for editing
      // visibility. Purely visual -- fog never affects render distance.
      {
        auto* fs_label = new QLabel(this);
        auto* fs_slider = new QSlider(Qt::Horizontal, this);
        fs_slider->setObjectName("_fog_distance_scale_slider");
        fs_slider->setMinimum(10);
        fs_slider->setMaximum(500);
        int const init_fs = static_cast<int>(_settings->value("fog_distance_scale", 2.0f).toFloat() * 100.f);
        fs_slider->setValue(std::clamp(init_fs, 10, 500));
        fs_label->setText(tr("Outdoor fog distance scale: %1x").arg(fs_slider->value() / 100.0, 0, 'f', 2));
        _perf_layout->addWidget(fs_label);
        _perf_layout->addWidget(fs_slider);
        connect(fs_slider, &QSlider::valueChanged, [this, fs_label](int v)
                {
                  fs_label->setText(tr("Outdoor fog distance scale: %1x").arg(v / 100.0, 0, 'f', 2));
                  _settings->setValue("fog_distance_scale", v / 100.0f);
                  _settings->sync();
                });
      }

      // Indoor (WMO) fog distance scale (live): same as above but applied only while the camera is inside a
      // WMO, so interior fog depth can be tuned independently of the outdoor zone fog.
      {
        auto* fsi_label = new QLabel(this);
        auto* fsi_slider = new QSlider(Qt::Horizontal, this);
        fsi_slider->setObjectName("_fog_distance_scale_interior_slider");
        fsi_slider->setMinimum(10);
        fsi_slider->setMaximum(500);
        int const init_fsi = static_cast<int>(_settings->value("fog_distance_scale_interior", 2.0f).toFloat() * 100.f);
        fsi_slider->setValue(std::clamp(init_fsi, 10, 500));
        fsi_label->setText(tr("Indoor fog distance scale: %1x").arg(fsi_slider->value() / 100.0, 0, 'f', 2));
        _perf_layout->addWidget(fsi_label);
        _perf_layout->addWidget(fsi_slider);
        connect(fsi_slider, &QSlider::valueChanged, [this, fsi_label](int v)
                {
                  fsi_label->setText(tr("Indoor fog distance scale: %1x").arg(v / 100.0, 0, 'f', 2));
                  _settings->setValue("fog_distance_scale_interior", v / 100.0f);
                  _settings->sync();
                });
      }

      // ADT loading radius (live, applies next time you cross a tile): the NxN grid of tiles kept loaded
      // around the camera. radius 1 = 3x3 (matches reference noggit), 2 = 5x5, etc. Fewer tiles = fewer
      // objects/WMOs to draw = higher fps, but content streams in a bit later as you move. See enterTile.
      {
        auto* lr_label = new QLabel(this);
        auto* lr_slider = new QSlider(Qt::Horizontal, this);
        lr_slider->setObjectName("_adt_loading_radius_slider");
        lr_slider->setMinimum(0);
        lr_slider->setMaximum(8);
        int const init_lr = _settings->value("loading_radius", 1).toInt();
        lr_slider->setValue(std::clamp(init_lr, 0, 8));
        lr_label->setText(tr("ADT loading radius: %1 (%2x%2 grid)").arg(lr_slider->value()).arg(2 * lr_slider->value() + 1));
        _perf_layout->addWidget(lr_label);
        _perf_layout->addWidget(lr_slider);
        connect(lr_slider, &QSlider::valueChanged, [this, lr_label](int v)
                {
                  lr_label->setText(tr("ADT loading radius: %1 (%2x%2 grid)").arg(v).arg(2 * v + 1));
                  _settings->setValue("loading_radius", v);
                  _settings->sync();
                });
      }

      // Async loader thread count (applies on RESTART -- the loader pool is created once at startup).
      // 3 matches reference noggit. More threads stream faster but steal CPU from the render thread while
      // loading; fewer = smoother frame time during streaming.
      {
        auto* th_label = new QLabel(this);
        auto* th_slider = new QSlider(Qt::Horizontal, this);
        th_slider->setObjectName("_async_thread_count_slider");
        th_slider->setMinimum(1);
        th_slider->setMaximum(16);
        int const init_th = _settings->value("async_thread_count", 8).toInt();
        th_slider->setValue(std::clamp(init_th, 1, 16));
        th_label->setText(tr("Loader threads (restart to apply): %1").arg(th_slider->value()));
        _perf_layout->addWidget(th_label);
        _perf_layout->addWidget(th_slider);
        connect(th_slider, &QSlider::valueChanged, [this, th_label](int v)
                {
                  th_label->setText(tr("Loader threads (restart to apply): %1").arg(v));
                  _settings->setValue("async_thread_count", v);
                  _settings->sync();
                });
      }

      // Creature draw distance (live): how far creature spawns render their 3D models.
      {
        auto* cd_label = new QLabel(this);
        auto* cd_slider = new QSlider(Qt::Horizontal, this);
        cd_slider->setObjectName("_creature_draw_distance_slider");
        cd_slider->setMinimum(50);
        cd_slider->setMaximum(2000);
        int const init_cd = _settings->value("creature/draw_distance", 500.0f).toFloat();
        cd_slider->setValue(std::clamp(init_cd, 50, 2000));
        cd_label->setText(tr("Creature draw distance: %1").arg(cd_slider->value()));
        _perf_layout->addWidget(cd_label);
        _perf_layout->addWidget(cd_slider);
        connect(cd_slider, &QSlider::valueChanged, [this, cd_label](int v)
                {
                  cd_label->setText(tr("Creature draw distance: %1").arg(v));
                  _settings->setValue("creature/draw_distance", static_cast<float>(v));
                  _settings->sync();
                });
      }

      // Ground clutter density (live): fraction of the client's scattered detail doodads (grass/
      // flowers/pebbles) to render, 0-100% -- the equivalent of the in-game density slider. The
      // on/off is the toolbar/View-menu "Ground clutter" toggle; this only sets how much.
      {
        auto* gc_label = new QLabel(this);
        auto* gc_slider = new QSlider(Qt::Horizontal, this);
        gc_slider->setObjectName("_ground_clutter_density_slider");
        gc_slider->setMinimum(0);
        gc_slider->setMaximum(100);
        int const init_gc = static_cast<int>(_settings->value("render/ground_clutter_density", 100.0f).toFloat());
        gc_slider->setValue(std::clamp(init_gc, 0, 100));
        gc_label->setText(tr("Ground clutter density: %1%").arg(gc_slider->value()));
        _perf_layout->addWidget(gc_label);
        _perf_layout->addWidget(gc_slider);
        connect(gc_slider, &QSlider::valueChanged, [this, gc_label](int v)
                {
                  gc_label->setText(tr("Ground clutter density: %1%").arg(v));
                  _settings->setValue("render/ground_clutter_density", static_cast<float>(v));
                  _settings->sync();
                });
      }

      // Ground clutter draw distance (live): how far from the camera detail doodads render.
      {
        auto* gcd_label = new QLabel(this);
        auto* gcd_slider = new QSlider(Qt::Horizontal, this);
        gcd_slider->setObjectName("_ground_clutter_distance_slider");
        gcd_slider->setMinimum(20);
        gcd_slider->setMaximum(500);
        int const init_gcd = static_cast<int>(_settings->value("render/ground_clutter_distance", 160.0f).toFloat());
        gcd_slider->setValue(std::clamp(init_gcd, 20, 500));
        gcd_label->setText(tr("Ground clutter distance: %1").arg(gcd_slider->value()));
        _perf_layout->addWidget(gcd_label);
        _perf_layout->addWidget(gcd_slider);
        connect(gcd_slider, &QSlider::valueChanged, [this, gcd_label](int v)
                {
                  gcd_label->setText(tr("Ground clutter distance: %1").arg(v));
                  _settings->setValue("render/ground_clutter_distance", static_cast<float>(v));
                  _settings->sync();
                });
      }

      // WMO water overlap dedupe (stencil) toggle (live): ON = overlapping exterior WMO water
      // planes (e.g. Timbermaw) draw each pixel only once so they don't stack/darken; OFF = legacy
      // behaviour (every plane blended). Read live by WMORender each frame.
      {
        auto* wmo_stencil_cb = new QCheckBox(tr("WMO water overlap dedupe (stencil)"), this);
        wmo_stencil_cb->setObjectName("_wmo_water_stencil_checkbox");
        wmo_stencil_cb->setChecked(_settings->value("water/wmo_stencil", true).toBool());
        _perf_layout->addWidget(wmo_stencil_cb);
        connect(wmo_stencil_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("water/wmo_stencil", checked);
                  _settings->sync();
                });
      }

      // WMO portal culling (live): ON = when inside a building/dungeon, only the rooms reachable through
      // visible doorways from your room are drawn (big fps win in cities like Ironforge). It's conservative
      // -- it never hides on-screen geometry -- but if anything ever pops out that shouldn't, turn it off.
      {
        auto* portal_cb = new QCheckBox(tr("WMO portal culling (hide unseen interior rooms)"), this);
        portal_cb->setObjectName("_wmo_portal_culling_checkbox");
        portal_cb->setChecked(_settings->value("render/wmo_portal_culling", false).toBool());
        _perf_layout->addWidget(portal_cb);
        connect(portal_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render/wmo_portal_culling", checked);
                  _settings->sync();
                });
      }

      // Distant horizon backdrop toggle (live): ON = draw the low-res far terrain silhouette beyond
      // the detailed terrain (only appears with fog); OFF = no backdrop, so enabling fog never renders
      // distant mesh past the view distance. Read live by WorldRender each frame.
      {
        auto* horizon_cb = new QCheckBox(tr("Render distant horizon backdrop"), this);
        horizon_cb->setObjectName("_render_horizon_checkbox");
        horizon_cb->setChecked(_settings->value("render_horizon", false).toBool());
        _perf_layout->addWidget(horizon_cb);
        connect(horizon_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render_horizon", checked);
                  _settings->sync();
                });
      }

      // Classic MCLQ water toggle: when ON, CLASSIC (vanilla) projects also write the legacy
      // per-MCNK MCLQ liquid chunk alongside MH2O on save. No effect on WotLK output.
      {
        auto* mclq_cb = new QCheckBox(tr("Save classic MCLQ water (vanilla, alongside MH2O)"), this);
        mclq_cb->setObjectName("_save_classic_mclq_checkbox");
        mclq_cb->setChecked(_settings->value("water/save_classic_mclq", true).toBool());
        _perf_layout->addWidget(mclq_cb);
        connect(mclq_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("water/save_classic_mclq", checked);
                  _settings->sync();
                });
      }

      // Make the View Distance field apply LIVE: WorldRender reads view_distance every frame, so
      // changing this field moves the terrain/horizon and FOG distance immediately (no Apply / reload).
      // Objects have their own "Object render distance" slider (object_render_distance).
      connect(ui->viewDistanceField, qOverload<double>(&QDoubleSpinBox::valueChanged), [this](double v)
              {
                _settings->setValue("view_distance", static_cast<float>(v));
                _settings->sync();
              });

      ui->_wireframe_color->setColor(Qt::white);

      connect(ui->saveButton, &QPushButton::clicked, [this]
              {
                hide();
                save_changes();
              }
      );

      connect(ui->discardButton, &QPushButton::clicked, [this]
              {
                hide();
                discard_changes();
              }
      );

      connect(ui->mysql_connect_test, &QPushButton::clicked, [this]
          {
              save_changes();
              #ifdef USE_MYSQL_UID_STORAGE
              mysql::testConnection();
              #endif
          }
      );

      // Split the graphics settings out of "Preferences" into a dedicated "Graphics" tab and add the
      // persistent render-feature toggles. Done before discard_changes() so the toggle checkboxes exist.
      build_graphics_tab();

      // load the values in the fields
      discard_changes();
    }

    void settings::build_graphics_tab()
    {
      // Move the whole "Viewport" group (VSync, Anti-Aliasing, Fullscreen, View Distance, FarZ, ADT
      // unloading, FPS limit, water opacity, object/creature render distances, WMO water dedupe,
      // distant horizon, MCLQ water) out of the Preferences tab into a new Graphics tab. groupBox_7
      // is reparented, not copied, so all existing widget wiring (ui->_vsync_cb, etc.) stays valid.
      auto* graphicsPage = new QWidget();
      auto* gLayout = new QVBoxLayout(graphicsPage);
      gLayout->setContentsMargins(4, 4, 4, 4);

      ui->groupBox_7->setParent(nullptr);
      gLayout->addWidget(ui->groupBox_7);

      // Render-feature toggles: persistent versions of the live View-menu/toolbar switches (Bloom,
      // Fog, Doodads, etc.). MapView::setupViewMenu() reads these "render/*" keys at map load and
      // applies them to its BoolToggleProperties, so a preference here survives restarts.
      auto* toggles = new QGroupBox("Render features");
      toggles->setAlignment(Qt::AlignCenter);
      auto* form = new QFormLayout(toggles);

      auto add_toggle = [&](QString const& label, QString const& key, bool def)
      {
        auto* cb = new QCheckBox();
        form->addRow(new QLabel(label), cb);
        _render_toggles.emplace_back(key, cb, def);
      };
      add_toggle("Doodads (M2 models)",              "render/doodads",          true);
      add_toggle("WMO doodads",                      "render/wmo_doodads",      true);
      add_toggle("WMOs",                             "render/wmo",              true);
      add_toggle("Terrain",                          "render/terrain",          true);
      add_toggle("Water",                            "render/water",            true);
      add_toggle("Model animations",                 "render/model_animations", true);
      add_toggle("Bloom",                            "render/bloom",            true);
      add_toggle("Fog",                              "render/fog",              true);
      add_toggle("Vertex color (terrain lighting)",  "render/vertex_color",     true);
      add_toggle("Baked terrain shadows",            "render/baked_shadows",    true);

      // MSAA: applies LIVE -- WorldRender re-reads render/msaa when (re)allocating its scene targets
      // each frame, so changing this reallocates the multisampled framebuffer on the spot.
      {
        auto* msaa = new QComboBox();
        msaa->addItem("Off", 0);
        msaa->addItem("2x", 2);
        msaa->addItem("4x", 4);
        msaa->addItem("8x", 8);
        int const cur = _settings->value("render/msaa", 8).toInt();
        msaa->setCurrentIndex(std::max(0, msaa->findData(cur)));
        connect(msaa, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx)
        {
          _settings->setValue("render/msaa", msaa->itemData(idx).toInt());
          _settings->sync();
        });
        form->addRow(new QLabel("Anti-aliasing (MSAA)"), msaa);
      }

      // Anisotropic filtering: applies LIVE -- WorldRender::draw re-applies the level to every loaded
      // texture array (models, particles, tilesets, liquids) when this changes, so no restart. The
      // 1.12 client ran little/no AF; the modern 16x default over-sharpens oblique/distant textures
      // ("everything pops / looks crispy"). Lower this to soften toward the client look. Off = 1x.
      {
        auto* af = new QComboBox();
        af->addItem("Off", 1);
        af->addItem("2x", 2);
        af->addItem("4x", 4);
        af->addItem("8x", 8);
        af->addItem("16x", 16);
        int const cur = _settings->value("render/anisotropic_filtering", 16).toInt();
        af->setCurrentIndex(std::max(0, af->findData(cur)));
        connect(af, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx)
        {
          _settings->setValue("render/anisotropic_filtering", af->itemData(idx).toInt());
          _settings->sync();
        });
        form->addRow(new QLabel("Anisotropic filtering"), af);
      }

      gLayout->addWidget(toggles);
      gLayout->addStretch(1);

      int const idx = ui->tabWidget->indexOf(ui->tab_7); // insert right where Preferences was
      ui->tabWidget->insertTab(idx, graphicsPage, "Graphics");

      // Performance tab: the distance sliders + advanced WMO/water toggles added at runtime. They
      // used to be appended into the Viewport group and squished the Graphics tab unreadable.
      if (_perf_layout)
      {
        auto* perfPage = new QWidget();
        auto* pLayout = new QVBoxLayout(perfPage);
        pLayout->setContentsMargins(4, 4, 4, 4);
        auto* distGroup = new QGroupBox("Distances && advanced rendering");
        distGroup->setAlignment(Qt::AlignCenter);
        distGroup->setLayout(_perf_layout);
        pLayout->addWidget(distGroup);
        pLayout->addStretch(1);
        ui->tabWidget->insertTab(idx + 1, perfPage, "Performance");
      }
    }

    void settings::discard_changes()
    {
      for (auto const& rt : _render_toggles)
      {
        std::get<1>(rt)->setChecked(
            _settings->value(std::get<0>(rt), std::get<2>(rt)).toBool());
      }

      ui->importPathField->setText(_settings->value("project/import_file", "import.txt").toString());
      ui->wmvLogPathField->setText(_settings->value("project/wmv_log_file").toString());
      ui->viewDistanceField->setValue(_settings->value("view_distance", 900.f).toFloat());
      ui->farZField->setValue(_settings->value("farZ", 900.f).toFloat());
      ui->_undock_tool_properties->setChecked(
          _settings->value("undock_tool_properties/enabled", true).toBool());
      ui->_undock_small_texture_palette->setChecked(
          _settings->value("undock_small_texture_palette/enabled", true).toBool());
      ui->_vsync_cb->setChecked(_settings->value("vsync", false).toBool());
      ui->_anti_aliasing_cb->setChecked(_settings->value("anti_aliasing", false).toBool());
      ui->_fullscreen_cb->setChecked(_settings->value("fullscreen", false).toBool());
      ui->_adt_unload_dist->setValue(_settings->value("unload_dist", 5).toInt());
      ui->_adt_unload_check_interval->setValue(_settings->value("unload_interval", 3).toInt());
      ui->_uid_cb->setChecked(_settings->value("uid_startup_check", true).toBool());
      ui->_systemWindowFrame->setChecked(_settings->value("systemWindowFrame", true).toBool());
      ui->_nativeMenubar->setChecked(_settings->value("nativeMenubar", true).toBool());
      ui->_classic_ui->setChecked(_settings->value("classicUI", false).toBool());
      ui->_additional_file_loading_log->setChecked(
          _settings->value("additional_file_loading_log", false).toBool());
      ui->_keyboard_locale->setCurrentText(_settings->value("keyboard_locale", "QWERTY").toString());
      ui->_theme->setCurrentText(_settings->value("theme", "Dark").toString());

      ui->assetBrowserBgCol->setColor(_settings->value("assetBrowser/background_color",
        QVariant::fromValue(QColor(127, 127, 127))).value<QColor>());
      ui->assetBrowserDiffuseLight->setColor(_settings->value("assetBrowser/diffuse_light",
        QVariant::fromValue(QColor::fromRgbF(1.0f, 0.532352924f, 0.0f))).value<QColor>());

      ui->assetBrowserAmbientLight->setColor(_settings->value("assetBrowser/ambient_light",
        QVariant::fromValue(QColor::fromRgbF(0.407770514f, 0.508424163f, 0.602650642f))).value<QColor>());

      ui->assetBrowserCopyToClipboard->setChecked(_settings->value("assetBrowser/copy_to_clipboard", true).toBool());
      ui->assetBrowserDefaultModel->setText(_settings->value("assetBrowser/default_model",
                                     "world/wmo/azeroth/human/buildings/human_farm/farm.wmo").toString());
      ui->assetBrowserMoveSensitivity->setValue(_settings->value("assetBrowser/move_sensitivity", 15.0f).toFloat());
      ui->assetBrowserRenderAssetPreview->setChecked(_settings->value("assetBrowser/render_asset_preview", false).toBool());


#ifdef USE_MYSQL_UID_STORAGE
  ui->MySQL_box->setChecked(true);

      // Per-project MySQL settings (see MySqlSettings.hpp) so each project keeps its own connection.
      auto server_str = Noggit::mysqlSetting("server", "127.0.0.1").toString();
      auto user_str = Noggit::mysqlSetting("user", "127.0.0.1").toString();
      auto pwd_str = Noggit::mysqlSetting("pwd", "127.0.0.1").toString();
      auto db_str = Noggit::mysqlSetting("db", "127.0.0.1").toString();
      auto port_int = Noggit::mysqlSetting("port", "127.0.0.1").toInt();

      // set some default
      if (server_str.isEmpty())
          server_str = "127.0.0.1";
      if (user_str.isEmpty())
          user_str = "root";
      if (pwd_str.isEmpty())
          pwd_str = "root";
      if (db_str.isEmpty())
          db_str = "noggit";
      if (!port_int)
          port_int = 3306;

      ui->_mysql_server_field->setText (server_str);
      ui->_mysql_user_field->setText(user_str);
      ui->_mysql_pwd_field->setText (pwd_str);
      ui->_mysql_db_field->setText (db_str);
      ui->_mysql_port_field->setValue (port_int);
#endif

      int wireframe_type = _settings->value("wireframe/type", 0).toInt();

      if (wireframe_type)
      {
        ui->radio_wire_cursor->setChecked(true);
      }
      else
      {
        ui->radio_wire_full->setChecked(true);
      }

      ui->_wireframe_radius->setValue(_settings->value("wireframe/radius", 1.5f).toFloat());
      ui->_wireframe_width->setValue(_settings->value("wireframe/width", 1.f).toFloat());
      ui->_wireframe_color->setColor(_settings->value("wireframe/color").value<QColor>());
      ui->_fps_limit_slider->setValue(_settings->value("fps_limit", 60).toInt());
    }

    void settings::save_changes()
    {
      _settings->setValue("project/import_file", ui->importPathField->text());
      _settings->setValue("project/wmv_log_file", ui->wmvLogPathField->text());
      _settings->setValue("farZ", ui->farZField->value());
      _settings->setValue("view_distance", ui->viewDistanceField->value());
      _settings->setValue("undock_tool_properties/enabled", ui->_undock_tool_properties->isChecked());
      _settings->setValue("undock_small_texture_palette/enabled",
                          ui->_undock_small_texture_palette->isChecked());
      _settings->setValue("vsync", ui->_vsync_cb->isChecked());
      _settings->setValue("anti_aliasing", ui->_anti_aliasing_cb->isChecked());
      _settings->setValue("fullscreen", ui->_fullscreen_cb->isChecked());
      _settings->setValue("unload_dist", ui->_adt_unload_dist->value());
      _settings->setValue("unload_interval", ui->_adt_unload_check_interval->value());
      _settings->setValue("uid_startup_check", ui->_uid_cb->isChecked());
      _settings->setValue("additional_file_loading_log", ui->_additional_file_loading_log->isChecked());
      _settings->setValue("keyboard_locale", ui->_keyboard_locale->currentText());
      _settings->setValue("systemWindowFrame", ui->_systemWindowFrame->isChecked());
      _settings->setValue("nativeMenubar", ui->_nativeMenubar->isChecked());
      _settings->setValue("classicUI", ui->_classic_ui->isChecked());

#ifdef USE_MYSQL_UID_STORAGE
      // Save under the per-project keys (see MySqlSettings.hpp) so each project keeps its own MySQL
      // connection instead of all projects sharing one global set.
      _settings->setValue (Noggit::mysqlSettingKey("enabled"), ui->MySQL_box->isChecked());
      _settings->setValue (Noggit::mysqlSettingKey("server"), ui->_mysql_server_field->text());
      _settings->setValue (Noggit::mysqlSettingKey("user"), ui->_mysql_user_field->text());
      _settings->setValue (Noggit::mysqlSettingKey("pwd"), ui->_mysql_pwd_field->text());
      _settings->setValue (Noggit::mysqlSettingKey("db"), ui->_mysql_db_field->text());
      _settings->setValue (Noggit::mysqlSettingKey("port"), ui->_mysql_port_field->text());
#endif

      _settings->setValue("wireframe/type", ui->radio_wire_cursor->isChecked());
      _settings->setValue("wireframe/radius", ui->_wireframe_radius->value());
      _settings->setValue("wireframe/width", ui->_wireframe_width->value());
      _settings->setValue("wireframe/color", ui->_wireframe_color->color());
      _settings->setValue("theme", ui->_theme->currentText());
      _settings->setValue("assetBrowser/background_color", ui->assetBrowserBgCol->color());
      _settings->setValue("assetBrowser/diffuse_light", ui->assetBrowserDiffuseLight->color());
      _settings->setValue("assetBrowser/ambient_light", ui->assetBrowserAmbientLight->color());
      _settings->setValue("assetBrowser/copy_to_clipboard", ui->assetBrowserCopyToClipboard->isChecked());
      _settings->setValue("assetBrowser/default_model", ui->assetBrowserDefaultModel->text());
      _settings->setValue("assetBrowser/move_sensitivity", ui->assetBrowserMoveSensitivity->value());
      _settings->setValue("assetBrowser/render_asset_preview", ui->assetBrowserRenderAssetPreview->isChecked());
      _settings->setValue("fps_limit", ui->_fps_limit_slider->value());

      for (auto const& rt : _render_toggles)
      {
        _settings->setValue(std::get<0>(rt), std::get<1>(rt)->isChecked());
      }

      _settings->sync();

      emit saved();
    }
  }
}
