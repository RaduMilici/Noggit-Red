#pragma once
#include "Services.hpp"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
class QWidget;
namespace Noggit::Runtime { class RuntimeManager; }
namespace Noggit::Creator {
// Local-only conveniences for a test: written to the local character and account databases after the
// server restart and before the character logs in, so the server never overwrites them.
struct TestOptions {
  qint64 money = 0;       // copper added to the test character
  int level = 0;          // sets the test character's level (1-60); 0 leaves it
  bool developer = false; // lets the local account use developer commands such as .respawn
};
class TestSessionService final : public QObject {
  Q_OBJECT
public:
  static TestSessionService* instance();
  explicit TestSessionService(Runtime::RuntimeManager* runtime);
  void testHere(QWidget* parent,Position const& position);
  void testGameObject(QWidget* parent,Position const& position);
  void testNpc(QWidget* parent,Position const& position);
  void testQuest(QWidget* parent,Id quest);
  // Beside a saved placement of this NPC or object (asks which one when there are several).
  void testEntity(QWidget* parent,bool object,Id entry,TestOptions const& options={});
  void testLocal(QWidget* parent);
  void cancel();
  bool busy() const {return _phase!=Idle;}
  QString status() const {return _status;}
signals:
  void changed();
private:
  enum Phase {Idle,WaitDatabase,Preparing,WaitRestart,WaitLogin};
  enum Target {None,Here,NpcTarget,QuestTarget,EntityTarget};
  void begin(QWidget* parent,Target target,Id id,Position position={});
  void tick();
  void prepareRequest();
  void finish(QString const& message,bool error=false);
  void removeRequest();
  void applyOptions();
  Runtime::RuntimeManager* _runtime;
  QPointer<QWidget> _parent;
  QTimer _timer;
  QElapsedTimer _deadline,_moduleDeadline;
  Phase _phase=Idle;
  Target _target=None;
  Position _position;
  Id _id=0;
  bool _object=false;
  TestOptions _options;
  Id _character=0,_account=0;
  qint64 _expires=0;
  QString _token,_status,_loginHint;
};
}
