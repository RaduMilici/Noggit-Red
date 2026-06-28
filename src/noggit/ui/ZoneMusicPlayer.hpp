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
    void ensure_deck(int deck);   // lazily create one of the two QMediaPlayer "decks"
    void rebuild_playlist();      // (re)load _files/_dir from _current_zone_music_id + _is_day
    void play_index(int index);   // crossfade to a track
    void start_track(int index);  // load a track onto the idle deck and crossfade the decks
    void play_random();           // play a random track from the playlist
    void schedule_next();         // wait the silence interval, then play_random()
    void tick_fade();             // step the crossfade (live deck up, other deck down)
    void stop_playback();

    // Two decks so a new track can fade IN while the previous one fades OUT at the same time (a real
    // crossfade with overlap), instead of going silent between tracks. _active_deck is the live one.
    static constexpr int DECK_COUNT = 2;
    QMediaPlayer* _decks[DECK_COUNT] = {nullptr, nullptr};
    QTemporaryFile* _deck_files[DECK_COUNT] = {nullptr, nullptr};
    int _active_deck = 0;
    QTimer* _silence_timer;
    QTimer* _fade_timer;          // drives the crossfade
    int _master_volume = 70;      // user's target volume (slider); the live deck fades up to this
    bool _track_ending = false;   // guards against scheduling the next track more than once per track
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
