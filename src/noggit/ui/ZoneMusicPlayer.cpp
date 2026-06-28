// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/ZoneMusicPlayer.hpp>

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
#include <QtCore/QUrl>
#include <QtCore/QSettings>

#include <sstream>
#include <algorithm>
#include <cstdlib>

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
    vol_layout->addWidget(new QLabel(tr("Volume"), this));
    _volume_slider = new QSlider(Qt::Horizontal, this);
    _volume_slider->setRange(0, 100);
    _volume_slider->setValue(_master_volume);
    vol_layout->addWidget(_volume_slider);
    layout->addLayout(vol_layout);

    _song_list = new QListWidget(this);
    _song_list->setSelectionMode(QListWidget::SingleSelection);
    _song_list->setMinimumHeight(140);
    layout->addWidget(_song_list);

    // NOTE: the QMediaPlayer decks (QtMultimedia backend) are created lazily in ensure_deck() --
    // creating them eagerly at map-open hung on some systems, and they're only needed once music plays.

    _silence_timer = new QTimer(this);
    _silence_timer->setSingleShot(true);

    _fade_timer = new QTimer(this);  // ~50ms steps -> ~1s fade
    connect(_fade_timer, &QTimer::timeout, [this]() { tick_fade(); });

    connect(_volume_slider, &QSlider::valueChanged, [this](int v)
            {
              _master_volume = v;
              // When not mid-crossfade, apply straight to the live deck; otherwise tick_fade() ramps
              // toward the new _master_volume on its own.
              if (!_fade_timer->isActive() && _decks[_active_deck]) { _decks[_active_deck]->setVolume(v); }
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
      _enable_check->setChecked(settings.value("zone_music/enabled", false).toBool());
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

  void ZoneMusicPlayer::update_zone(int zone_music_id, bool is_day)
  {
    // Zone has NO music -> keep whatever is currently playing (do NOT stop/switch).
    if (zone_music_id <= 0)
    {
      return;
    }

    // Same music set (and day/night) -> let the current track keep playing.
    if (zone_music_id == _current_zone_music_id && is_day == _is_day)
    {
      return;
    }

    // New zone music -> switch to it.
    _current_zone_music_id = zone_music_id;
    _is_day = is_day;

    rebuild_playlist();

    if (_enabled && !_files.empty())
    {
      _silence_timer->stop();
      play_random();
    }
  }

  void ZoneMusicPlayer::rebuild_playlist()
  {
    _files.clear();
    _dir.clear();
    _song_list->clear();
    _now_playing = -1;
    _silence_min_ms = 0;
    _silence_max_ms = 0;

    std::string zone_name = "-";
    int sound_entry_id = 0;

    try
    {
      if (_current_zone_music_id > 0 && gZoneMusicDB.CheckIfIdExists(_current_zone_music_id))
      {
        auto const zm = gZoneMusicDB.getByID(_current_zone_music_id);
        zone_name = zm.getString(ZoneMusicDB::Name);
        sound_entry_id = static_cast<int>(zm.getUInt(_is_day ? ZoneMusicDB::DayMusic : ZoneMusicDB::NightMusic));
        _silence_min_ms = static_cast<int>(zm.getUInt(_is_day ? ZoneMusicDB::SilenceIntervalMinDay
                                                              : ZoneMusicDB::SilenceIntervalMinNight));
        _silence_max_ms = static_cast<int>(zm.getUInt(_is_day ? ZoneMusicDB::SilenceIntervalMaxDay
                                                              : ZoneMusicDB::SilenceIntervalMaxNight));
      }
    }
    catch (...)
    {
      sound_entry_id = 0;
    }

    _zone_label->setText(tr("Zone: %1").arg(QString::fromStdString(zone_name)));

    try
    {
      if (sound_entry_id > 0 && gSoundEntriesDB.CheckIfIdExists(sound_entry_id))
      {
        auto const se = gSoundEntriesDB.getByID(sound_entry_id);
        _dir = se.getString(SoundEntriesDB::FilePath);

        for (int i = 0; i < 10; ++i)
        {
          std::string const fn = se.getString(SoundEntriesDB::Filenames + i);
          if (!fn.empty())
          {
            _files.push_back(fn);
            _song_list->addItem(QString::fromStdString(fn));
          }
        }
      }
    }
    catch (...)
    {
      _files.clear();
    }
  }

  void ZoneMusicPlayer::play_random()
  {
    if (_files.empty())
    {
      return;
    }
    std::uniform_int_distribution<int> dist(0, static_cast<int>(_files.size()) - 1);
    play_index(dist(_rng));
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
    path << _dir << "\\" << _files[index];

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
      temp->write(file.getBuffer(), file.getSize());
      temp->close();
      temp->rename(temp->fileName() + QString::fromStdString(_files[index]));

      // Drop the idle deck's previous temp file (that track already finished fading out).
      if (_deck_files[idle])
      {
        _deck_files[idle]->deleteLater();
      }
      _deck_files[idle] = temp;

      _decks[idle]->setVolume(0);  // start silent and fade in over the old deck
      _decks[idle]->setMedia(QUrl::fromLocalFile(temp->fileName()));
      _decks[idle]->play();
    }
    catch (...)
    {
      LogError << "ZoneMusic: failed to load '" << path.str() << "'" << std::endl;
      return;
    }

    // The idle deck is now the live one; the previously-live deck becomes the one we fade out.
    _active_deck = idle;
    _now_playing = index;
    _song_list->setCurrentRow(index);
    _track_ending = false;  // fresh track -> allow the end-of-track watchdog to fire again

    _fade_timer->start(50); // ~1s crossfade
  }

  void ZoneMusicPlayer::tick_fade()
  {
    int const step = std::max(1, _master_volume / 20); // ~1s fade (20 steps x 50ms)

    auto* in_deck = _decks[_active_deck];
    auto* out_deck = _decks[1 - _active_deck];

    // Fade the live deck IN toward the master volume.
    bool in_done = true;
    if (in_deck)
    {
      int v = in_deck->volume();
      if (v < _master_volume)
      {
        v = std::min(_master_volume, v + step);
        in_deck->setVolume(v);
        in_done = (v >= _master_volume);
      }
    }

    // Fade the previous deck OUT, then stop it once silent so it's free for the next switch.
    bool out_done = true;
    if (out_deck)
    {
      int v = out_deck->volume();
      if (v > 0)
      {
        v = std::max(0, v - step);
        out_deck->setVolume(v);
        out_done = (v <= 0);
      }
      if (out_done && out_deck->state() != QMediaPlayer::StoppedState)
      {
        out_deck->stop();
      }
    }

    if (in_done && out_done)
    {
      if (in_deck)
      {
        in_deck->setVolume(_master_volume);
      }
      _fade_timer->stop();
    }
  }

  void ZoneMusicPlayer::schedule_next()
  {
    // Clamp the authored silence interval to a sane range so a new song ALWAYS reliably follows the
    // one that just ended (garbage / very large DBC values won't leave it silent indefinitely).
    int const lo = std::clamp(_silence_min_ms, 0, 15000);
    int const hi = std::clamp(_silence_max_ms, lo, 15000);
    int wait_ms = lo;
    if (hi > lo)
    {
      std::uniform_int_distribution<int> dist(lo, hi);
      wait_ms = dist(_rng);
    }
    _silence_timer->start(wait_ms);
  }
}
