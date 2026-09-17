// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Noggit::Audio
{
  // Bytes to hand to QMediaPlayer / QSoundEffect for a sound file read from the archive.
  //
  // Modern (CASC) clients ship every sound effect as Ogg Vorbis (Classic Era listfile: 266,347 .ogg,
  // 6,501 .mp3 music tracks, 1 .wav). Qt5's Windows media backends (DirectShow / WMF) have no Vorbis
  // decoder on a stock machine and QSoundEffect never decodes anything but WAV, so every effect played
  // as silence. "OggS" data is decoded here (stb_vorbis, public domain, src/external/stb) to 16-bit PCM
  // WAV and `name` gets a ".wav" suffix so the temp file the players rename to carries the extension the
  // backend keys on. Anything else (mp3 music, wav) passes through unchanged. Implemented in
  // ui/SfxPlayer.cpp (the one translation unit that compiles stb_vorbis). docs/client_re/42 sec 11.
  std::vector<char> decodeForQtMedia(char const* data, std::size_t size, std::string& name);
}
