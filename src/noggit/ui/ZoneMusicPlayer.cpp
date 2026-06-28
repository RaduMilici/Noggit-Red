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

    // NOTE: the QMediaPlayer (QtMultimedia backend) is created lazily in ensure_player() -- creating
    // it eagerly at map-open hung on some systems, and it's only needed once music actually plays.

    _silence_timer = new QTimer(this);
    _silence_timer->setSingleShot(true);

    _fade_timer = new QTimer(this);  // ~50ms steps -> ~1s fade
    connect(_fade_timer, &QTimer::timeout, [this]() { tick_fade(); });

    connect(_volume_slider, &QSlider::valueChanged, [this](int v)
            {
              _master_volume = v;
              if (_player && !_fade_timer->isActive()) { _player->setVolume(v); }
            });

    // Click a song to play it manually.
    connect(_song_list, &QListWidget::itemClicked, [this](QListWidgetItem* item)
            {
              if (item) { play_index(_song_list->row(item)); }
            });

    connect(_silence_timer, &QTimer::timeout, [this]() { if (_enabled) { play_random(); } });

    connect(_enable_check, &QCheckBox::toggled, [this](bool checked) { set_enabled(checked); });
  }

  void ZoneMusicPlayer::ensure_player()
  {
    if (_player)
    {
      return;
    }
    _player = new QMediaPlayer(this);
    _player->setVolume(_volume_slider->value());

    // When a track finishes, wait the authored silence interval then play another random one.
    connect(_player, &QMediaPlayer::mediaStatusChanged, [this](QMediaPlayer::MediaStatus status)
            {
              if (status == QMediaPlayer::EndOfMedia && _enabled)
              {
                schedule_next();
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
    _fade_pending_index = -1;
    if (_player)
    {
      _player->stop();
      _player->setVolume(_master_volume); // reset so a later manual play isn't stuck silent
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

    ensure_player();

    // If a track is already playing, fade it out first; start_track() runs when the fade-out ends.
    if (_player->state() == QMediaPlayer::PlayingState && _player->volume() > 0)
    {
      _silence_timer->stop();
      _fade_pending_index = index;
      _fade_timer->start(50);
    }
    else
    {
      start_track(index);
    }
  }

  void ZoneMusicPlayer::start_track(int index)
  {
    if (index < 0 || index >= static_cast<int>(_files.size()))
    {
      return;
    }

    _silence_timer->stop();
    ensure_player();

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

      // Drop the previous track's temp file.
      if (_temp_file)
      {
        _temp_file->deleteLater();
      }
      _temp_file = temp;

      _player->setVolume(0);  // start silent and fade in
      _player->setMedia(QUrl::fromLocalFile(temp->fileName()));
      _player->play();
    }
    catch (...)
    {
      LogError << "ZoneMusic: failed to load '" << path.str() << "'" << std::endl;
      return;
    }

    _now_playing = index;
    _song_list->setCurrentRow(index);

    _fade_pending_index = -1;  // fade IN
    _fade_timer->start(50);
  }

  void ZoneMusicPlayer::tick_fade()
  {
    if (!_player)
    {
      _fade_timer->stop();
      return;
    }

    int const step = std::max(1, _master_volume / 20); // ~1s fade (20 steps x 50ms)
    int v = _player->volume();

    if (_fade_pending_index >= 0)
    {
      // Fading OUT the current track, then start the pending one.
      v -= step;
      if (v <= 0)
      {
        _player->setVolume(0);
        _fade_timer->stop();
        int const idx = _fade_pending_index;
        _fade_pending_index = -1;
        start_track(idx);
        return;
      }
      _player->setVolume(v);
    }
    else
    {
      // Fading IN to the master volume.
      v += step;
      if (v >= _master_volume)
      {
        _player->setVolume(_master_volume);
        _fade_timer->stop();
      }
      else
      {
        _player->setVolume(v);
      }
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
