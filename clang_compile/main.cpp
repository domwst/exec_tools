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
  WallTimeLimit,
  CpuTimeLimit,
  Finished,
};

std::string_view ToString(CompilationStatus status) {
  switch(status) {
    case InProgress:
      return "";
    case MemoryLimit:
      return "ML";
    case CpuTimeLimit:
      return "TL";
    case WallTimeLimit:
      return "WT";
    case Finished:
      return "OK";
  }
}

std::string ToString(sex::util::ExitStatus status) {
  switch (status.getType()) {
    case sex::util::ExitStatus::Type::Exited:
      return std::string() + "exited " + std::to_string(status.getExitCode());
    case sex::util::ExitStatus::Type::Signaled:
      return std::string() + "signaled " + std::to_string(status.getSignal());
  }
}

constexpr uint64_t memoryLimit = 256 << 20; // 256Mb
constexpr auto wallTimeLimit = 10s;
constexpr auto cpuTimeLimit = 9s;


struct RunStatistics {
  std::chrono::microseconds wallTime{};
  sex::CgroupController::CpuUsage cpuTime{};
  uint64_t maxMemoryBytes{};
  sex::util::ExitStatus exitStatus;
  CompilationStatus status{};
};

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
  statusCheck.set(10ms, 10ms);
  sex::util::TimerFd deadline;
  deadline.set(wallTimeLimit);

  enum Event : uintptr_t {
    StatusCheck,
    Deadline,
    FinishedProc,
  };

  sex::util::EventPoller eventPoller;
  eventPoller.Add(statusCheck, (void*)StatusCheck, sex::util::EventPoller::IN);
  eventPoller.Add(deadline, (void*)Deadline, sex::util::EventPoller::IN);
  eventPoller.Add(compilation.getPidFd(), (void*)FinishedProc, sex::util::EventPoller::IN);

  uint64_t maxMemoryBytes = 0;

  CompilationStatus currentStatus = InProgress;
  while (currentStatus == InProgress) {
    sex::util::EventPoller::Event ev{};
    eventPoller.Poll(ev);
    switch ((Event)(uintptr_t)ev.cookie) {
      case StatusCheck: {
        auto mem = cctl.getCurrentMemory();
        if (mem > maxMemoryBytes) {
          maxMemoryBytes = mem;
        }
        if (maxMemoryBytes > memoryLimit) {
          currentStatus = MemoryLimit;
        }

        auto cpu = cctl.getCpuUsage().total;
        if (cpu > cpuTimeLimit) {
          currentStatus = CpuTimeLimit;
        }

        statusCheck.wait();
        break;
      }

      case Deadline:
        currentStatus = WallTimeLimit;
        deadline.wait();
        break;

      case FinishedProc:
        currentStatus = Finished;
        break;
    }
  }
  auto timeSpent = wallTimeLimit - deadline.cancel().first_expiration;
  cctl.killAll();

  auto s = std::move(compilation).wait();

  RunStatistics statistics{
    .wallTime = std::chrono::duration_cast<std::chrono::microseconds>(timeSpent),
    .cpuTime = cctl.getCpuUsage(),
    .maxMemoryBytes = maxMemoryBytes,
    .exitStatus = s,
    .status = currentStatus,
  };

  status.send(statistics).ensure();

  if (s == sex::util::ExitStatus::Exited(0)) {
    std::ofstream(destination) << std::ifstream(c.getPath() / "output").rdbuf();  // TODO: check for errors
    fs::permissions(destination, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);
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
      .setMemoryLimitHigh(memoryLimit)
      .setMemoryLimitMax(memoryLimit * 3 / 2)
      .setCpuWindowLimit(100ms, 100ms)
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

  auto statistics = status.receive<RunStatistics>().unwrap();

  std::cout << "time.wall: " << statistics.wallTime.count() << "\n";
  std::cout << "time.cpu.total: " << statistics.cpuTime.total.count() << "\n";
  std::cout << "time.cpu.user: " << statistics.cpuTime.user.count() << "\n";
  std::cout << "time.cpu.system: " << statistics.cpuTime.system.count() << "\n";
  std::cout << "memory.max: " << statistics.maxMemoryBytes << "\n";
  std::cout << "status: " << ToString(statistics.exitStatus) << "\n";
  std::cout << "verdict: " << ToString(statistics.status) << "\n";
}
