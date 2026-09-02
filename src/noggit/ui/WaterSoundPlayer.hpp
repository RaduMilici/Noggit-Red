// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <array>
#include <map>
#include <utility>

class QSoundEffect;

namespace Noggit::Ui
{
  // Continuous WATER LOOP engine (doc 38, client RE FUN_00462b50): the ambient loop the client
  // plays from nearby liquid -- RiverStill/Slow/Fast, Ocean, LavaPool/Flow, SlimeLoop -- selected
  // via SoundWaterType.dbc by {liquid class, flow speed}. Client law reproduced:
  //   * at most TWO class loops sound at once;
  //   * a class whose loop is playing at a DIFFERENT speed row is stopped (5.0 s fade,
  //     DAT_00803560) and restarted at the new speed (fade-in 5.0 s, DAT_0080355c);
  //   * volume = the row's authored Volume x distance attenuation x the Ambience slider;
  //   * silenced entirely while the LISTENER is submerged (the underwater loop is a separate
  //     lane, DAT_00835a44 gate).
  // Fed each frame from the game tick with the nearest liquid of each class (see update()).
  class WaterSoundPlayer : public QObject
  {
  public:
    static WaterSoundPlayer& instance();

    // One resolved nearby-liquid source for this frame.
    struct Source
    {
      int liquid_class = -1; // 0 river/water, 1 ocean, 2 magma, 3 slime (-1 = none)
      int speed = 0;         // SoundWaterType FluidSpeed: 0 still, 4 slow, 8 fast
      float distance = 0.0f; // yards to the nearest surface of this class (attenuation)
      // The liquid's DBC id. On 3.3.5a data LiquidType.dbc authors the loop sound per FLOW
      // VARIANT on the row itself (Water 1111 / Slow Water 1112 / Fast Water 1113 / Magma 3072
      // / Slow+Fast Magma 3052 ...), so the id alone yields exact flow there; Classic (1.12)
      // has no such column and falls back to SoundWaterType by {class, speed}.
      int liquid_id = 0;
    };

    // Per-frame update: `sources` = the nearest liquid of each present class (<=4), `submerged`
    // = listener under a liquid surface (kills the surface loops). Volume follows `ambience_vol`
    // (0..100, the Ambience slider). Enable=false stops everything (fade).
    void update(std::array<Source, 4> const& sources, int source_count,
                bool submerged, int ambience_vol, bool enabled);

    void stop_all();

  private:
    WaterSoundPlayer();

    // Attenuation range (client DAT_00803550 = 4.1666665 chunk-units -> yards below); full
    // volume within FADE_START, silent past AUDIBLE_RANGE.
    static constexpr float ATTEN_FULL = 30.0f;    // DERIVED: within this, no distance cut
    static constexpr float ATTEN_RANGE = 120.0f;  // DERIVED: beyond this, inaudible
    static constexpr int FADE_MS = 5000;          // client 5.0 s in AND out (0080355c / 00803560)
    // The underwater loop is NOT one of the 5 s surface crossfades -- it tracks the listener
    // crossing the waterline, which is perceptually instant. Its exact ramp is NOT RE'd; this is
    // a short labelled ramp so the transition reads as ears entering/leaving the water.
    static constexpr int UW_FADE_MS = 300;
    static constexpr int TICK_MS = 50;

    // Two channels (client cap = 2 concurrent class loops).
    struct Channel
    {
      // SEAMLESS LOOPING (2026-08-28): QSoundEffect with an infinite loop count, NOT QMediaPlayer.
      // The type-22 ambient loops are authored as gapless whole-file loops -- UndwaterLoop.wav is
      // 5.863 s of PCM 16/22050 with NO smpl chunk (no loop points) and matching levels at the
      // seam -- and the client plays them as looping FMOD samples, i.e. sample-accurate wrap.
      // Restarting a QMediaPlayer on EndOfMedia re-decodes and inserts an audible gap every
      // 5.9 s, which is what made the underwater loop "very obviously looping". QSoundEffect
      // holds the decoded PCM and loops it in the backend with no seam.
      QSoundEffect* player = nullptr;
      int liquid_class = -1;
      int speed = -1;
      int sound_entries_id = 0;
      float dbc_vol = 1.0f;
      int target_vol = 0;    // 0..100 after attenuation x slider
      int cur_vol = 0;       // ramped toward target
    };
    std::array<Channel, 2> _channels;
    // UNDERWATER LOOP lane (doc 38: SoundType 22 "UnderWaterLoop", id 4123 on both eras' data;
    // gated by the client's submerged-listener state DAT_00835a44 -- the same gate that silences
    // the surface loops). Its start/stop as the listener crosses the surface IS the "ears going
    // in / coming out of the water" transition the surface loops alone cannot produce.
    Channel _underwater;
    int underwater_sound(); // SoundEntries lookup by NAME (era-safe), cached in _uw_sound
    int _uw_sound = -2;     // -2 = not looked up yet, -1 = absent from this client's data
    std::map<int, QString> _file_cache; // SoundEntries id -> extracted temp path ("" = failed)

    int water_type_sound(int liquid_class, int speed); // SoundWaterType lookup (cached)
    void ensure_player(int ch);
    void ensure_underwater_player();
    QString extract(int sound_entries_id, float& dbc_vol_out);
    std::map<std::pair<int, int>, int> _wt_cache; // (class,speed) -> SoundEntries id (-1 miss)
  };
}
