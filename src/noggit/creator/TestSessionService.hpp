#pragma once
#include "Services.hpp"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
class QWidget;
namespace Noggit::Runtime { class RuntimeManager; }
namespace Noggit::Creator {
class TestSessionService final : public QObject {
  Q_OBJECT
public:
  static TestSessionService* instance();
  explicit TestSessionService(Runtime::RuntimeManager* runtime);
  void testHere(QWidget* parent,Position const& position);
  void testNpc(QWidget* parent,Position const& position);
  void testQuest(QWidget* parent,Id quest);
  void testLocal(QWidget* parent);
  void cancel();
  bool busy() const {return _phase!=Idle;}
  QString status() const {return _status;}
signals:
  void changed();
private:
  enum Phase {Idle,WaitDatabase,Preparing,WaitRestart,WaitLogin};
  enum Target {None,Here,NpcTarget,QuestTarget};
  void begin(QWidget* parent,Target target,Id id,Position position={});
  void tick();
  void prepareRequest();
  void finish(QString const& message,bool error=false);
  void removeRequest();
  Runtime::RuntimeManager* _runtime;
  QPointer<QWidget> _parent;
  QTimer _timer;
  QElapsedTimer _deadline,_moduleDeadline;
  Phase _phase=Idle;
  Target _target=None;
  Position _position;
  Id _id=0;
  qint64 _expires=0;
  QString _token,_status,_loginHint;
};
}
