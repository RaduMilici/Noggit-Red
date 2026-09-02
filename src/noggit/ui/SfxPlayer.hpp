// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <map>
#include <random>
#include <utility>

class QMediaPlayer;

namespace Noggit::Ui
{
  // One-shot SFX playback (doc 38 audio port): footsteps, fidgets, $CSD event sounds.
  // A small round-robin pool of QMediaPlayer so rapid steps overlap instead of cutting each
  // other off, with the extracted MPQ file CACHED per (SoundEntries id, file index) -- a
  // footstep must not re-extract its wav every fire. File choice per play follows the client:
  // weighted random over the row's up-to-10 files by the Freq columns (uniform when
  // unauthored). Volume = the row's authored Volume x the effects slider.
  class SfxPlayer : public QObject
  {
  public:
    static SfxPlayer& instance();

    // Play one shot of this SoundEntries row (0/invalid = no-op).
    void play(int sound_entries_id);
    // Effects volume 0..100 (persisted by the caller; default 70).
    void set_volume(int v) { _volume = v; }
    int volume() const { return _volume; }
    // Submerged-listener duck (doc 38): the client switches the sound provider to the zone's
    // UNDERWATER SoundProviderPreferences row (11 for 765 zones -- an EAX preset whose
    // roomHF -10000 mB kills the highs = the muffle). EAX is not reproducible on QMediaPlayer;
    // the audible core is APPROXIMATED (labeled) as a 0.5 volume duck on one-shots.
    void set_submerged(bool s) { _submerged = s; }

  private:
    bool _submerged = false;
  public:

  private:
    SfxPlayer();

    static constexpr int POOL = 4;
    QMediaPlayer* _pool[POOL] = {nullptr, nullptr, nullptr, nullptr};
    int _next = 0;
    int _volume = 70;
    std::mt19937 _rng;
    // (SoundEntries id, file index) -> extracted temp file path ("" = extraction failed, don't retry)
    std::map<std::pair<int, int>, QString> _file_cache;
  };
}
