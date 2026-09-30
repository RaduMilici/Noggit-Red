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
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QDir>
#include <QApplication>

#include <noggit/ssh/SshHostKeyPrompt.hpp>
#include <noggit/ssh/SshTunnelManager.hpp>

#include <memory>
#include <thread>

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

      // [settings window sizing 2026-08-16] The Performance tab has grown a lot of runtime toggles/sliders,
      // so give the window a taller default and open it near the top-left of the screen (it used to open a
      // bit low and to the right). Raise the minimum height so the tab content isn't clipped.
      setMinimumSize(680, 720);
      resize(780, 900);
      move(40, 30);

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


      // Modern (CASC) clients: the community listfile (id;path csv) that names files noggit opens by
      // path (DB2 tables, the map WDT) and labels ids in the UI. A project-local listfile.csv still wins.
      {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel("CASC listfile (modern clients)", this);
        label->setMinimumWidth(ui->label_28->minimumWidth());
        _casc_listfile_field = new QLineEdit(this);
        _casc_listfile_field->setToolTip("Community listfile csv (id;path) used for CASC-backed projects when the project folder has no listfile.csv");
        auto* browse = new QPushButton("Browse", this);
        row->addWidget(label);
        row->addWidget(_casc_listfile_field);
        row->addWidget(browse);
        ui->verticalLayout_21->addLayout(row);

        connect(_casc_listfile_field, &QLineEdit::textChanged, [&](QString value)
                {
                  _settings->setValue("casc/listfile_path", value);
                }
        );
        connect(browse, &QPushButton::clicked, [=]
                {
                  auto result(QFileDialog::getOpenFileName(nullptr, "CASC listfile", _casc_listfile_field->text(), "Listfile (*.csv *.txt);;All files (*)"));
                  if (!result.isNull())
                  {
                    _casc_listfile_field->setText(result);
                  }
                }
        );
      }

      // Modern (CASC) clients: texture fidelity. The Classic Forever 1.60.1 beta root lists 95,774 textures
      // twice -- content flag 0x1 marks the 4x-resolution variant (512x1024 vs 128x256 for the same file
      // id). The creature/character M2 duplicates are the 0x80 low-violence set and WMOs have no variants,
      // so this is the whole HD/SD switch the client data offers. Read by ApplicationProject when the
      // client storage opens (the root manifest is resolved once), so it applies on project open.
      {
        auto* hd_cb = new QCheckBox("Prefer HD texture variants (modern clients; applies when a project is opened)", this);
        hd_cb->setObjectName("_casc_prefer_hd_checkbox");
        hd_cb->setToolTip("When the client's root manifest lists a texture in both a high-resolution (content flag 0x1) and a "
                          "low-resolution record, use the high-resolution one. Only records present in the local store are used "
                          "either way. Untick for the low-resolution set. Reopen the project to apply.");
        hd_cb->setChecked(_settings->value("casc/prefer_hd_textures", true).toBool());
        ui->verticalLayout_21->addWidget(hd_cb);
        connect(hd_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("casc/prefer_hd_textures", checked);
                  _settings->sync();
                });
      }

      // Modern (CASC) clients: character model fidelity, LIVE. The Forever Beta pairs 18 plain/HD character
      // files (humanmale.m2 / humanmale_hd.m2) through CreatureModelData rows and gives 8,568 display
      // extras both a classic and an HD bake; the client's "character models" option swaps both at
      // runtime. World.cpp resolves spawns through this value and MapView re-resolves them when it
      // changes (docs/client_re/42 sec 23).
      {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel("Character models (modern clients)", this);
        label->setMinimumWidth(ui->label_28->minimumWidth());
        auto* fidelity = new QComboBox(this);
        fidelity->setObjectName("_character_model_fidelity_combo");
        fidelity->addItem("As authored by each display", 0);
        fidelity->addItem("Classic (SD) models", 1);
        fidelity->addItem("HD models", 2);
        fidelity->setToolTip("Which variant of a paired character model NPCs use: the row the display authors, the plain file "
                             "with its classic bake, or the _hd file with its HD bake. Applies live to loaded spawns.");
        fidelity->setCurrentIndex(std::max(0, fidelity->findData(_settings->value("render/character_model_fidelity", 0).toInt())));
        row->addWidget(label);
        row->addWidget(fidelity);
        ui->verticalLayout_21->addLayout(row);
        connect(fidelity, qOverload<int>(&QComboBox::currentIndexChanged), [this, fidelity](int index)
                {
                  _settings->setValue("render/character_model_fidelity", fidelity->itemData(index).toInt());
                  _settings->sync();
                });
      }

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

      // Ground clutter amount == the client's own density CVar (3.3.5a groundEffectDensity, 1.12
      // frillDensity): the number of 8x8 subcell visits per chunk, so 64 == every subcell once and
      // the DBC's authored per-subcell amount is scaled by visits/64. Blizzard validate this to
      // 16..256 and ship it at 16; the slider keeps their full range so the scene can be made denser
      // than the client allows itself, without starting there. See RE_notes/14_ground_clutter_335a.md.
      //
      // NOT live: the placement is baked into each chunk's cached detail-doodad list, so a change
      // only shows on chunks loaded afterwards -- hence the reload note in the label.
      {
        auto* gcf_label = new QLabel(this);
        auto* gcf_slider = new QSlider(Qt::Horizontal, this);
        gcf_slider->setObjectName("_ground_clutter_frill_density_slider");
        gcf_slider->setMinimum(16);
        gcf_slider->setMaximum(256);
        int const init_gcf = static_cast<int>(_settings->value("render/ground_clutter_frill_density", 16.0f).toFloat());
        gcf_slider->setValue(std::clamp(init_gcf, 16, 256));
        gcf_label->setText(tr("Ground clutter amount: %1 (client default 16; needs map reload)")
                             .arg(gcf_slider->value()));
        _perf_layout->addWidget(gcf_label);
        _perf_layout->addWidget(gcf_slider);
        connect(gcf_slider, &QSlider::valueChanged, [this, gcf_label](int v)
                {
                  gcf_label->setText(tr("Ground clutter amount: %1 (client default 16; needs map reload)").arg(v));
                  _settings->setValue("render/ground_clutter_frill_density", static_cast<float>(v));
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
        int const init_gcd = static_cast<int>(_settings->value("render/ground_clutter_distance", 70.0f).toFloat());
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
        portal_cb->setChecked(_settings->value("render/wmo_portal_culling", true).toBool());
        _perf_layout->addWidget(portal_cb);
        connect(portal_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render/wmo_portal_culling", checked);
                  _settings->sync();
                });
      }

      // Client doodad distance cull (live): ON = static doodads/trees/clutter are culled + faded by the
      // CLIENT's exact per-size-class distance table (small clutter drops at ~30y, trees ~200y, tall
      // landmarks ~750y), like the real 3.3.5a client. OFF = draw everything out to the object render
      // distance (old editor behaviour). Byte-exact port of FUN_00791cb0; read live by WorldRender.
      {
        auto* doodad_cull_cb = new QCheckBox(tr("Client doodad distance cull (size-class)"), this);
        doodad_cull_cb->setObjectName("_gv_doodad_cull_checkbox");
        // [2026-09-01] Default must match WorldRender, which reads this as FALSE (it is opt-IN since
        // it was made a toggle). The panel defaulting to true showed the box CHECKED while the
        // renderer was not culling -- and any build predating the opt-in change writes true here,
        // silently turning the cull on for everyone sharing these QSettings.
        doodad_cull_cb->setChecked(_settings->value("render/gv_doodad_cull", false).toBool());
        _perf_layout->addWidget(doodad_cull_cb);
        connect(doodad_cull_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render/gv_doodad_cull", checked);
                  _settings->sync();
                });
      }

      // Client DRESSING distance cull (live): ON = small vegetation props (size classes 0-1: ferns,
      // shrubs, grass clumps under ~4yd) cull + fade at the client's 30yd/100yd like the real client --
      // distant vistas show bare ground instead of a green prop carpet ("grass should look brown").
      // Trees/landmarks (classes 2-4) are NOT affected -- they keep the uniform draw distance unless
      // the full doodad cull above is enabled. Byte-exact same FUN_00791cb0 tables; read live.
      {
        auto* dressing_cull_cb = new QCheckBox(tr("Client dressing distance cull (small vegetation)"), this);
        dressing_cull_cb->setObjectName("_gv_dressing_cull_checkbox");
        dressing_cull_cb->setChecked(_settings->value("render/gv_dressing_cull", true).toBool());
        _perf_layout->addWidget(dressing_cull_cb);
        connect(dressing_cull_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render/gv_dressing_cull", checked);
                  _settings->sync();
                });
      }

      // Client creature distance cull (live): ON = creatures/NPCs are culled + faded by the SAME client
      // size-class table (a humanoid is class 1 -> ~100y, big creatures persist farther), exactly like the
      // client. OFF = draw creatures out to the flat creature draw distance (500y). The player character is
      // never culled. Byte-exact port of FUN_00791cb0 (units are scene nodes like doodads); read live.
      {
        auto* creature_cull_cb = new QCheckBox(tr("Client creature distance cull (size-class)"), this);
        creature_cull_cb->setObjectName("_gv_creature_cull_checkbox");
        creature_cull_cb->setChecked(_settings->value("render/gv_creature_cull", true).toBool());
        _perf_layout->addWidget(creature_cull_cb);
        connect(creature_cull_cb, &QCheckBox::toggled, [this](bool checked)
                {
                  _settings->setValue("render/gv_creature_cull", checked);
                  _settings->sync();
                });
      }

      // Distant horizon backdrop toggle (live): ON = draw the client's low-res WDL terrain (CMapLowDetail)
      // in the fog colour beyond the detailed terrain, fog on or off; OFF = no backdrop. Read live by
      // WorldRender each frame.
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

      // [2026-09-09] Horizon far clip scale (live) = the client's `horizonFarclipScale` CVar: the low-res
      // backdrop reaches view distance x this (stock default 4.0, the client caps it at 6.0). Also sets
      // how far the backdrop's tile selection looks. Match your Config.wtf value to see the same horizon.
      {
        auto* hz_label = new QLabel(this);
        auto* hz_slider = new QSlider(Qt::Horizontal, this);
        hz_slider->setObjectName("_horizon_farclip_scale_slider");
        hz_slider->setMinimum(100);
        hz_slider->setMaximum(600);
        int const init_hz = static_cast<int>(_settings->value("horizon_farclip_scale", 4.0f).toFloat() * 100.f);
        hz_slider->setValue(std::clamp(init_hz, 100, 600));
        hz_label->setText(tr("Horizon far clip scale (client horizonFarclipScale): %1x").arg(hz_slider->value() / 100.0, 0, 'f', 2));
        _perf_layout->addWidget(hz_label);
        _perf_layout->addWidget(hz_slider);
        connect(hz_slider, &QSlider::valueChanged, [this, hz_label](int v)
                {
                  hz_label->setText(tr("Horizon far clip scale (client horizonFarclipScale): %1x").arg(v / 100.0, 0, 'f', 2));
                  _settings->setValue("horizon_farclip_scale", v / 100.0f);
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

      connect(ui->mysql_connect_test, &QPushButton::clicked, this, [this] { test_mysql_connection(); });

      build_ssh_section();

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

      // [VULKAN 2026-08-29] Graphics API picker (user asked for a dropdown instead of launcher env vars).
      // 0 = OpenGL (default, the shipping renderer). 1 = Vulkan preview: the VK backend renders alongside
      // GL and is composited into the viewport (the migration lane; passes move over one by one).
      // 2 = Vulkan full view (dev): VK fills the viewport, GL scene draw skipped -- clay world, for fps
      // probing only. Read ONCE at map load (MapView::initializeGL) -> takes effect on the next map open.
      {
        auto* api = new QComboBox();
        api->addItem("OpenGL", 0);
        api->addItem("Vulkan", 1);
        // (mode 2 = bare VK full view is a DEV-ONLY checkpoint: NOGGIT_VK_FULL env, never in the UI)
        int const cur = std::min(1, _settings->value("render/graphics_api", 0).toInt());
        api->setCurrentIndex(std::max(0, api->findData(cur)));
        api->setToolTip("Applies when a map is (re)opened. Vulkan falls back to OpenGL if unsupported.");
        connect(api, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx)
        {
          _settings->setValue("render/graphics_api", api->itemData(idx).toInt());
          _settings->sync();
        });
        form->addRow(new QLabel("Graphics API"), api);

        // Parity harness: with the Vulkan preview, compare GL vs VK pixels every ~60 frames and log
        // "[VK-DIFF] ..." lines (+ per-camera PNGs when nrcln/bin/Release/vk_diff_cams.txt exists).
        auto* parity_cb = new QCheckBox("Vulkan parity check (logs [VK-DIFF], writes vk_diff\\*.png)");
        parity_cb->setChecked(_settings->value("render/vk_parity_check", false).toBool());
        parity_cb->setToolTip("Needs Graphics API = Vulkan (preview). Applies on next map open.");
        connect(parity_cb, &QCheckBox::toggled, [=](bool checked)
        {
          _settings->setValue("render/vk_parity_check", checked);
          _settings->sync();
        });
        form->addRow(parity_cb);
      }

      // Shadow quality: the 3.3.5a client's own extShadowQuality ladder (0-5, labels from the exe --
      // wow335a.exe @0xa4d47a..0xa4d5bc; RE doc 35). Level 0 IS the classic/1.12 look: baked terrain
      // shadows + the blob unit decal (client rule FUN_007e49e0: blobs draw only at quality < 1).
      // Applies LIVE -- WorldRender re-reads graphics/shadow_quality every frame.
      {
        auto* sq = new QComboBox();
        sq->addItem("0 - Lowest (baked terrain + blob units, 1.12-style)", 0);
        sq->addItem("1 - Low (dynamic PC/NPC shadows, low-res)", 1);
        sq->addItem("2 - Medium (dynamic PC/NPC shadows, high-res)", 2);
        sq->addItem("3 - Med-High (full environmental + PC/NPC, low-res, lg-dist)", 3);
        sq->addItem("4 - High (full environmental + PC/NPC, hi-res, lg-dist)", 4);
        sq->addItem("5 - Very High (cascaded shadow maps)", 5);
        int const cur = _settings->value("graphics/shadow_quality", 3).toInt();
        sq->setCurrentIndex(std::max(0, sq->findData(cur)));
        connect(sq, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx)
        {
          _settings->setValue("graphics/shadow_quality", sq->itemData(idx).toInt());
          _settings->sync();
        });
        form->addRow(new QLabel("Shadow quality (client extShadowQuality)"), sq);
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
      _casc_listfile_field->setText(_settings->value("casc/listfile_path").toString());
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
  // load the STORED per-project toggle -- this was hardcoded setChecked(true), so ANY settings
  // visit that ended in save silently re-enabled MySQL no matter how often it was turned off
  ui->MySQL_box->setChecked(Noggit::mysqlSetting("enabled", false).toBool());

      // Per-project MySQL settings (see MySqlSettings.hpp) so each project keeps its own connection.
      auto server_str = Noggit::mysqlSetting("server", "127.0.0.1").toString();
      // Same defaults as the connection code (mysql.cpp loadConnectionDetails); these used to fall back
      // to "127.0.0.1" for every field, so an unsaved World DB showed up as "127.0.0.1".
      auto user_str = Noggit::mysqlSetting("user", "root").toString();
      auto pwd_str = Noggit::mysqlSetting("pwd", "mangos").toString();
      auto db_str = Noggit::mysqlSetting("db", "tw_world").toString();
      auto port_int = Noggit::mysqlSetting("port", 3306).toInt();

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

      // SSH tunnel (per project, see ssh/SshTunnelConfig.hpp). Read the raw toggle, not
      // TunnelConfig::enabled (which also requires MySQL itself to be enabled).
      {
        auto const tunnel = Noggit::Ssh::TunnelConfig::fromProjectSettings();
        _ssh_box->setChecked(Noggit::mysqlSetting(Noggit::Ssh::Keys::enabled(), false).toBool());
        _ssh_host->setText(tunnel.ssh_host);
        _ssh_port->setValue(tunnel.ssh_port);
        _ssh_user->setText(tunnel.ssh_user);
        _ssh_key->setText(tunnel.key_path);
        _ssh_remote_host->setText(tunnel.remote_db_host);
        _ssh_remote_port->setValue(tunnel.remote_db_port);
        _ssh_fingerprint->setText(tunnel.expected_fingerprint);
        update_direct_fields_enabled();
        update_ssh_status();
      }
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

      {
        namespace Keys = Noggit::Ssh::Keys;
        QString const key_path = Noggit::Ssh::expandKeyPath(_ssh_key->text());
        QString const pin = Noggit::Ssh::normalizeFingerprint(_ssh_fingerprint->text());
        _ssh_key->setText(key_path);
        if (!pin.isEmpty())
        {
          _ssh_fingerprint->setText(pin);
        }
        _settings->setValue(Noggit::mysqlSettingKey(Keys::enabled()), _ssh_box->isChecked());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::host()), _ssh_host->text().trimmed());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::port()), _ssh_port->value());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::user()), _ssh_user->text().trimmed());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::keyPath()), key_path);
        _settings->setValue(Noggit::mysqlSettingKey(Keys::remoteDbHost()), _ssh_remote_host->text().trimmed());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::remoteDbPort()), _ssh_remote_port->value());
        _settings->setValue(Noggit::mysqlSettingKey(Keys::fingerprint()), _ssh_fingerprint->text().trimmed());
        _settings->sync();

        // Settings changed while this project is open: restart (or stop) its tunnel to match.
        auto& tunnel = Noggit::Ssh::SshTunnelManager::instance();
        if (!tunnel.openProjectId().isEmpty() && tunnel.openProjectId() == Noggit::projectKeyId())
        {
          tunnel.applyConfig(Noggit::Ssh::TunnelConfig::fromProjectSettings());
        }
      }
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
      void settings::build_ssh_section()
    {
      using Noggit::Ssh::SshTunnelManager;

      _ssh_box = new QGroupBox("SSH tunnel (connect to a remote database securely)", this);
      _ssh_box->setCheckable(true);
      _ssh_box->setChecked(false);
      _ssh_box->setToolTip("Noggit starts a private SSH tunnel to the server and connects to MySQL through it, so the "
                           "database port never has to be open to the internet. Saved per project.");

      auto* layout = new QVBoxLayout(_ssh_box);
      auto* form = new QFormLayout();
      layout->addLayout(form);

      _ssh_host = new QLineEdit(_ssh_box);
      _ssh_host->setPlaceholderText("server IP or host name, e.g. 203.0.113.10");
      form->addRow("SSH host", _ssh_host);

      _ssh_port = new QSpinBox(_ssh_box);
      _ssh_port->setRange(1, 65535);
      _ssh_port->setValue(22);
      form->addRow("SSH port", _ssh_port);

      _ssh_user = new QLineEdit(_ssh_box);
      _ssh_user->setPlaceholderText("the SSH username your administrator gave you");
      form->addRow("SSH user", _ssh_user);

      auto* key_row = new QHBoxLayout();
      _ssh_key = new QLineEdit(_ssh_box);
      _ssh_key->setPlaceholderText("your PRIVATE key file (not the .pub file)");
      auto* browse = new QPushButton("Browse...", _ssh_box);
      key_row->addWidget(_ssh_key);
      key_row->addWidget(browse);
      form->addRow("Private key", key_row);
      connect(browse, &QPushButton::clicked, this, [this]
      {
        QString start = _ssh_key->text().isEmpty() ? QDir::homePath() + "/.ssh" : QFileInfo(_ssh_key->text()).absolutePath();
        QString const file = QFileDialog::getOpenFileName(this, "Select your SSH private key", start);
        if (!file.isEmpty())
        {
          _ssh_key->setText(file);
        }
      });

      auto* agent_note = new QLabel(
        "Keys protected by a passphrase must be unlocked in ssh-agent first (run: ssh-add &lt;key file&gt;). "
        "Noggit never asks for, stores or sends passphrases.", _ssh_box);
      agent_note->setWordWrap(true);
      agent_note->setTextFormat(Qt::RichText);
      agent_note->setStyleSheet("color: gray;");
      layout->addWidget(agent_note);

      auto* advanced = new QGroupBox("Advanced (the defaults are usually right)", _ssh_box);
      auto* advanced_form = new QFormLayout(advanced);
      _ssh_remote_host = new QLineEdit(advanced);
      _ssh_remote_host->setPlaceholderText("127.0.0.1");
      _ssh_remote_host->setToolTip("Where MySQL runs, as seen FROM the SSH server. 127.0.0.1 = on the same server.");
      advanced_form->addRow("Database host on server", _ssh_remote_host);
      _ssh_remote_port = new QSpinBox(advanced);
      _ssh_remote_port->setRange(1, 65535);
      _ssh_remote_port->setValue(3306);
      advanced_form->addRow("Database port on server", _ssh_remote_port);
      _ssh_fingerprint = new QLineEdit(advanced);
      _ssh_fingerprint->setPlaceholderText("optional, e.g. SHA256:abc123...  (from your administrator)");
      _ssh_fingerprint->setToolTip("If set, Noggit only connects when the server's host key has exactly this "
                                   "fingerprint. If empty, you confirm the fingerprint the first time you connect.");
      advanced_form->addRow("Expected host key", _ssh_fingerprint);
      layout->addWidget(advanced);

      _ssh_status = new QLabel(_ssh_box);
      _ssh_status->setWordWrap(true);
      _ssh_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
      layout->addWidget(_ssh_status);

      auto* buttons = new QHBoxLayout();
      buttons->addStretch();
      _ssh_forget = new QPushButton("Forget trusted host key", _ssh_box);
      _ssh_forget->setToolTip("Only use this when your administrator confirms the server's key was changed "
                              "(e.g. the server was rebuilt).");
      _ssh_test = new QPushButton("Test SSH tunnel", _ssh_box);
      buttons->addWidget(_ssh_forget);
      buttons->addWidget(_ssh_test);
      layout->addLayout(buttons);

      // Under the MySQL fields, above the "built without MySQL" note and the MySQL test button.
      ui->verticalLayout_37->insertWidget(ui->verticalLayout_37->indexOf(ui->mysql_warning), _ssh_box);

#ifdef USE_MYSQL_UID_STORAGE
      // Don't show the database password in clear text.
      ui->_mysql_pwd_field->setEchoMode(QLineEdit::Password);

      connect(_ssh_box, &QGroupBox::toggled, this, [this] { update_direct_fields_enabled(); });
      connect(_ssh_test, &QPushButton::clicked, this, [this] { test_ssh_tunnel(); });
      connect(_ssh_forget, &QPushButton::clicked, this, [this]
      {
        save_changes();
        auto const config = Noggit::Ssh::TunnelConfig::fromProjectSettings();
        if (QMessageBox::warning(this, "Forget trusted host key",
              QString("Remove the trusted SSH host key for %1?\n\nOnly do this if your administrator confirmed that "
                      "the server's key changed. You will have to verify the new fingerprint.").arg(config.ssh_host),
              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        {
          return;
        }
        QString error;
        auto& tunnel = SshTunnelManager::instance();
        if (tunnel.config().project_id == config.project_id)
        {
          tunnel.stop();
        }
        if (!tunnel.forgetHostKeys(config, &error))
        {
          QMessageBox::critical(this, "Forget trusted host key", error);
        }
        update_ssh_status();
      });
      connect(&SshTunnelManager::instance(), &SshTunnelManager::stateChanged, this, [this] { update_ssh_status(); });
#else
      _ssh_box->setEnabled(false);
#endif
    }

    void settings::update_direct_fields_enabled()
    {
      // In tunnel mode the database is reached through the tunnel; the direct Server/Port are unused.
      bool const direct = !_ssh_box->isChecked();
      ui->_mysql_server_field->setEnabled(direct);
      ui->_mysql_port_field->setEnabled(direct);
      QString const tip = direct ? QString() : QString("Not used while the SSH tunnel is enabled (see \"Database host/port on server\").");
      ui->_mysql_server_field->setToolTip(tip);
      ui->_mysql_port_field->setToolTip(tip);
    }

    void settings::update_ssh_status()
    {
#ifdef USE_MYSQL_UID_STORAGE
      auto& tunnel = Noggit::Ssh::SshTunnelManager::instance();
      bool const ours = tunnel.config().project_id == Noggit::projectKeyId();
      _ssh_status->setText(ours ? tunnel.statusText() : QString("SSH tunnel: not running"));
#endif
    }

    void settings::set_db_test_running(bool running)
    {
      _db_test_running = running;
      ui->mysql_connect_test->setEnabled(!running);
      _ssh_test->setEnabled(!running);
      _ssh_forget->setEnabled(!running);
    }

    void settings::with_tunnel(bool explicit_restart, std::function<void()> on_connected)
    {
#ifdef USE_MYSQL_UID_STORAGE
      using Noggit::Ssh::SshTunnelManager;
      using State = SshTunnelManager::State;
      auto& tunnel = SshTunnelManager::instance();
      auto const config = Noggit::Ssh::TunnelConfig::fromProjectSettings();

      if (!config.enabled)
      {
        set_db_test_running(false);
        QMessageBox::information(this, "SSH tunnel", "Enable MySQL and the SSH tunnel for this project first.");
        return;
      }

      auto connection = std::make_shared<QMetaObject::Connection>();
      // Returns true once the attempt is over (connected, failed, or cancelled).
      auto handle = [this, &tunnel, config, on_connected, connection](State state) -> bool
      {
        switch (state)
        {
          case State::Starting:
          case State::Reconnecting:
            return false;
          case State::Connected:
            QObject::disconnect(*connection);
            on_connected();
            return true;
          case State::Disabled:
            QObject::disconnect(*connection);
            set_db_test_running(false);
            return true;
          case State::Failed:
            break;
        }

        QObject::disconnect(*connection);
        if (tunnel.lastError() == Noggit::Ssh::ErrorKind::HostKeyUnknown && !tunnel.pendingHostKeys().isEmpty())
        {
          if (Noggit::Ssh::promptTrustHostKey(this, config, tunnel.pendingHostKeys()))
          {
            QString error;
            if (tunnel.trustPendingHostKeys(&error))
            {
              // Retry outside this signal handler.
              QTimer::singleShot(0, this, [this, on_connected] { with_tunnel(true, on_connected); });
              return true;
            }
            QMessageBox::critical(this, "SSH tunnel", error);
          }
          set_db_test_running(false);
          return true;
        }

        set_db_test_running(false);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("SSH tunnel failed");
        box.setText(tunnel.lastErrorMessage());
        if (!tunnel.lastErrorDetails().isEmpty())
        {
          box.setDetailedText(tunnel.lastErrorDetails());
        }
        box.exec();
        return true;
      };

      set_db_test_running(true);
      if (explicit_restart)
      {
        tunnel.restart(config);
      }
      else
      {
        tunnel.start(config); // no-op if this project's tunnel is already up
      }
      if (!handle(tunnel.state()))
      {
        *connection = connect(&tunnel, &SshTunnelManager::stateChanged, this, [handle](State s) { handle(s); });
      }
#else
      Q_UNUSED(explicit_restart);
      Q_UNUSED(on_connected);
#endif
    }

    void settings::test_ssh_tunnel()
    {
      if (_db_test_running)
      {
        return;
      }
      save_changes();
      with_tunnel(true, [this]
      {
        set_db_test_running(false);
        QMessageBox::information(this, "SSH tunnel",
          "The SSH tunnel is working.\n\n" + Noggit::Ssh::SshTunnelManager::instance().statusText()
          + "\n\nUse \"Test MySQL connection\" to check the database login.");
      });
    }

    void settings::test_mysql_connection()
    {
      if (_db_test_running)
      {
        return;
      }
      save_changes();
#ifdef USE_MYSQL_UID_STORAGE
      if (!Noggit::mysqlSetting("enabled", false).toBool())
      {
        QMessageBox::information(this, "MySQL", "MySQL is disabled for this project. Tick the MySQL box first.");
        return;
      }

      bool const via_tunnel = Noggit::mysqlSetting(Noggit::Ssh::Keys::enabled(), false).toBool();
      auto run_probe = [this, via_tunnel]
      {
        auto& tunnel = Noggit::Ssh::SshTunnelManager::instance();
        mysql::ConnectionTarget target = mysql::currentConnectionTarget();
        if (via_tunnel)
        {
          target.host = "127.0.0.1";
          target.port = tunnel.localPort();
        }
        set_db_test_running(true);
        mysql::initClientLibrary();

        // mysql_real_connect blocks (up to its 5 s timeouts): run it off the UI thread.
        QPointer<settings> self(this);
        std::thread([self, target, via_tunnel]
        {
          mysql::ProbeResult const result = mysql::probeConnection(target);
          QMetaObject::invokeMethod(qApp, [self, result, via_tunnel, schema = target.schema]
          {
            if (!self)
            {
              return;
            }
            self->set_db_test_running(false);
            auto& tunnel = Noggit::Ssh::SshTunnelManager::instance();
            QString const detail = QString::fromStdString(result.error);
            QString text;
            switch (result.status)
            {
              case mysql::ProbeStatus::Ok:
                QMessageBox::information(self, "MySQL", QString("Successfully connected to the MySQL database%1.")
                                           .arg(via_tunnel ? " through the SSH tunnel" : ""));
                return;
              case mysql::ProbeStatus::Unreachable:
                if (via_tunnel)
                {
                  auto const config = tunnel.config();
                  text = QString("The SSH tunnel is working, but MySQL could not be reached at %1:%2 on the server. "
                                 "Check \"Database host/port on server\" and that MySQL is running there.")
                           .arg(config.remote_db_host).arg(config.remote_db_port);
                  if (tunnel.remoteForwardRefused())
                  {
                    text += "\n\nThe SSH server reported that it could not open that address (or is not allowed to).";
                  }
                }
                else
                {
                  text = "Could not reach the MySQL server. Check the server address and port, and that MySQL accepts connections from this computer.";
                }
                break;
              case mysql::ProbeStatus::AuthFailed:
                text = "The MySQL server rejected the login. Check the MySQL user and password.";
                break;
              case mysql::ProbeStatus::SchemaFailed:
                text = QString("Logged in to MySQL, but the World DB '%1' could not be opened or prepared. Check the "
                               "name and that the MySQL user has access to it.").arg(QString::fromStdString(schema));
                break;
              case mysql::ProbeStatus::Failed:
                text = "Connecting to MySQL failed.";
                break;
            }
            QMessageBox box(self);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle("MySQL connection failed");
            box.setText(text);
            box.setInformativeText(detail); // MySQL's own message; never contains the password
            box.exec();
          }, Qt::QueuedConnection);
        }).detach();
      };

      if (via_tunnel)
      {
        with_tunnel(false, run_probe);
      }
      else
      {
        run_probe();
      }
#endif
    }
  }
}
