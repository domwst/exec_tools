#pragma once

#include <sex/cgroup_controller.hpp>
#include <sex/util/exit_status.hpp>

#include <chrono>
#include <string_view>
#include <string>
#include <ostream>

namespace common {

enum CompilationStatus {
  InProgress,
  MemoryLimit,
  WallTimeLimit,
  CpuTimeLimit,
  Finished,
};

struct RunStatistics {
  std::chrono::microseconds wallTime{};
  sex::CgroupController::CpuUsage cpuTime{};
  uint64_t maxMemoryBytes{};
  sex::util::ExitStatus exitStatus;
  CompilationStatus status{};
};

std::string_view ToString(CompilationStatus status);

std::string ToString(sex::util::ExitStatus status);

std::ostream &operator<<(std::ostream &out, RunStatistics statistics);

} // namespace common
