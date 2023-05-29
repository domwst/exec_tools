#include "statistics.hpp"

namespace common {

std::string_view ToString(CompilationStatus status) {
  switch (status) {
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

std::ostream &operator<<(std::ostream &out, RunStatistics statistics) {
  out << "time.wall: " << statistics.wallTime.count() << "\n";
  out << "time.cpu.total: " << statistics.cpuTime.total.count() << "\n";
  out << "time.cpu.user: " << statistics.cpuTime.user.count() << "\n";
  out << "time.cpu.system: " << statistics.cpuTime.system.count() << "\n";
  out << "memory.max: " << statistics.maxMemoryBytes << "\n";
  out << "status: " << ToString(statistics.exitStatus) << "\n";
  out << "verdict: " << ToString(statistics.status) << "\n";
  return out;
}

} // namespace common
