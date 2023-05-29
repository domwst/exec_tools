#include <sex/execute.hpp>
#include <sex/cgroup_controller.hpp>
#include <sex/container.hpp>
#include <sex/util/rlimit.hpp>
#include <sex/util/mount/tmpfs.hpp>
#include <sex/util/mount/bind.hpp>

#include <common/wait_process.hpp>

#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <sys/resource.h>
#include <fcntl.h>

using namespace std;

namespace fs = filesystem;

constexpr uint64_t memoryLimitBytes = 256 << 20;
constexpr auto cpuTimeLimit = 4s;
constexpr auto wallTimeLimit = 5s;
constexpr auto sboxSize = 8ull << 20;
constexpr auto maxFileSize = 64ull << 10;

int main(int argc, char* argv[]) {
  if (argc != 5) {
    std::cerr << "Usage: " << argv[0] << " executable input_file output_file errors_file\n";
    return 1;
  }
  const std::string_view executable = argv[1];
  const std::string_view input = argv[2];
  const std::string_view output = argv[3];
  const std::string_view errors = argv[4];
  const std::string uniqueName = "run_" + std::to_string(getpid());

  fs::permissions(executable, fs::perms::others_exec, fs::perm_options::add);
  sex::CgroupController cctl(
    uniqueName,
    sex::CgroupController::Builder{}
      .setPidsLimit(1)
      .setCpuWindowLimit(100ms, 100ms)
      .setMemoryLimitMax(3 * memoryLimitBytes / 2)
      .setMemoryLimitHigh(memoryLimitBytes)
  );
  sex::Container container(uniqueName);

  auto inFd = SEX_SYSCALL(open(input.data(), O_RDONLY | O_CLOEXEC)).unwrap();
  auto outFd = SEX_SYSCALL(open(output.data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644)).unwrap();
  auto errFd = SEX_SYSCALL(open(errors.data(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644)).unwrap();

  auto cfd = cctl.getCgroupFd();

  auto run = [&] {
    return sex::Execute(
      [&inFd, &outFd, &errFd, &executable, &container] {
        constexpr std::string_view inner_exec_name = "main";
        const auto name_inside_container = "/" + std::string(inner_exec_name);

        sex::util::TmpFsMount tmp(container.getPath(), sboxSize);
        tmp.mount().ensure();
        sex::util::FileBindMount exec(executable, container.getPath() / inner_exec_name);
        exec.mount().ensure();
        container.enter();

        SEX_SYSCALL(dup2(inFd, STDIN_FILENO)).ensure();
        SEX_SYSCALL(dup2(outFd, STDOUT_FILENO)).ensure();
        SEX_SYSCALL(dup2(errFd, STDERR_FILENO)).ensure();
        sex::util::SetRLimit(sex::util::ResourceType::FileSize, maxFileSize).ensure();

        SEX_SYSCALL(execlp(name_inside_container.c_str(), name_inside_container.c_str(), NULL)).ensure();
      },
      sex::util::ExecuteArgs{}
        .NewNetworkNS()
        .NewMountNS()
        .NewPidNS()
//        .NewTimeNS()
        .NewUserNS()
        .IntoCgroup(cfd)
        .CreatePidFd()
    );
  };

  auto stats = common::RunAndWait(
    std::move(run),
    {
        .checkInterval = 10ms,
        .wallTimeLimit = wallTimeLimit,
        .cpuTimeLimit = cpuTimeLimit,
        .memoryLimitBytes = memoryLimitBytes,
      },
    cctl
  );

  std::cout << stats;
}