#pragma once
#include <QProcess>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <unistd.h>
#include <signal.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Noggit::Runtime {
// Foreground runtime children must not survive a crash or forced editor exit.
// Normal closes still use RuntimeManager's ordered, bounded shutdown.
class RuntimeProcess final : public QProcess {
public:
#ifdef Q_OS_WIN
  RuntimeProcess() {
    // No inheritable handle: only Noggit owns the lifetime of this job.
    _job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool configured = _job && SetInformationJobObject(_job, JobObjectExtendedLimitInformation,
                                                       &limits, sizeof(limits));
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (configured && size) {
      _attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
      if (_attributes && InitializeProcThreadAttributeList(_attributes, 1, 0, &size)) {
        _attributesInitialized = true;
        // Assign during CreateProcess, not in a started() callback: no orphan race.
        _ready = UpdateProcThreadAttribute(_attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                           &_job, sizeof(_job), nullptr, nullptr);
      }
    }
    setCreateProcessArgumentsModifier([this](CreateProcessArguments* args) {
      if (!_ready) {
        // Fail closed even when used outside RuntimeManager. Never launch uncontained.
        args->applicationName = L"";
        return;
      }
      _startup.StartupInfo = *args->startupInfo;
      _startup.StartupInfo.cb = sizeof(_startup);
      _startup.lpAttributeList = _attributes;
      args->startupInfo = &_startup.StartupInfo;
      args->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
  }
  ~RuntimeProcess() override {
    if (_attributesInitialized) DeleteProcThreadAttributeList(_attributes);
    if (_attributes) HeapFree(GetProcessHeap(), 0, _attributes);
    if (_job) CloseHandle(_job);
  }
  bool containmentReady() const { return _ready; }
private:
  HANDLE _job = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST _attributes = nullptr;
  STARTUPINFOEXW _startup{};
  bool _attributesInitialized = false;
  bool _ready = false;
#else
  bool containmentReady() const { return true; }
#endif
#ifdef Q_OS_LINUX
  pid_t const _owner = ::getpid();
protected:
  void setupChildProcess() override {
    // Runs after fork, before exec: no Qt calls, allocations, logging or locks here.
    // SIGTERM lets MariaDB and the servers perform their own graceful cleanup.
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
      ::_exit(126);
    // The parent may have exited between fork and installing the death signal.
    if (::getppid() != _owner)
      ::_exit(125);
  }
#endif
};
}
