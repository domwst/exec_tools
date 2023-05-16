#include <sex/execute.hpp>
#include <sex/container.hpp>
#include <sex/cgroup_controller.hpp>
#include <sex/util/mount/tmpfs.hpp>
#include <sex/util/mount/bind.hpp>
#include <sex/util/timer_fd.hpp>
#include <sex/util/event_poller.hpp>
#include <sex/util/data_transfer.hpp>

#include <chrono>
#include <iostream>
#include <csignal>
#include "sex/util/pipe.hpp"

using namespace std::chrono_literals;

enum CompilationStatus {
  InProgress,
  MemoryLimit,
  TimeLimit,
  Finished,
};

constexpr uint64_t memoryLimit = 3ull << 30;

void Proxy(
  sex::Container& c,
  const fs::path& source,
  const fs::path& destination,
  sex::CgroupController& cctl,
  sex::util::DataTransfer& status,
  sex::util::FileDescriptor logsFd,
  sex::util::DataTransfer& pid,
  sex::util::DataTransfer& start
) {
  sex::util::TmpFsMount tmpMnt(c.getPath(), 32 << 20); // 32Mb
  sex::util::FileBindMount sourceMnt(source, c.getPath() / "source.cpp");

  tmpMnt.mount().ensure();
  std::move(tmpMnt).detach();  // Bind mounts would be alive
  sourceMnt.mount().ensure();

  std::vector<fs::path> pathsToMount = {
          "/usr",
          "/lib",
          "/lib64",
  };
  for (const auto& p : pathsToMount) {
    sex::util::DirectoryBindMount mnt(p, c.getPath() / p.relative_path());
    mnt.mount().ensure();
    std::move(mnt).detach();
  }

  fs::create_directory(c.getPath() / "tmp");

  auto compilation = sex::Execute([&c, &logsFd, &start] {
    c.enter();
    SEX_SYSCALL(dup2(logsFd.getInt(), STDERR_FILENO)).ensure();
    start.receive<char>().ensure();

    SEX_SYSCALL(execlp("clang++-15", "clang++-15", "source.cpp", "-o", "output", "-static", "-std=c++2a", NULL))
    .ensure();
  }, sex::util::ExecuteArgs{}.NewPidNS().CreatePidFd());

  pid.send(compilation.getPid()).ensure();

  sex::util::TimerFd statusCheck;
  statusCheck.set(20ms, 20ms);
  sex::util::TimerFd deadline;
  deadline.set(10s);

  enum Event : uintptr_t {
    StatusCheck,
    Deadline,
    FinishedProc,
  };

  sex::util::EventPoller eventPoller;
  eventPoller.Add(statusCheck, (void*)StatusCheck, sex::util::EventPoller::IN);
  eventPoller.Add(deadline, (void*)Deadline, sex::util::EventPoller::IN);
  eventPoller.Add(compilation.getPidFd(), (void*)FinishedProc, sex::util::EventPoller::IN);

  CompilationStatus currentStatus = InProgress;
  while (currentStatus == InProgress) {
    sex::util::EventPoller::Event ev{};
    eventPoller.Poll(ev);
    switch ((Event)(uintptr_t)ev.cookie) {
      case StatusCheck: {
        auto mem = cctl.getCurrentMemory();
        if (mem > memoryLimit) {
          currentStatus = MemoryLimit;
        }
        statusCheck.wait();
        std::cout << "Status check, mem=" << mem << std::endl;
        break;
      }

      case Deadline:
        currentStatus = TimeLimit;
        deadline.wait();
        std::cerr << "Deadline" << std::endl;
        break;

      case FinishedProc:
        currentStatus = Finished;
        std::cerr << "Process is finished" << std::endl;
        break;
    }
  }
  compilation.sendSignal(SIGKILL).ensure();

  auto s = std::move(compilation).wait();

  std::cout << "Waited for process" << std::endl;

  status.send(currentStatus).ensure();
  if (currentStatus != Finished) return;
  status.send(s).ensure();

  if (s == sex::util::ExitStatus::Exited(0)) {
    std::ofstream(destination) << std::ifstream(c.getPath() / "output").rdbuf();  // TODO: check for errors
  }
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " source destination logs" << std::endl;
    return 1;
  }

  auto logsFd = sex::util::FdHolder(SEX_SYSCALL(open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0666)).unwrap());

  const std::string_view source = argv[1], destination = argv[2];
  sex::Container c("clang_compile_" + std::to_string(getpid()));
  sex::CgroupController cctl(
    "clang_compile_" + std::to_string(getpid()),
    sex::CgroupController::Builder{}
      .setPidsLimit(10)
      .setMemoryLimitHigh(memoryLimit)  // 256Mb
      .setMemoryLimitMax(memoryLimit * 3 / 2)
  );

  sex::util::DataTransfer start;
  sex::util::DataTransfer pidAndKill;
  sex::util::DataTransfer status;

  auto proxy = sex::Execute([&] {
    Proxy(c, source, destination, cctl, status, logsFd, pidAndKill, start);
  }, sex::util::ExecuteArgs{}.NewUserNS().NewMountNS().NewNetworkNS().NewUTSNS());

  int pid = pidAndKill.receive<int>().unwrap();
  cctl.addProcess(pid);

  start.send('s').ensure();

  auto proxyStatus = std::move(proxy).wait();
  if (proxyStatus != sex::util::ExitStatus::Exited(0)) {
    std::cerr << "Proxy process finished with unexpected status: ";
    if (proxyStatus.isExited()) {
      std::cerr << "exit(" << proxyStatus.getExitCode() << ")";
    } else {
      std::cerr << "signal(" << proxyStatus.getSignal() << ")";
    }
    std::cerr << std::endl;
    return 1;
  }

  auto execStatus = status.receive<CompilationStatus>().unwrap();

  switch (execStatus) {
    case CompilationStatus::TimeLimit:
      std::cerr << "Time limit exceeded" << std::endl;
      break;

    case CompilationStatus::MemoryLimit:
      std::cerr << "Memory limit exceeded" << std::endl;
      break;

    case CompilationStatus::Finished: {
      auto s = status.receive<sex::util::ExitStatus>().unwrap();
      if (s.isExited()) {
        std::cerr << "Clang finished with exit code " << s.getExitCode() << std::endl;
      } else {
        std::cerr << "Clang was killed by signal #" << s.getSignal() << std::endl;
      }
      break;
    }

    case CompilationStatus::InProgress:
      SEX_ASSERT(false);
  }
}
