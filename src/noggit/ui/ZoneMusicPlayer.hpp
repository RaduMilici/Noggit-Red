// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QtWidgets/QWidget>
#include <QtMultimedia/QMediaPlayer>
#include <QtCore/QTimer>

#include <string>
#include <vector>
#include <random>
#include <unordered_map>
#include <QtCore/QDateTime>

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
    ~ZoneMusicPlayer() override;

    // Enable/disable playback. When disabled, stops everything (the list still reflects the zone).
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const { return _enabled; }

    // Call each frame with the already-resolved ZoneMusic id (World::getZoneMusic), the zone INTRO
    // music id (World::getZoneIntroMusic, ZoneIntroMusicTable -- city/instance intros like Ironforge
    // Intro / CoT intro), and whether it's day; switches the playlist when either changes, keeps the
    // current track when both are 0.
    void update_zone(int zone_music_id, int intro_music_id, bool is_day);

    // ===== ZONE AMBIENCE channel (client law RE'd, doc 38) =====
    // A looping SoundEntries stream from the zone's SoundAmbience row (Day/Night column), with
    // the client's "Underwater (DONOTRENAME)" SoundEntries override while the LISTENER is
    // submerged. Every record change is a genuine 5.0 s CROSSFADE (FUN_00460b00: the old stream
    // fades to silence over 5 s AND the new one ramps 0 -> its authored SoundEntries Volume
    // over 5 s) -- unlike music (4 s out + hard start). Call each frame like update_zone.
    void update_ambience(int sound_ambience_id, bool is_day, bool underwater);
    // The live Ambience slider value (0..100) -- the water-loop engine shares this channel.
    int ambience_volume() const { return _amb_volume; }

    // Submerged-listener MUSIC duck (doc 38): the client's underwater SoundProviderPreferences
    // row (EAX roomHF -10000 mB) muffles everything; EAX is unreproducible on QMediaPlayer, so
    // the music decks approximate it (labeled) with a 0.5 volume duck. The ambience channel is
    // EXEMPT -- underwater it already plays the dedicated underwater record.
    void set_submerged(bool s)
    {
      if (_submerged == s) { return; }
      _submerged = s;
      if (!_fade_timer->isActive() && _decks[_active_deck])
      {
        _decks[_active_deck]->setVolume(effective_master());
      }
    }

  private:
    void ensure_deck(int deck);   // lazily create one of the two QMediaPlayer "decks"
    void rebuild_playlist();      // (re)load _files/_dir from _current_zone_music_id + _is_day
    void play_index(int index);   // start a track (new deck hard-starts full, old deck fades out)
    void start_track(int index);  // load a track onto the idle deck; fade the previous deck out
    void play_random();           // play a random LOOP track from the playlist (excludes the intro)
    void schedule_next();         // wait the silence interval, then play_random()
    void tick_fade();             // ramp the OUTGOING deck down (client-faithful: no fade-in)
    void stop_playback();
    void shutdown_audio();        // release the media backend fully (stop + clear media) for a clean exit
    bool intro_off_cooldown(int intro_id) const; // ZoneIntroMusicTable MinDelayMinutes gate

    // ambience channel internals (see update_ambience)
    void ensure_amb_deck(int deck);
    void start_ambience_entry(int se_id); // begin the 5 s crossfade to this SoundEntries id (0 = to silence)
    void tick_ambience_fade();            // wall-clock 5.0 s ramp, both decks
    int underwater_ambience_entry();      // "Underwater (DONOTRENAME)" SoundEntries id (cached name scan)

    // CLIENT-FAITHFUL transition (RE'd from wow.exe, see RE_notes/ghidra/out/MUSIC_ENGINE_RE.md):
    // the 1.12 client does NOT crossfade -- a new track HARD-STARTS at full volume (instant
    // FSOUND_SetVolume, no fade-in) while the previous track FADES OUT over 4.0 s (FSOUND fade-to-
    // silence-then-free). Two decks still overlap during that 4 s fade-out.
    static constexpr int FADE_OUT_MS      = 4000; // client zone-change fade-out (FUN_007a5a10 4.0s)
    static constexpr int FADE_TICK_MS     = 20;   // fade timer cadence (50 Hz -> smooth, not stepped)
    static constexpr int DEFAULT_SILENCE_MS = 6000; // client default inter-track gap (FUN_004601f0 +6000)

    static constexpr int DECK_COUNT = 2;
    QMediaPlayer* _decks[DECK_COUNT] = {nullptr, nullptr};
    QTemporaryFile* _deck_files[DECK_COUNT] = {nullptr, nullptr};
    int _active_deck = 0;
    QTimer* _silence_timer;
    QTimer* _fade_timer;          // drives the fade-out of the previous deck
    int _master_volume = 70;      // user's target volume (slider); the live deck plays AT this
    bool _submerged = false;      // see set_submerged()
    int effective_master() const
    {
      return _submerged ? (_master_volume + 1) / 2 : _master_volume;
    }
    bool _track_ending = false;   // guards against scheduling the next track more than once per track
    int _fade_from_volume = 0;    // outgoing deck's volume when the fade-out started
    // WALL-CLOCK fade anchor (2026-08-27, user: fade "goes lower, pause, lower, pause"): the fade
    // ran on COUNTED ticks, but zone changes stall the UI thread with tile loads, so the QTimer
    // fired in late bursts -- chunked volume drops AND a stretched fade. Elapsed real time puts
    // every (late) tick at the right point on the client's 4.0 s linear ramp.
    qint64 _fade_start_ms = 0;
    std::mt19937 _rng;

    bool _enabled = false;
    int _current_zone_music_id = -1;
    int _current_intro_music_id = -1;
    bool _is_day = true;

    // Per-file source directory (SoundEntries.DirectoryBase), parallel to _files -- the intro track
    // often lives in a DIFFERENT folder (Sound\Music\Custom) than the looping zone music, so a single
    // shared _dir no longer works.
    std::vector<std::string> _dirs;
    std::vector<std::string> _files;
    int _now_playing = -1;
    int _silence_min_ms = 0;
    int _silence_max_ms = 0;

    // Zone INTRO music (ZoneIntroMusicTable): the client plays it ONCE on zone entry (unlooped, full
    // volume, its own stream, does NOT duck the loop), gated by a per-id MinDelayMinutes cooldown. In
    // the list it's the entry at _intro_index; it is EXCLUDED from the random loop rotation and only
    // fires on entry (or on manual click). _intro_last_played tracks the cooldown by ZoneIntroMusic id.
    int _intro_index = -1;              // index in _files of the intro track (-1 = none)
    int _intro_min_delay_ms = 0;        // MinDelayMinutes * 60000 for the current intro
    std::unordered_map<int, qint64> _intro_last_played; // ZoneIntroMusic id -> last-played epoch ms

    // UI
    QCheckBox* _enable_check;
    QLabel* _zone_label;
    QSlider* _volume_slider;
    QListWidget* _song_list;

    // ===== ambience channel state =====
    QMediaPlayer* _amb_decks[2] = {nullptr, nullptr};
    QTemporaryFile* _amb_files[2] = {nullptr, nullptr};
    int _amb_active = 0;
    QTimer* _amb_fade_timer = nullptr;
    qint64 _amb_fade_start_ms = 0;   // wall-clock crossfade anchor (5000 ms, both directions)
    int _amb_fade_from = 0;          // outgoing deck volume at fade start
    int _amb_target_volume = 0;      // incoming deck target = SoundEntries Volume x slider
    float _amb_dbc_vol = 1.0f;       // the playing row's authored Volume (for live slider apply)
    int _amb_volume = 70;            // ambience slider (client AmbienceVolume analogue)
    int _current_ambience_se = 0;    // SoundEntries id currently sounding (0 = silence)
    int _underwater_ambience_se = -2; // -2 = not yet resolved by the name scan
    QSlider* _ambience_slider = nullptr;
  };
}
