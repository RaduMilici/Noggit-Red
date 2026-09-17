// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/SfxPlayer.hpp>

#include <noggit/DBC.h>
#include <noggit/Log.h>

#include <set>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryFile>
#include <noggit/audio/QtMediaTemp.hpp>

// stb_vorbis, compiled once here (see QtMediaTemp.hpp)
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../external/stb/stb_vorbis.c" // src/noggit/ui -> src/external/stb
#include <cstdlib>
#include <cstring>

namespace Noggit::Audio
{
  std::vector<char> decodeForQtMedia(char const* data, std::size_t size, std::string& name)
  {
    if (!data || size < 4 || std::memcmp(data, "OggS", 4) != 0)
    {
      return std::vector<char>(data, data + size);
    }
    int channels = 0;
    int sample_rate = 0;
    short* pcm = nullptr;
    int const frames = stb_vorbis_decode_memory(reinterpret_cast<unsigned char const*>(data), static_cast<int>(size),
                                                &channels, &sample_rate, &pcm);
    if (frames <= 0 || !pcm || channels <= 0 || sample_rate <= 0)
    {
      if (pcm) std::free(pcm);
      LogError << "Audio: Ogg Vorbis decode failed for '" << name << "' (" << size << " bytes)" << std::endl;
      return std::vector<char>(data, data + size);
    }
    std::uint32_t const data_bytes = static_cast<std::uint32_t>(frames) * static_cast<std::uint32_t>(channels) * 2u;
    std::vector<char> wav(44 + data_bytes);
    auto const put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(wav.data() + at, &v, 4); };
    auto const put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(wav.data() + at, &v, 2); };
    std::memcpy(wav.data(), "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, static_cast<std::uint16_t>(channels));
    put32(24, static_cast<std::uint32_t>(sample_rate));
    put32(28, static_cast<std::uint32_t>(sample_rate) * static_cast<std::uint32_t>(channels) * 2u);
    put16(32, static_cast<std::uint16_t>(channels * 2));
    put16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, data_bytes);
    std::memcpy(wav.data() + 44, pcm, data_bytes);
    std::free(pcm);
    name += ".wav";
    return wav;
  }
}
#include <QtCore/QUrl>
#include <QtMultimedia/QMediaPlayer>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Noggit::Ui
{
  SfxPlayer& SfxPlayer::instance()
  {
    static SfxPlayer s_instance;
    return s_instance;
  }

  SfxPlayer::SfxPlayer()
    : QObject(qApp)
    , _rng(std::random_device{}())
  {
    _volume = std::clamp(QSettings().value("zone_music/effects_volume", 70).toInt(), 0, 100);
  }

  void SfxPlayer::play(int sound_entries_id)
  {
    // [SFX-TRACE 2026-08-28 TEMP] "footstep sound not working in ascension". Verified offline that
    // the whole chain is healthy there: footstep id 7 -> FootstepTerrainLookup -> SoundEntries 560,
    // whose .wav exists in common.MPQ and is byte-identical (PCM 16/22050) to the Turtle file that
    // DOES play; the live FOOTSTEP-DIAG shows the resolve firing, and no "SFX: file not found" is
    // logged. Every remaining failure mode in here is SILENT, so log one line per distinct sound id
    // covering the two silent early-outs (volume gate, unknown row) and the actual playback state.
    static std::set<int> s_traced;
    bool const trace = s_traced.insert(sound_entries_id).second;
    if (trace)
    {
      LogError << "[SFX-TRACE] id=" << sound_entries_id << " effectsVolume=" << _volume
               << " submerged=" << (_submerged ? 1 : 0)
               << " rowExists=" << (sound_entries_id > 0 && gSoundEntriesDB.CheckIfIdExists(sound_entries_id) ? 1 : 0)
               << std::endl;
    }
    if (sound_entries_id <= 0 || _volume <= 0)
    {
      if (trace)
      {
        LogError << "[SFX-TRACE] id=" << sound_entries_id << " ABORT: volume gate (_volume="
                 << _volume << ")" << std::endl;
      }
      return;
    }
    try
    {
      if (!gSoundEntriesDB.CheckIfIdExists(sound_entries_id))
      {
        return;
      }
      auto const se = gSoundEntriesDB.getByID(sound_entries_id);

      // client file pick: weighted random over the authored files by the Freq columns
      std::vector<int> file_indices;
      std::vector<int> weights;
      int weight_sum = 0;
      for (int i = 0; i < 10; ++i)
      {
        std::string const fn = se.getString(SoundEntriesDB::Filenames + i);
        if (fn.empty())
        {
          continue;
        }
        int const w = static_cast<int>(se.getUInt(SoundEntriesDB::Freq + i));
        file_indices.push_back(i);
        weights.push_back(w);
        weight_sum += w;
      }
      if (file_indices.empty())
      {
        return;
      }
      int pick = 0;
      if (file_indices.size() > 1)
      {
        if (weight_sum > 0)
        {
          std::uniform_int_distribution<int> dist(0, weight_sum - 1);
          int roll = dist(_rng);
          for (std::size_t i = 0; i < weights.size(); ++i)
          {
            roll -= weights[i];
            if (roll < 0)
            {
              pick = static_cast<int>(i);
              break;
            }
          }
        }
        else
        {
          std::uniform_int_distribution<int> dist(0, static_cast<int>(file_indices.size()) - 1);
          pick = dist(_rng);
        }
      }
      int const file_idx = file_indices[static_cast<std::size_t>(pick)];

      // extraction cache: one temp file per (row, file); "" marks a known-missing file
      auto const key = std::make_pair(sound_entries_id, file_idx);
      auto cached = _file_cache.find(key);
      if (cached == _file_cache.end())
      {
        QString path_out;
        std::string const fn = se.getString(SoundEntriesDB::Filenames + file_idx);
        std::string const full = std::string(se.getString(SoundEntriesDB::FilePath)) + "\\" + fn;
        auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
        if (client_data && client_data->exists(full))
        {
          try
          {
            BlizzardArchive::ClientFile file(full, client_data);
            auto* temp = new QTemporaryFile(this);
            if (temp->open())
            {
              std::string media_name = fn;
              auto const bytes = Noggit::Audio::decodeForQtMedia(file.getBuffer(), file.getSize(), media_name);
              temp->write(bytes.data(), static_cast<qint64>(bytes.size()));
              temp->close();
              temp->rename(temp->fileName() + QString::fromStdString(media_name));
              path_out = temp->fileName();
            }
            else
            {
              delete temp;
            }
          }
          catch (...) {}
        }
        else
        {
          LogError << "SFX: file not found '" << full << "'" << std::endl;
        }
        cached = _file_cache.emplace(key, path_out).first;
      }
      if (cached->second.isEmpty())
      {
        return;
      }

      if (!_pool[_next])
      {
        _pool[_next] = new QMediaPlayer(this);
      }
      QMediaPlayer* const p = _pool[_next];
      _next = (_next + 1) % POOL;

      float dbc_vol = se.getFloat(SoundEntriesDB::Volume);
      if (!(dbc_vol > 0.0f) || dbc_vol > 4.0f)
      {
        dbc_vol = 1.0f;
      }
      p->stop();
      float const duck = _submerged ? 0.5f : 1.0f; // labeled approximation of the EAX muffle (hpp note)
      p->setVolume(std::clamp(static_cast<int>(std::lround(dbc_vol * _volume * duck)), 0, 100));
      p->setMedia(QUrl::fromLocalFile(cached->second));
      p->play();
      if (trace)
      {
        LogError << "[SFX-TRACE] id=" << sound_entries_id << " PLAY vol="
                 << p->volume() << " dbcVol=" << dbc_vol << " file='"
                 << cached->second.toStdString() << "' mediaStatus="
                 << static_cast<int>(p->mediaStatus()) << " error="
                 << static_cast<int>(p->error()) << " errStr='"
                 << p->errorString().toStdString() << "'" << std::endl;
      }
    }
    catch (...) {}
  }
}
