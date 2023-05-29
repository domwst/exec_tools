#pragma once

#include <sex/util/file_descriptor.hpp>
#include <sex/util/syscall.hpp>
#include <sex/cgroup_controller.hpp>
#include <sex/util/event_poller.hpp>
#include <sex/util/timer_fd.hpp>
#include <sex/detail/process_knob.hpp>

#include <common/statistics.hpp>

#include <chrono>

namespace common {

struct WaitParameters {
  std::chrono::microseconds checkInterval = notSpecified;
  std::chrono::microseconds wallTimeLimit = notSpecified;
  std::chrono::microseconds cpuTimeLimit = notSpecified;
  uint64_t memoryLimitBytes = -1;

  static constexpr auto notSpecified = std::chrono::microseconds(-1);
};

RunStatistics RunAndWait(
    auto&& run,
    WaitParameters params,
    sex::CgroupController& cgroup,
    auto&&... otherChecks
) {
  using namespace std::chrono_literals;
  SEX_ASSERT(
    params.checkInterval != WaitParameters::notSpecified &&
    params.wallTimeLimit != WaitParameters::notSpecified &&
    params.cpuTimeLimit != WaitParameters::notSpecified &&
    params.memoryLimitBytes != uint64_t(-1)
  );


  sex::util::TimerFd statusCheck;
  statusCheck.set(10ms, 10ms);
  sex::util::TimerFd deadline;
  deadline.set(params.wallTimeLimit);

  auto processKnob = run();

  enum Event : uintptr_t {
    StatusCheck,
    Deadline,
    FinishedProc,
  };

  sex::util::EventPoller eventPoller;
  eventPoller.Add(statusCheck, (void*)StatusCheck, sex::util::EventPoller::IN);
  eventPoller.Add(deadline, (void*)Deadline, sex::util::EventPoller::IN);
  eventPoller.Add(processKnob.getPidFd(), (void*)FinishedProc, sex::util::EventPoller::IN);

  uint64_t maxMemoryBytes = 0;

  CompilationStatus currentStatus = InProgress;
  int checkId = 0;
  while (currentStatus == InProgress) {
    sex::util::EventPoller::Event ev{};
    eventPoller.Poll(ev);
    switch ((Event)(uintptr_t)ev.cookie) {
      case StatusCheck: {
        auto mem = cgroup.getCurrentMemory();
        if (mem > maxMemoryBytes) {
          maxMemoryBytes = mem;
        }
        if (maxMemoryBytes > params.memoryLimitBytes) {
          currentStatus = MemoryLimit;
        }

        auto cpu = cgroup.getCpuUsage().total;
        if (cpu > params.cpuTimeLimit) {
          currentStatus = CpuTimeLimit;
        }
        (otherChecks(checkId), ...);

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
  auto timeSpent = params.wallTimeLimit - deadline.cancel().first_expiration;
  cgroup.killAll();

  auto s = std::move(processKnob).wait();

  return {
    .wallTime = std::chrono::duration_cast<std::chrono::microseconds>(timeSpent),
    .cpuTime = cgroup.getCpuUsage(),
    .maxMemoryBytes = maxMemoryBytes,
    .exitStatus = s,
    .status = currentStatus,
  };
}

} // common