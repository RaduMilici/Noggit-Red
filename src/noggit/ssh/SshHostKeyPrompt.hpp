// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <noggit/ssh/SshTunnelConfig.hpp>

class QWidget;

namespace Noggit
{
  namespace Ssh
  {
    // Shows the server's host-key fingerprints and asks the user to confirm them (default: Cancel).
    // Returns true only when the user explicitly chose to trust the key.
    bool promptTrustHostKey(QWidget* parent, TunnelConfig const& config, QVector<HostKey> const& keys);
  }
}
