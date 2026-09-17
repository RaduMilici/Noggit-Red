// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/ZoneMusicPlayer.hpp>

#include <noggit/ui/SfxPlayer.hpp>
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QSlider>
#include <QtWidgets/QListWidget>
#include <QtCore/QTemporaryFile>
#include <noggit/audio/QtMediaTemp.hpp>
#include <QtCore/QUrl>
#include <QtCore/QSettings>
#include <QtCore/QCoreApplication>

#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace Noggit::Ui
{
  ZoneMusicPlayer::ZoneMusicPlayer(QWidget* parent)
    : QWidget(parent)
    , _rng(std::random_device{}())
  {
    setMinimumWidth(280);

    auto* layout = new QVBoxLayout(this);

    _enable_check = new QCheckBox(tr("Enable zone music"), this);
    _enable_check->setChecked(false);
    layout->addWidget(_enable_check);

    _zone_label = new QLabel(tr("Zone: -"), this);
    layout->addWidget(_zone_label);

    auto* vol_layout = new QHBoxLayout();
    vol_layout->addWidget(new QLabel(tr("Music"), this));
    _volume_slider = new QSlider(Qt::Horizontal, this);
    _volume_slider->setRange(0, 100);
    _volume_slider->setValue(_master_volume);
    vol_layout->addWidget(_volume_slider);
    layout->addLayout(vol_layout);

    // Effects channel volume (footsteps / fidgets / event one-shots -- the client's
    // SoundVolume CVar analogue; playback via SfxPlayer, doc 38)
    {
      auto* fx_layout = new QHBoxLayout();
      fx_layout->addWidget(new QLabel(tr("Effects"), this));
      auto* fx_slider = new QSlider(Qt::Horizontal, this);
      fx_slider->setRange(0, 100);
      fx_slider->setValue(Noggit::Ui::SfxPlayer::instance().volume());
      fx_layout->addWidget(fx_slider);
      layout->addLayout(fx_layout);
      connect(fx_slider, &QSlider::valueChanged, [](int v)
              {
                Noggit::Ui::SfxPlayer::instance().set_volume(v);
                QSettings().setValue("zone_music/effects_volume", v);
              });
    }

    // Ambience channel volume (the client's separate AmbienceVolume CVar analogue, doc 38)
    auto* amb_layout = new QHBoxLayout();
    amb_layout->addWidget(new QLabel(tr("Ambience"), this));
    _ambience_slider = new QSlider(Qt::Horizontal, this);
    _ambience_slider->setRange(0, 100);
    _ambience_slider->setValue(_amb_volume);
    amb_layout->addWidget(_ambience_slider);
    layout->addLayout(amb_layout);
    connect(_ambience_slider, &QSlider::valueChanged, [this](int v)
            {
              _amb_volume = v;
              // live-apply to the sounding deck when not mid-crossfade (the fade tick otherwise
              // ramps toward the recomputed target on its own)
              _amb_target_volume = std::clamp(static_cast<int>(std::lround(_amb_dbc_vol * v)), 0, 100);
              if (!_amb_fade_timer->isActive() && _amb_decks[_amb_active])
              {
                _amb_decks[_amb_active]->setVolume(_amb_target_volume);
              }
              QSettings().setValue("zone_music/ambience_volume", v);
            });

    _song_list = new QListWidget(this);
    _song_list->setSelectionMode(QListWidget::SingleSelection);
    _song_list->setMinimumHeight(140);
    layout->addWidget(_song_list);

    // NOTE: the QMediaPlayer decks (QtMultimedia backend) are created lazily in ensure_deck() --
    // creating them eagerly at map-open hung on some systems, and they're only needed once music plays.

    _silence_timer = new QTimer(this);
    _silence_timer->setSingleShot(true);

    _fade_timer = new QTimer(this);  // music fade-out ticker (wall-clock t inside)
    connect(_fade_timer, &QTimer::timeout, [this]() { tick_fade(); });

    _amb_fade_timer = new QTimer(this); // ambience 5.0 s crossfade ticker (wall-clock t inside)
    connect(_amb_fade_timer, &QTimer::timeout, [this]() { tick_ambience_fade(); });

    connect(_volume_slider, &QSlider::valueChanged, [this](int v)
            {
              _master_volume = v;
              // When not mid-crossfade, apply straight to the live deck; otherwise tick_fade() ramps
              // toward the new _master_volume on its own.
              if (!_fade_timer->isActive() && _decks[_active_deck]) { _decks[_active_deck]->setVolume(effective_master()); }
              QSettings().setValue("zone_music/volume", v);
            });

    // Click a song to play it manually.
    connect(_song_list, &QListWidget::itemClicked, [this](QListWidgetItem* item)
            {
              if (item) { play_index(_song_list->row(item)); }
            });

    connect(_silence_timer, &QTimer::timeout, [this]() { if (_enabled) { play_random(); } });

    connect(_enable_check, &QCheckBox::toggled, [this](bool checked)
            {
              set_enabled(checked);
              QSettings().setValue("zone_music/enabled", checked);
            });

    // Restore the volume + enabled state saved from the previous session. Applying these AFTER the
    // signals are wired lets setChecked() drive set_enabled() so playback resumes when the next
    // update_zone() (from MapView::tick) arrives. Re-saving the same values here is harmless.
    {
      QSettings settings;
      int const saved_volume = std::clamp(settings.value("zone_music/volume", _master_volume).toInt(), 0, 100);
      _master_volume = saved_volume;
      _volume_slider->setValue(saved_volume);
      int const saved_amb = std::clamp(settings.value("zone_music/ambience_volume", _amb_volume).toInt(), 0, 100);
      _amb_volume = saved_amb;
      _ambience_slider->setValue(saved_amb);
      _enable_check->setChecked(settings.value("zone_music/enabled", false).toBool());
    }

    // Tear the audio backend down BEFORE the app exits. Qt5's Windows QMediaPlayer runs a DirectShow
    // filter-graph thread; if a deck is still Playing at process teardown that thread can keep the
    // process alive (and audio looping) in the background = "Noggit stays open playing music after
    // close". aboutToQuit fires while the event loop is still running, so the stop + media release
    // actually get processed. (Destructor does the same as a fallback for a plain widget teardown.)
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { shutdown_audio(); });
  }

  ZoneMusicPlayer::~ZoneMusicPlayer()
  {
    shutdown_audio();
  }

  void ZoneMusicPlayer::shutdown_audio()
  {
    _enabled = false;
    if (_silence_timer) { _silence_timer->stop(); }
    if (_fade_timer) { _fade_timer->stop(); }
    if (_amb_fade_timer) { _amb_fade_timer->stop(); }
    for (int i = 0; i < DECK_COUNT; ++i)
    {
      if (_decks[i])
      {
        _decks[i]->stop();
        // Releasing the media unhooks the DirectShow graph and the temp-file handle, which lets the
        // backend thread finish -- a bare stop() alone can leave it running on exit.
        _decks[i]->setMedia(QMediaContent());
      }
      if (_amb_decks[i])
      {
        _amb_decks[i]->stop();
        _amb_decks[i]->setMedia(QMediaContent());
      }
    }
  }

  void ZoneMusicPlayer::ensure_deck(int deck)
  {
    if (deck < 0 || deck >= DECK_COUNT || _decks[deck])
    {
      return;
    }
    auto* player = new QMediaPlayer(this);
    player->setVolume(0);
    player->setNotifyInterval(250); // tighter positionChanged cadence for the end-of-track watchdog
    _decks[deck] = player;

    auto on_track_ended = [this, deck]()
            {
              // Only the live deck's end advances the playlist (the other deck is the one fading out).
              if (!_enabled || _track_ending || deck != _active_deck)
              {
                return;
              }
              _track_ending = true; // start_track() resets this for the next track
              schedule_next();
            };

    // When a track finishes, wait the authored silence interval then play another random one.
    connect(player, &QMediaPlayer::mediaStatusChanged, [on_track_ended](QMediaPlayer::MediaStatus status)
            {
              if (status == QMediaPlayer::EndOfMedia)
              {
                on_track_ended();
              }
            });

    // Surface decode/playback failures (QMediaPlayer reports these asynchronously, not via our exists()
    // check) so a track that "plays" silently is visible in the log.
    connect(player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error),
            [player, deck](QMediaPlayer::Error err)
            {
              LogError << "ZMUSIC deck=" << deck << " playback error " << static_cast<int>(err)
                       << " '" << player->errorString().toStdString() << "'" << std::endl;
            });

    // Fallback: Qt5's QMediaPlayer (WMF backend on Windows) doesn't always emit EndOfMedia, so also
    // watch the playback position approaching the duration. The _track_ending guard keeps both paths
    // from scheduling twice for the same track.
    connect(player, &QMediaPlayer::positionChanged, [this, deck, on_track_ended](qint64 pos)
            {
              auto* p = _decks[deck];
              qint64 const dur = p ? p->duration() : 0;
              if (dur > 0 && pos >= dur - 250 && p->state() != QMediaPlayer::StoppedState)
              {
                on_track_ended();
              }
            });
  }

  void ZoneMusicPlayer::set_enabled(bool enabled)
  {
    if (_enabled == enabled)
    {
      return;
    }
    _enabled = enabled;

    if (_enabled)
    {
      // Force the next update_zone() (driven from MapView::tick, only while enabled) to (re)build the
      // playlist for the current zone and start a track.
      _current_zone_music_id = -1;
    }
    else
    {
      stop_playback();
    }
  }

  void ZoneMusicPlayer::stop_playback()
  {
    _silence_timer->stop();
    _fade_timer->stop();
    // NOTE: the AMBIENCE channel is NOT stopped here -- it is independent of the music enable
    // (its own gate is the Ambience slider; full teardown stays in shutdown_audio).
    for (int i = 0; i < DECK_COUNT; ++i)
    {
      if (_decks[i])
      {
        _decks[i]->stop();
        _decks[i]->setVolume(0);
      }
    }
    _now_playing = -1;
    _song_list->clearSelection();
  }

  void ZoneMusicPlayer::update_zone(int zone_music_id, int intro_music_id, bool is_day)
  {
    // Zone has NEITHER looping music NOR an intro -> keep whatever is currently playing (don't switch).
    if (zone_music_id <= 0 && intro_music_id <= 0)
    {
      return;
    }

    // Same music set + intro + day/night -> let the current track keep playing.
    if (zone_music_id == _current_zone_music_id && intro_music_id == _current_intro_music_id
        && is_day == _is_day)
    {
      return;
    }

    // New zone music/intro -> switch to it.
    bool const intro_changed = (intro_music_id != _current_intro_music_id);
    _current_zone_music_id = zone_music_id;
    _current_intro_music_id = intro_music_id;
    _is_day = is_day;

    rebuild_playlist();

    if (_enabled)
    {
      _silence_timer->stop();
      // CLIENT-FAITHFUL: the zone INTRO fires ONCE on entry (if off its MinDelayMinutes cooldown), at
      // full volume, as a one-shot before the looping zone music. Play it first; when it ends the
      // normal silence -> random-loop-track flow takes over. Otherwise hard-start a looping track.
      if (_intro_index >= 0 && intro_changed && intro_off_cooldown(_current_intro_music_id))
      {
        _intro_last_played[_current_intro_music_id] = QDateTime::currentMSecsSinceEpoch();
        play_index(_intro_index);
      }
      else if (!_files.empty())
      {
        play_random();
      }
    }
  }

  bool ZoneMusicPlayer::intro_off_cooldown(int intro_id) const
  {
    auto const it = _intro_last_played.find(intro_id);
    if (it == _intro_last_played.end()) { return true; } // never played this session
    return (QDateTime::currentMSecsSinceEpoch() - it->second) >= _intro_min_delay_ms;
  }

  void ZoneMusicPlayer::rebuild_playlist()
  {
    _files.clear();
    _dirs.clear();
    _song_list->clear();
    _now_playing = -1;
    _silence_min_ms = 0;
    _silence_max_ms = 0;
    _intro_index = -1;
    _intro_min_delay_ms = 0;

    std::string zone_name = "-";
    int day_sound_id = 0, night_sound_id = 0;

    try
    {
      if (_current_zone_music_id > 0 && gZoneMusicDB.CheckIfIdExists(_current_zone_music_id))
      {
        auto const zm = gZoneMusicDB.getByID(_current_zone_music_id);
        zone_name = zm.getString(ZoneMusicDB::Name);
        // Play ALL of the zone's music -- BOTH the Day and Night SoundEntries -- so night tracks show in
        // the list and enter the random rotation too (no day/night segregation, per request).
        day_sound_id = static_cast<int>(zm.getUInt(ZoneMusicDB::DayMusic));
        night_sound_id = static_cast<int>(zm.getUInt(ZoneMusicDB::NightMusic));
        _silence_min_ms = static_cast<int>(zm.getUInt(_is_day ? ZoneMusicDB::SilenceIntervalMinDay
                                                              : ZoneMusicDB::SilenceIntervalMinNight));
        _silence_max_ms = static_cast<int>(zm.getUInt(_is_day ? ZoneMusicDB::SilenceIntervalMaxDay
                                                              : ZoneMusicDB::SilenceIntervalMaxNight));
      }
    }
    catch (...)
    {
      day_sound_id = night_sound_id = 0;
    }

    // Add every non-empty file of a SoundEntries row to the playlist, each carrying its OWN directory
    // (SoundEntries.DirectoryBase), with a UI label prefix.
    auto add_sound_entry = [this](int se_id, char const* prefix)
    {
      try
      {
        if (se_id <= 0 || !gSoundEntriesDB.CheckIfIdExists(se_id)) { return; }
        auto const se = gSoundEntriesDB.getByID(se_id);
        std::string const dir = se.getString(SoundEntriesDB::FilePath);
        for (int i = 0; i < 10; ++i)
        {
          std::string const fn = se.getString(SoundEntriesDB::Filenames + i);
          if (!fn.empty())
          {
            _files.push_back(fn);
            _dirs.push_back(dir);
            _song_list->addItem(QString::fromStdString(std::string(prefix) + fn));
          }
        }
      }
      catch (...) {}
    };

    // 1) Looping zone music: BOTH the Day and Night SoundEntries (deduped when a zone reuses the same set
    //    for day and night), so every song for the zone plays and appears in the list.
    add_sound_entry(day_sound_id, "");
    if (night_sound_id != day_sound_id) { add_sound_entry(night_sound_id, ""); }

    // 2) Zone INTRO music (ZoneIntroMusicTable -> SoundEntries): city/instance intros such as
    //    "IronForge Intro.mp3" and "cot_intro.mp3" live on IntroSound, not ZoneMusic -- this is what
    //    was missing, so they never showed in the list or played. Labelled "[Intro]".
    int intro_sound_id = 0;
    try
    {
      if (_current_intro_music_id > 0 && gZoneIntroMusicTableDB.CheckIfIdExists(_current_intro_music_id))
      {
        auto const zi = gZoneIntroMusicTableDB.getByID(_current_intro_music_id);
        intro_sound_id = static_cast<int>(zi.getUInt(ZoneIntroMusicTableDB::SoundId));
        // MinDelayMinutes -> ms cooldown (client: FUN_00461440 blocks replay for MinDelayMinutes*60000).
        _intro_min_delay_ms = static_cast<int>(zi.getUInt(ZoneIntroMusicTableDB::MinDelayMinutes)) * 60000;
        if (std::getenv("NOGGIT_MUSIC_DEBUG")) // TEMP 2026-07-25
        {
          LogError << "MUSICDBG intro id=" << _current_intro_music_id
                   << " name='" << zi.getString(ZoneIntroMusicTableDB::Name) << "'"
                   << " soundId=" << intro_sound_id
                   << " minDelayMin=" << zi.getUInt(ZoneIntroMusicTableDB::MinDelayMinutes)
                   << " priority=" << zi.getUInt(ZoneIntroMusicTableDB::Priority) << std::endl;
        }
        if (zone_name == "-" || zone_name.empty())
        {
          zone_name = zi.getString(ZoneIntroMusicTableDB::Name); // label the zone even if it has intro-only music
        }
      }
    }
    catch (...)
    {
      intro_sound_id = 0;
    }
    int const before_intro = static_cast<int>(_files.size());
    add_sound_entry(intro_sound_id, "[Intro] ");
    if (static_cast<int>(_files.size()) > before_intro)
    {
      _intro_index = before_intro; // one-shot on entry; excluded from the random loop rotation
    }

    _zone_label->setText(tr("Zone: %1").arg(QString::fromStdString(zone_name)));
  }

  void ZoneMusicPlayer::play_random()
  {
    // Random LOOP track only -- the intro is a one-shot fired on zone entry, never in the rotation.
    std::vector<int> pool;
    for (int i = 0; i < static_cast<int>(_files.size()); ++i)
    {
      if (i != _intro_index) { pool.push_back(i); }
    }
    if (pool.empty())
    {
      return;
    }
    // Don't replay the track that just finished back-to-back. Only drop it when there's an alternative,
    // so a zone with a single looping track still plays.
    if (pool.size() > 1 && _now_playing >= 0)
    {
      pool.erase(std::remove(pool.begin(), pool.end(), _now_playing), pool.end());
      if (pool.empty()) { return; }
    }
    std::uniform_int_distribution<int> dist(0, static_cast<int>(pool.size()) - 1);
    play_index(pool[dist(_rng)]);
  }

  void ZoneMusicPlayer::play_index(int index)
  {
    if (index < 0 || index >= static_cast<int>(_files.size()))
    {
      return;
    }
    start_track(index);
  }

  void ZoneMusicPlayer::start_track(int index)
  {
    if (index < 0 || index >= static_cast<int>(_files.size()))
    {
      return;
    }

    _silence_timer->stop();

    // Load the new track onto the IDLE deck so the live deck keeps playing while the new one fades
    // in over it.
    int const idle = 1 - _active_deck;
    ensure_deck(idle);

    std::stringstream path;
    path << _dirs[index] << "\\" << _files[index];

    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    if (!client_data || !client_data->exists(path.str()))
    {
      LogError << "ZoneMusic: file not found '" << path.str() << "'" << std::endl;
      return;
    }

    try
    {
      BlizzardArchive::ClientFile file(path.str(), client_data);

      // Extract to a temp file with the original extension so QMediaPlayer recognises the format.
      auto* temp = new QTemporaryFile(this);
      if (!temp->open())
      {
        delete temp;
        return;
      }
      std::string media_name = _files[index];
      auto const bytes = Noggit::Audio::decodeForQtMedia(file.getBuffer(), file.getSize(), media_name);
      temp->write(bytes.data(), static_cast<qint64>(bytes.size()));
      temp->close();
      temp->rename(temp->fileName() + QString::fromStdString(media_name));

      // Drop the idle deck's previous temp file (that track already finished fading out).
      if (_deck_files[idle])
      {
        _deck_files[idle]->deleteLater();
      }
      _deck_files[idle] = temp;

      // CLIENT-FAITHFUL: the new track HARD-STARTS at full volume -- the 1.12 client sets the stream
      // volume instantly (FSOUND_SetVolume) with NO fade-in. Only the previous deck fades out.
      _decks[idle]->setVolume(effective_master());
      _decks[idle]->setMedia(QUrl::fromLocalFile(temp->fileName()));
      _decks[idle]->play();
    }
    catch (...)
    {
      LogError << "ZoneMusic: failed to load '" << path.str() << "'" << std::endl;
      return;
    }

    // The idle deck is now the live one; the previously-live deck becomes the one we fade out (4.0 s).
    _active_deck = idle;
    _now_playing = index;
    _song_list->setCurrentRow(index);
    _track_ending = false;  // fresh track -> allow the end-of-track watchdog to fire again

    _fade_from_volume = (_decks[1 - _active_deck] ? _decks[1 - _active_deck]->volume() : 0);
    _fade_start_ms = QDateTime::currentMSecsSinceEpoch(); // wall-clock ramp (see hpp note)
    _fade_timer->start(FADE_TICK_MS); // ramps the OUTGOING deck to silence over FADE_OUT_MS
  }

  void ZoneMusicPlayer::tick_fade()
  {
    // CLIENT-FAITHFUL: only the OUTGOING deck ramps (to silence over FADE_OUT_MS = 4.0 s). The live
    // deck holds at full volume the whole time -- the 1.12 client does NOT fade music in.
    QMediaPlayer* const in_deck  = _decks[_active_deck];
    QMediaPlayer* const out_deck = _decks[1 - _active_deck];

    if (in_deck && in_deck->volume() != effective_master())
    {
      in_deck->setVolume(effective_master()); // hold live deck at full (guards against slider drift)
    }

    // WALL-CLOCK t (2026-08-27): counted ticks stepped AND stretched the fade whenever tile loads
    // stalled the UI thread (exactly the zone-change moment). Real elapsed time keeps the ramp on
    // the client's 4.0 s line no matter how late the timer fires.
    float const t = std::min(1.0f,
                             static_cast<float>(QDateTime::currentMSecsSinceEpoch() - _fade_start_ms)
                               / static_cast<float>(FADE_OUT_MS));
    if (out_deck)
    {
      // CLIENT-EXACT curve: a LINEAR amplitude ramp. The 1.12 client fades in FMOD's 0-255 volume with
      // step = 255/duration_ms (RE: FUN_007a5a50, rate const 1000 = ms/sec) -- i.e. a straight
      // amplitude decrement per millisecond. (An earlier logarithmic curve LINGERED at low volume,
      // which is where QMediaPlayer's coarse 0-100 steps are most audible -> the "cut out at the end".
      // The real linear ramp passes through the low tail quickly, so 50 Hz updates read smooth.)
      int const v = std::max(0, static_cast<int>(std::lround(_fade_from_volume * (1.0f - t))));
      out_deck->setVolume(v);
      if (t >= 1.0f && out_deck->state() != QMediaPlayer::StoppedState)
      {
        out_deck->stop();
      }
    }
    if (t >= 1.0f)
    {
      _fade_timer->stop();
    }
  }

  void ZoneMusicPlayer::schedule_next()
  {
    // Silence between tracks = ZoneMusic.dbc SilenceIntervalMin..Max; when the DBC authors NO interval
    // the client falls back to a 6000 ms gap (RE: FUN_004601f0 "+6000"). Clamp to a sane ceiling so a
    // garbage DBC value can't leave it silent forever.
    int lo = _silence_min_ms;
    int hi = _silence_max_ms;
    if (lo <= 0 && hi <= 0)
    {
      lo = hi = DEFAULT_SILENCE_MS;
    }
    lo = std::clamp(lo, 0, 30000);
    hi = std::clamp(hi, lo, 30000);
    int wait_ms = lo;
    if (hi > lo)
    {
      std::uniform_int_distribution<int> dist(lo, hi);
      wait_ms = dist(_rng);
    }
    _silence_timer->start(wait_ms);
  }

  // ===== ZONE AMBIENCE channel (client law RE'd 2026-08-27, doc 38) =====

  void ZoneMusicPlayer::ensure_amb_deck(int deck)
  {
    if (deck < 0 || deck >= 2 || _amb_decks[deck])
    {
      return;
    }
    auto* p = new QMediaPlayer(this);
    p->setVolume(0);
    _amb_decks[deck] = p;
    // Ambience LOOPS: the client opens the stream in loop mode; QMediaPlayer has no loop flag,
    // so restart the same media on end (only while this deck is the live one).
    connect(p, &QMediaPlayer::mediaStatusChanged, [this, deck](QMediaPlayer::MediaStatus s)
            {
              if (s == QMediaPlayer::EndOfMedia && _enabled && deck == _amb_active && _amb_decks[deck])
              {
                _amb_decks[deck]->setPosition(0);
                _amb_decks[deck]->play();
              }
            });
  }

  int ZoneMusicPlayer::underwater_ambience_entry()
  {
    if (_underwater_ambience_se != -2)
    {
      return _underwater_ambience_se;
    }
    // The client selects the submerged-listener ambience BY NAME from SoundEntries
    // (FUN_004609b0: "Underwater (DONOTRENAME)"). Resolve once, cache forever.
    _underwater_ambience_se = 0;
    try
    {
      for (DBCFile::Iterator it = gSoundEntriesDB.begin(); it != gSoundEntriesDB.end(); ++it)
      {
        if (std::string(it->getString(SoundEntriesDB::Name)) == "Underwater (DONOTRENAME)")
        {
          _underwater_ambience_se = static_cast<int>(it->getUInt(SoundEntriesDB::ID));
          break;
        }
      }
    }
    catch (...) {}
    return _underwater_ambience_se;
  }

  void ZoneMusicPlayer::update_ambience(int sound_ambience_id, bool is_day, bool underwater)
  {
    // INDEPENDENT of the music enable (user 2026-08-27 "no ambience/birds": the checkbox only
    // covers music; the client runs ambience on its own CVar). The Ambience slider is the
    // gate: 0 = off (fades the current loop out through the normal 5 s path).
    if (_amb_volume <= 0)
    {
      if (_current_ambience_se != 0)
      {
        _current_ambience_se = 0;
        start_ambience_entry(0);
      }
      return;
    }
    // Chooser (client FUN_00460bd0 priority; the ghost/forced lanes don't exist in noggit):
    // submerged listener -> the "Underwater (DONOTRENAME)" row, else the zone SoundAmbience
    // row's Day(+1)/Night(+2) SoundEntries (3-column table {ID, Day, Night}, dump-verified).
    int wanted = 0;
    if (underwater)
    {
      wanted = underwater_ambience_entry();
    }
    if (wanted == 0 && sound_ambience_id > 0)
    {
      try
      {
        if (gSoundAmbienceDB.CheckIfIdExists(sound_ambience_id))
        {
          auto const row = gSoundAmbienceDB.getByID(sound_ambience_id);
          wanted = static_cast<int>(row.getUInt(is_day ? 1 : 2));
        }
      }
      catch (...) {}
    }
    if (wanted == _current_ambience_se)
    {
      return; // client law: an unchanged record never restarts (FUN_00460b00 step 2)
    }
    _current_ambience_se = wanted;
    start_ambience_entry(wanted);
  }

  void ZoneMusicPlayer::start_ambience_entry(int se_id)
  {
    // Begin the client's 5.0 s crossfade: whatever plays on the active deck becomes the
    // fading-out deck; the new record opens at volume 0 on the idle deck and ramps up.
    _amb_fade_start_ms = QDateTime::currentMSecsSinceEpoch();
    _amb_fade_from = (_amb_decks[_amb_active] ? _amb_decks[_amb_active]->volume() : 0);
    int const idle = 1 - _amb_active;
    ensure_amb_deck(idle);
    _amb_target_volume = 0;
    _amb_dbc_vol = 1.0f;

    if (se_id > 0)
    {
      try
      {
        auto const se = gSoundEntriesDB.getByID(se_id);
        std::string const dir = se.getString(SoundEntriesDB::FilePath);
        std::vector<std::string> files;
        for (int i = 0; i < 10; ++i)
        {
          std::string const fn = se.getString(SoundEntriesDB::Filenames + i);
          if (!fn.empty())
          {
            files.push_back(fn);
          }
        }
        if (!files.empty())
        {
          // like the client stream open: pick one of the row's files, then LOOP it
          int pick = 0;
          if (files.size() > 1)
          {
            std::uniform_int_distribution<int> dist(0, static_cast<int>(files.size()) - 1);
            pick = dist(_rng);
          }
          std::string const path = dir + "\\" + files[pick];
          auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
          if (client_data && client_data->exists(path))
          {
            BlizzardArchive::ClientFile file(path, client_data);
            auto* temp = new QTemporaryFile(this);
            if (temp->open())
            {
              std::string media_name = files[pick];
              auto const bytes = Noggit::Audio::decodeForQtMedia(file.getBuffer(), file.getSize(), media_name);
              temp->write(bytes.data(), static_cast<qint64>(bytes.size()));
              temp->close();
              temp->rename(temp->fileName() + QString::fromStdString(media_name));
              if (_amb_files[idle])
              {
                _amb_files[idle]->deleteLater();
              }
              _amb_files[idle] = temp;
              // client target volume = the SoundEntries row's authored Volume (FUN_00460b00
              // ramps to SOUNDDEFINITION +0x78 = that field), scaled by our ambience slider
              _amb_dbc_vol = se.getFloat(SoundEntriesDB::Volume);
              if (!(_amb_dbc_vol > 0.0f) || _amb_dbc_vol > 4.0f)
              {
                _amb_dbc_vol = 1.0f; // garbage/unauthored volume -> full
              }
              _amb_target_volume = std::clamp(static_cast<int>(std::lround(_amb_dbc_vol * _amb_volume)), 0, 100);
              _amb_decks[idle]->setVolume(0); // client: instant 0, then the 5 s ramp to target
              _amb_decks[idle]->setMedia(QUrl::fromLocalFile(temp->fileName()));
              _amb_decks[idle]->play();
            }
            else
            {
              delete temp;
            }
          }
          else
          {
            LogError << "Ambience: file not found '" << path << "'" << std::endl;
          }
        }
      }
      catch (...) {}
    }

    _amb_active = idle;
    _amb_fade_timer->start(FADE_TICK_MS);
  }

  void ZoneMusicPlayer::tick_ambience_fade()
  {
    // Wall-clock t over the client's 5.0 s (FUN_00460b00 / FUN_007a5a10(5.0) + FUN_007a57b0(5.0)):
    // both decks ramp linearly; late timer ticks (tile-load stalls) still land ON the line.
    float const t = std::min(1.0f,
                             static_cast<float>(QDateTime::currentMSecsSinceEpoch() - _amb_fade_start_ms)
                               / 5000.0f);
    QMediaPlayer* const in_deck = _amb_decks[_amb_active];
    QMediaPlayer* const out_deck = _amb_decks[1 - _amb_active];
    if (in_deck)
    {
      in_deck->setVolume(static_cast<int>(std::lround(_amb_target_volume * t)));
    }
    if (out_deck)
    {
      out_deck->setVolume(std::max(0, static_cast<int>(std::lround(_amb_fade_from * (1.0f - t)))));
      if (t >= 1.0f && out_deck->state() != QMediaPlayer::StoppedState)
      {
        out_deck->stop();
      }
    }
    if (t >= 1.0f)
    {
      _amb_fade_timer->stop();
    }
  }
}
