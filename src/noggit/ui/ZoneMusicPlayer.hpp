// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QtWidgets/QWidget>
#include <QtMultimedia/QMediaPlayer>
#include <QtCore/QTimer>

#include <string>
#include <vector>
#include <random>

class QSlider;
class QListWidget;
class QLabel;
class QCheckBox;
class QTemporaryFile;

namespace Noggit::Ui
{
  // Plays a zone's background music like the client: as the camera crosses zone boundaries the
  // playlist switches (AreaTable -> ZoneMusic -> SoundEntries) and a random track plays, with the
  // authored silence interval between tracks. Doubles as a small dropdown UI (volume + song list,
  // click a song to play it manually; the playing track is highlighted).
  class ZoneMusicPlayer : public QWidget
  {
    Q_OBJECT
  public:
    explicit ZoneMusicPlayer(QWidget* parent = nullptr);

    // Enable/disable playback. When disabled, stops everything (the list still reflects the zone).
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const { return _enabled; }

    // Call each frame with the already-resolved ZoneMusic id (World::getZoneMusic) and whether it's
    // day; switches the playlist when the music changes, keeps the current track when id == 0.
    void update_zone(int zone_music_id, bool is_day);

  private:
    void ensure_player();         // lazily create the QMediaPlayer (QtMultimedia) on first use
    void rebuild_playlist();      // (re)load _files/_dir from _current_zone_music_id + _is_day
    void play_index(int index);   // play a track (fades out the current one first if one is playing)
    void start_track(int index);  // actually load + play a track and fade it in
    void play_random();           // play a random track from the playlist
    void schedule_next();         // wait the silence interval, then play_random()
    void tick_fade();             // step the volume fade in/out
    void stop_playback();

    QMediaPlayer* _player = nullptr;
    QTimer* _silence_timer;
    QTimer* _fade_timer;          // drives the volume fade in/out
    int _master_volume = 70;      // user's target volume (slider); fades go 0 <-> this
    int _fade_pending_index = -1; // track to start once the fade-out completes (-1 = fading in)
    QTemporaryFile* _temp_file = nullptr;
    std::mt19937 _rng;

    bool _enabled = false;
    int _current_zone_music_id = -1;
    bool _is_day = true;

    std::string _dir;
    std::vector<std::string> _files;
    int _now_playing = -1;
    int _silence_min_ms = 0;
    int _silence_max_ms = 0;

    // UI
    QCheckBox* _enable_check;
    QLabel* _zone_label;
    QSlider* _volume_slider;
    QListWidget* _song_list;
  };
}
