// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The application-wide tunnel instance and its GUI bits. Kept apart from SshTunnelManager.cpp so the
// manager itself stays widget-free and unit testable.

#include <noggit/ssh/SshTunnelManager.hpp>
#include <noggit/ssh/SshHostKeyPrompt.hpp>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

namespace Noggit
{
  namespace Ssh
  {
    bool promptTrustHostKey(QWidget* parent, TunnelConfig const& config, QVector<HostKey> const& keys)
    {
      QStringList lines;
      for (HostKey const& key : keys)
      {
        lines << QStringLiteral("%1  %2").arg(key.type, key.fingerprint);
      }

      QMessageBox box(parent ? parent : QApplication::activeWindow());
      box.setWindowTitle(QStringLiteral("Confirm SSH server identity"));
      box.setIcon(QMessageBox::Warning);
      box.setWindowFlag(Qt::WindowStaysOnTopHint);
      box.setTextFormat(Qt::RichText);
      box.setText(QStringLiteral(
        "<p>Noggit has not connected to <b>%1</b> (port %2) before.</p>"
        "<p>Before trusting it, ask your server administrator for the server's SSH fingerprint and check "
        "that it matches one of these <b>exactly</b>:</p><pre>%3</pre>"
        "<p>If it does not match, press <b>Cancel</b>: someone may be intercepting the connection.</p>")
        .arg(config.ssh_host.toHtmlEscaped()).arg(config.ssh_port).arg(lines.join('\n').toHtmlEscaped()));
      QPushButton* trust = box.addButton(QStringLiteral("Fingerprint matches - trust and connect"), QMessageBox::AcceptRole);
      QPushButton* cancel = box.addButton(QMessageBox::Cancel);
      box.setDefaultButton(cancel);
      box.exec();
      return box.clickedButton() == trust;
    }

    SshTunnelManager& SshTunnelManager::instance()
    {
      static SshTunnelManager* manager = []
      {
        // Parented to the application so it is destroyed (and ssh terminated) while Qt is still alive.
        auto* m = new SshTunnelManager(Options(), QCoreApplication::instance());
        m->setHostKeyPrompt([](TunnelConfig const& config, QVector<HostKey> const& keys)
        {
          return promptTrustHostKey(nullptr, config, keys);
        });
        if (QCoreApplication::instance())
        {
          QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, m, [m] { m->stop(); });
        }
        return m;
      }();
      return *manager;
    }
  }
}
