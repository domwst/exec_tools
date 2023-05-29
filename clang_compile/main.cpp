#include <sex/execute.hpp>
#include <sex/container.hpp>
#include <sex/cgroup_controller.hpp>
#include <sex/util/mount/tmpfs.hpp>
#include <sex/util/mount/bind.hpp>
#include <sex/util/data_transfer.hpp>
#include <sex/util/syscall.hpp>

#include <common/statistics.hpp>
#include <common/wait_process.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;
using namespace common;

constexpr uint64_t memoryLimit = 256 << 20; // 256Mb
constexpr auto wallTimeLimit = 10s;
constexpr auto cpuTimeLimit = 9s;

void Proxy(
  sex::Container& c,
  const fs::path& source,
  const fs::path& destination,
  sex::CgroupController& cctl,
  sex::util::DataTransfer& statistics,
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

  auto run_compilation = [&] {
    auto knob = sex::Execute([&c, &logsFd, &start] {
      c.enter();
      SEX_SYSCALL(dup2(logsFd.getInt(), STDERR_FILENO)).ensure();
      start.receive<char>().ensure();

      SEX_SYSCALL(execlp("clang++-15", "clang++-15", "source.cpp", "-o", "output", "-static", "-std=c++2a", NULL))
              .ensure();
    }, sex::util::ExecuteArgs{}.NewPidNS().CreatePidFd());
    pid.send(knob.getPid()).ensure();
    return knob;
  };

  auto stats = common::RunAndWait(
    std::move(run_compilation),
    {
      .checkInterval = 10ms,
      .wallTimeLimit = wallTimeLimit,
      .cpuTimeLimit = cpuTimeLimit,
      .memoryLimitBytes = memoryLimit,
    },
    cctl
  );

  statistics.send(stats).ensure();

  if (stats.exitStatus == sex::util::ExitStatus::Exited(0)) {
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

  auto logsFd = sex::util::FdHolder(SEX_SYSCALL(open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666)).unwrap());

  const std::string_view source = argv[1], destination = argv[2];
  const std::string uniqueName = "clang_compile_" + std::to_string(getpid());
  sex::Container c(uniqueName);
  sex::CgroupController cctl(
    uniqueName,
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

  std::cout << statistics;
}
