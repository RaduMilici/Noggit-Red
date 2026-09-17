// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/WaterSoundPlayer.hpp>

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryFile>
#include <noggit/audio/QtMediaTemp.hpp>
#include <QtCore/QUrl>
#include <QtMultimedia/QSoundEffect>

#include <algorithm>
#include <cmath>
#include <string>

namespace Noggit::Ui
{
  WaterSoundPlayer& WaterSoundPlayer::instance()
  {
    static WaterSoundPlayer s;
    return s;
  }

  WaterSoundPlayer::WaterSoundPlayer()
    : QObject(qApp)
  {
  }

  int WaterSoundPlayer::water_type_sound(int liquid_class, int speed)
  {
    auto const key = std::make_pair(liquid_class, speed);
    auto const it = _wt_cache.find(key);
    if (it != _wt_cache.end())
    {
      return it->second;
    }
    int found = -1;
    try
    {
      // SoundWaterType: match this class + flow speed; the table maps ocean's three speeds all
      // to Ocean, slime/lava's to their loops, so a class+speed lookup is exact (doc 38).
      for (auto row = gSoundWaterTypeDB.begin(); row != gSoundWaterTypeDB.end(); ++row)
      {
        if (static_cast<int>(row->getUInt(SoundWaterTypeDB::LiquidClass)) == liquid_class
            && static_cast<int>(row->getUInt(SoundWaterTypeDB::FluidSpeed)) == speed)
        {
          found = static_cast<int>(row->getUInt(SoundWaterTypeDB::Sound));
          break;
        }
      }
      // flow speed not authored for this class (e.g. only 'still' present) -> fall back to still
      if (found < 0 && speed != 0)
      {
        for (auto row = gSoundWaterTypeDB.begin(); row != gSoundWaterTypeDB.end(); ++row)
        {
          if (static_cast<int>(row->getUInt(SoundWaterTypeDB::LiquidClass)) == liquid_class)
          {
            found = static_cast<int>(row->getUInt(SoundWaterTypeDB::Sound));
            break;
          }
        }
      }
    }
    catch (...) {}
    _wt_cache[key] = found;
    return found;
  }

