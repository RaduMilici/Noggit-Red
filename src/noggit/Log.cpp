// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace
{
  // Per-frame / per-item [Debug] logging (ground clutter, baked-texture probes, tile loads, ...) can
  // balloon log.txt to hundreds of MB over a long session. Suppress [Debug] output by default; set the
  // env var NOGGIT_DEBUG_LOG=1 to turn it back on for diagnosis. [Error] and info logging are unaffected.
  class NullBuffer : public std::streambuf
  {
  public:
    int overflow(int c) override { return c; } // discard everything
  };
  NullBuffer g_null_buffer;
  std::ostream g_null_stream(&g_null_buffer);

  bool debug_logging_enabled()
  {
    static bool const enabled = []
    {
      char const* const e = std::getenv("NOGGIT_DEBUG_LOG");
      return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    return enabled;
  }
}

std::ostream& _LogError(const char * pFile, int pLine)
{
  return std::cerr << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Error] ";
}
std::ostream& _LogDebug(const char * pFile, int pLine)
{
  if (!debug_logging_enabled())
    return g_null_stream;
  return std::clog << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Debug] ";
}
std::ostream& _Log(const char * pFile, int pLine)
{
  return std::cout << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): ";
}

#if DEBUG__LOGGINGTOCONSOLE
void InitLogging()
{
  LogDebug << "Logging to console window." << std::endl;
}
#else
namespace
{
  std::ofstream gLogStream;
}
void InitLogging()
{
  // Set up log.
  gLogStream.open("log.txt", std::ios_base::out | std::ios_base::trunc);
  if (gLogStream)
  {
    std::cout.rdbuf(gLogStream.rdbuf());
    std::clog.rdbuf(gLogStream.rdbuf());
    std::cerr.rdbuf(gLogStream.rdbuf());
  }
}
#endif
