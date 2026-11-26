# Orbit development

## Prerequisites

Use Linux with CMake 3.28 or newer, Ninja, Git and Python 3.10 or newer. The `debug` and
`release` presets select GCC 15. The `sanitizers` preset selects Clang 22 and its
compiler-rt libraries, with the GCC toolchain providing libstdc++. On Ubuntu 26.04, install
the tools and run the commands below from the root of a checkout.

```sh
sudo apt-get update
sudo apt-get install -y cmake g++-15 git ninja-build python3
sudo apt-get install -y clang-22 libclang-rt-22-dev  # for sanitizers
```

CMake FetchContent downloads the pinned spdlog, CLI11, Abseil and GoogleTest sources from
GitHub on the first configure in each build directory. If outbound access is restricted,
provide local checkouts of the same tags with CMake's `FETCHCONTENT_SOURCE_DIR_<NAME>`
cache variables. Use `CMakeUserPresets.json` for local overrides.

## Build and test

Each preset has its own build directory under `build/` and enables tests. Debug includes
symbols and assertions. Release uses the toolchain's standard optimizations and defines
`NDEBUG`. Sanitizers uses Debug settings with ASan and UBSan.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Use the label filters below with any of the three test presets. Use `-R <regex>` to
select by CTest name, `-N` to list registrations, or `-V` to see the commands and their
output. GoogleTest discovery runs when CTest loads the registrations, so compile first.
A missing or empty test selection is an error with the test presets.

```sh
ctest --preset debug -L '^unit$'
ctest --preset debug -L '^integration$'
ctest --preset debug -R '^SendBufferTest\.'
ctest --preset debug -N
```

For an individual Python scenario, supply the executable from the same build.

```sh
python3 -u tests/integration/proxy_sessions_test.py ./build/debug/orbit \
  ProxySessionIntegrationTest.test_graceful_shutdown_waits_for_active_session
```

For a direct CMake build without development test dependencies, use
`cmake -S . -B build/production -G Ninja -DCMAKE_BUILD_TYPE=Release -DORBIT_BUILD_TESTS=OFF`
and `cmake --build build/production`. The direct build uses the selected system compiler
and standard library, which must support C++23, including `std::expected` and `std::format`.

## Test deadlines and diagnostics

Deadline regression fixtures advance their own monotonic clock while dispatching controlled
readiness events. Optional socket callbacks count actual operations and can advance the clock
after a real syscall, before the reactor observes its result. Fixtures keep callback state alive
through reactor teardown. Tests that exercise timerfd arming and epoll use the system clock.

GoogleTest discovery and each unit test have a 15-second limit. CTest runs the Python suite
serially with a 45-second limit and terminates the test process tree if it exceeds that limit.
Each fixture owns one direct child and the client and backend sockets. The transfer workers
only perform writes on sockets with timeouts.
The harness allows five seconds for startup readiness, socket I/O, a complete fixed-size read
or expected shutdown. Cleanup closes its sockets, allows five seconds for the child to exit
following SIGTERM, then uses SIGKILL and waits up to two seconds to reap it. Having to kill
the child, or observing a nonzero exit including during shutdown, fails the test.

The harness uses the existing Orbit startup log message to avoid spare probe connections.
Shutdown synchronization and the idle CPU test need access to the child process's
`/proc/<pid>/status` and `/proc/<pid>/stat`. The integration suite also needs IPv4 loopback
TCP. The test backend retains its OS-assigned port. Orbit's public CLI requires a nonzero
port, so the harness releases a bound probe socket before Orbit binds. A concurrent process
can still acquire that port in the gap. The resulting startup error reports the endpoint and
exit status.

CTest displays failures and writes the most recent run to
`build/<preset>/Testing/Temporary/LastTest.log`. Use `--output-log <path>` to preserve a
separate log per test category. The harness writes combined child stdout and stderr to a
temporary file and includes its command, PID, exit status and up to 32 KiB of child output
in failure diagnostics. For larger output it includes the beginning and end. The temporary
file is closed on teardown. A hard CTest timeout can prevent the harness from printing its
final diagnostics.

## Sanitizer reports

The sanitizer preset enables stack use-after-scope checks, keeps frame pointers and disables
UBSan recovery. It explicitly enables leak detection and UBSan stack traces in the test
environment, including for Orbit subprocesses. Clang can use `llvm-symbolizer` on `PATH` or
fall back to `addr2line` from binutils. Inspect the first reported error and its source stack.
ASan reports allocation and free stacks where applicable. Test success requires a clean child
exit, including the shutdown-time leak check. LeakSanitizer needs process inspection via
`ptrace`, which may be blocked by sandboxes, containers or an attached debugger. Run the
same preset in an environment that permits the check.

Orbit targets and the Abseil library use matching instrumentation because Abseil's hash-table
layout changes under ASan. GCC 15 builds the ordinary presets, but its UBSan build fails to
compile the pinned Abseil headers. See [LIM-017](known-limitations.md#lim-017--gnu-ubsan-and-abseil-are-incompatible).

## Continuous integration

The [CI workflow](../.github/workflows/ci.yaml) runs on pushes to `main` and pull requests
targeting `main`. Each Ubuntu 26.04 job runs one of the three presets with separate unit and
integration steps. An integration step still runs after a unit failure if the build succeeded.
Matrix jobs have independent results and a 20-minute deadline. Failed jobs upload available
CMake configure and CTest logs as `diagnostics-<preset>` for seven days. Package installation
and build output are available in the job log.