  int WaterSoundPlayer::underwater_sound()
  {
    if (_uw_sound != -2)
    {
      return _uw_sound;
    }
    _uw_sound = -1;
    // Resolved by NAME rather than the documented id so both eras' data work unchanged: the row
    // is SoundType 22 (ambient loop) called "UnderWaterLoop" (4123 in the 3.3.5a/Ascension and
    // 1.12 tables alike, dir Sound\Ambience, UndwaterLoop.wav).
    try
    {
      for (auto row = gSoundEntriesDB.begin(); row != gSoundEntriesDB.end(); ++row)
      {
        std::string name = row->getString(SoundEntriesDB::Name);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("underwaterloop") != std::string::npos)
        {
          _uw_sound = static_cast<int>(row->getUInt(SoundEntriesDB::ID));
          break;
        }
      }
    }
    catch (...) {}
    LogError << "WaterLoop: underwater loop SoundEntries id = " << _uw_sound << std::endl;
    return _uw_sound;
  }

  QString WaterSoundPlayer::extract(int sound_entries_id, float& dbc_vol_out)
  {
    dbc_vol_out = 1.0f;
    auto const cached = _file_cache.find(sound_entries_id);
    if (cached != _file_cache.end())
    {
      // volume still needs resolving for a cached path
      try
      {
        float v = gSoundEntriesDB.getByID(sound_entries_id).getFloat(SoundEntriesDB::Volume);
        dbc_vol_out = (v > 0.0f && v <= 4.0f) ? v : 1.0f;
      }
      catch (...) {}
      return cached->second;
    }
    QString out;
    try
    {
      auto const se = gSoundEntriesDB.getByID(sound_entries_id);
      float v = se.getFloat(SoundEntriesDB::Volume);
      dbc_vol_out = (v > 0.0f && v <= 4.0f) ? v : 1.0f;
      std::string fn;
      for (int i = 0; i < 10; ++i)
      {
        std::string const f = se.getString(SoundEntriesDB::Filenames + i);
        if (!f.empty()) { fn = f; break; } // loops author one file
      }
      if (!fn.empty())
      {
        std::string const full = std::string(se.getString(SoundEntriesDB::FilePath)) + "\\" + fn;
        auto* cd = Noggit::Application::NoggitApplication::instance()->clientData();
        if (cd && cd->exists(full))
        {
          BlizzardArchive::ClientFile file(full, cd);
          auto* temp = new QTemporaryFile(this);
          if (temp->open())
          {
            std::string media_name = fn;
            auto const bytes = Noggit::Audio::decodeForQtMedia(file.getBuffer(), file.getSize(), media_name);
            temp->write(bytes.data(), static_cast<qint64>(bytes.size()));
            temp->close();
            temp->rename(temp->fileName() + QString::fromStdString(media_name));
            out = temp->fileName();
          }
          else { delete temp; }
        }
        else
        {
          LogError << "WaterLoop: file not found '" << full << "'" << std::endl;
        }
      }
    }
    catch (...) {}
    _file_cache[sound_entries_id] = out;
    return out;
  }

  void WaterSoundPlayer::ensure_player(int ch)
  {
    if (!_channels[ch].player)
    {
      // SoundType-22 rows are gapless whole-file loops -> loop them in the audio backend
      // (QSoundEffect::Infinite) instead of restarting on EndOfMedia, which re-decoded and left
      // an audible seam every pass (see the Channel note in the header).
      auto* p = new QSoundEffect(this);
      p->setLoopCount(QSoundEffect::Infinite);
      p->setVolume(0.0);
      _channels[ch].player = p;
    }
  }

  void WaterSoundPlayer::ensure_underwater_player()
  {
    if (_underwater.player)
    {
      return;
    }
    auto* p = new QSoundEffect(this);
    p->setLoopCount(QSoundEffect::Infinite); // seamless: no EndOfMedia restart seam
    p->setVolume(0.0);
    _underwater.player = p;
  }

  void WaterSoundPlayer::stop_all()
  {
    if (_underwater.player)
    {
      _underwater.player->stop();
      _underwater.player->setVolume(0.0);
      _underwater.sound_entries_id = 0;
      _underwater.liquid_class = -1;
      _underwater.cur_vol = 0;
      _underwater.target_vol = 0;
    }
    for (auto& c : _channels)
    {
      if (c.player)
      {
        c.player->stop();
        c.player->setVolume(0.0);
      }
      c.liquid_class = -1;
      c.speed = -1;
      c.sound_entries_id = 0;
      c.cur_vol = 0;
      c.target_vol = 0;
    }
  }

  void WaterSoundPlayer::update(std::array<Source, 4> const& sources, int source_count,
                                bool submerged, int ambience_vol, bool enabled)
  {
    // UNDERWATER LOOP (the "ears in / out of the water" lane, doc 38). Runs BEFORE the surface
    // gate below because it is exactly the sound that must play while that gate is silencing
    // everything else. Ramps over UW_FADE_MS so crossing the waterline is audible as a
    // transition rather than a hard cut.
    {
      int const uw_target = (enabled && submerged && ambience_vol > 0) ? ambience_vol : 0;
      int const uw_step = std::max(1, (100 * TICK_MS) / UW_FADE_MS);
      Channel& c = _underwater;
      if (uw_target > 0 && c.sound_entries_id == 0)
      {
        int const se = underwater_sound();
        if (se > 0)
        {
          float dbc = 1.0f;
          QString const path = extract(se, dbc);
          if (!path.isEmpty())
          {
            ensure_underwater_player();
            c.sound_entries_id = se;
            c.dbc_vol = dbc;
            c.liquid_class = 0; // marks the loop-restart connection as armed (see ensure_player)
            c.cur_vol = 0;
            c.player->setVolume(0.0);
            c.player->setSource(QUrl::fromLocalFile(path));
            c.player->play();
          }
        }
      }
      if (c.player)
      {
        c.target_vol = std::clamp(static_cast<int>(std::lround(c.dbc_vol * uw_target)), 0, 100);
        if (c.cur_vol < c.target_vol) { c.cur_vol = std::min(c.target_vol, c.cur_vol + uw_step); }
        else if (c.cur_vol > c.target_vol) { c.cur_vol = std::max(c.target_vol, c.cur_vol - uw_step); }
        c.player->setVolume(c.cur_vol / 100.0);
        if (c.target_vol == 0 && c.cur_vol == 0 && c.sound_entries_id != 0)
        {
          c.player->stop();
          c.sound_entries_id = 0;
          c.liquid_class = -1;
        }
      }
    }

    // GATE (client FUN_00462b50): sound enabled AND listener NOT submerged. Underwater kills the
    // surface loops -- the submerged sound is the separate underwater ambience lane (above).
    if (!enabled || submerged || ambience_vol <= 0)
    {
      for (auto& c : _channels)
      {
        if (c.player && c.cur_vol > 0)
        {
          c.cur_vol = std::max(0, c.cur_vol - std::max(1, (100 * TICK_MS) / FADE_MS));
          c.player->setVolume(c.cur_vol / 100.0);
          if (c.cur_vol == 0) { c.player->stop(); c.liquid_class = -1; c.speed = -1; }
        }
      }
      return;
    }

    // Rank present sources by distance; the client sounds at most TWO class loops at once.
    std::array<Source, 4> ranked = sources;
    int const n = std::min(source_count, 4);
    std::sort(ranked.begin(), ranked.begin() + n,
              [](Source const& a, Source const& b) { return a.distance < b.distance; });
    int const want = std::min(n, 2);

    bool assigned[2] = {false, false};
    // keep a channel already playing the wanted class; else (re)assign
    for (int s = 0; s < want; ++s)
    {
      Source const& src = ranked[s];
      // 3.3.5a: the row's own loop column is exact (and encodes the flow variant). Classic:
      // returns 0 -> SoundWaterType by {class, speed}. (doc 38, version-gated in DBC.cpp)
      int se = LiquidTypeDB::loopSound(src.liquid_id);
      if (se <= 0)
      {
        se = water_type_sound(src.liquid_class, src.speed);
      }
      if (se <= 0) { continue; }

      // DISTANCE ATTENUATION -- the row's OWN authored 3D distances (user 2026-08-27: "too much
      // water sound in Timbermaw"). The client hands the loop to FMOD as a 3D sound and lets it
      // roll off with SoundEntries.minDistance / distanceCutoff; my earlier DERIVED 30/120 yd
      // pair was ~4x too generous (RiverStill authors minDistance 8, cutoff 45 -- Ocean 10/60,
      // SlimeLoop 6/30, LavaPool 30/60), so a pool across a cave played at almost full volume.
      // FMOD's default 3D model is inverse rolloff: vol = minDistance / distance, silent past
      // the cutoff.
      float min_dist = 8.0f;
      float cutoff = 45.0f;
      try
      {
        auto const row = gSoundEntriesDB.getByID(se);
        float const md = row.getFloat(SoundEntriesDB::minDistance);
        float const co = row.getFloat(SoundEntriesDB::distanceCutoff);
        if (md > 0.1f) { min_dist = md; }
        if (co > min_dist) { cutoff = co; }
      }
      catch (...) {}

      float atten = 0.0f;
      if (src.distance <= cutoff)
      {
        atten = std::clamp(min_dist / std::max(src.distance, min_dist), 0.0f, 1.0f);
      }
      if (atten <= 0.001f) { continue; } // out of range entirely -> do not claim a channel

      // find a channel already on this class+speed, else a free one
      int ch = -1;
      for (int i = 0; i < 2; ++i)
      {
        if (!assigned[i] && _channels[i].liquid_class == src.liquid_class
            && _channels[i].speed == src.speed)
        {
          ch = i; break;
        }
      }
      if (ch < 0)
      {
        for (int i = 0; i < 2; ++i)
        {
          if (!assigned[i] && _channels[i].liquid_class < 0) { ch = i; break; }
        }
      }
      if (ch < 0)
      {
        for (int i = 0; i < 2; ++i) { if (!assigned[i]) { ch = i; break; } }
      }
      if (ch < 0) { continue; }
      assigned[ch] = true;

      ensure_player(ch);
      Channel& c = _channels[ch];
      // (re)start when the class or speed row changed (client stops the old speed, 5 s fades)
      if (c.liquid_class != src.liquid_class || c.speed != src.speed || c.sound_entries_id != se)
      {
        float dbc = 1.0f;
        QString const path = extract(se, dbc);
        if (path.isEmpty())
        {
          c.liquid_class = -1; c.speed = -1; assigned[ch] = false; continue;
        }
        c.liquid_class = src.liquid_class;
        c.speed = src.speed;
        c.sound_entries_id = se;
        c.dbc_vol = dbc;
        c.cur_vol = 0; // fade in from 0
        c.player->setVolume(0.0);
        c.player->setSource(QUrl::fromLocalFile(path));
        c.player->play();
      }
      c.target_vol = std::clamp(
        static_cast<int>(std::lround(c.dbc_vol * atten * ambience_vol)), 0, 100);
    }

    // ramp every channel toward its target (5 s in/out); stop unassigned ones
    int const step = std::max(1, (100 * TICK_MS) / FADE_MS);
    for (int i = 0; i < 2; ++i)
    {
      Channel& c = _channels[i];
      if (!c.player) { continue; }
      if (!assigned[i])
      {
        c.target_vol = 0;
      }
      if (c.cur_vol < c.target_vol) { c.cur_vol = std::min(c.target_vol, c.cur_vol + step); }
      else if (c.cur_vol > c.target_vol) { c.cur_vol = std::max(c.target_vol, c.cur_vol - step); }
      c.player->setVolume(c.cur_vol / 100.0);
      if (!assigned[i] && c.cur_vol == 0 && c.liquid_class >= 0)
      {
        c.player->stop();
        c.liquid_class = -1;
        c.speed = -1;
      }
    }
  }
}
