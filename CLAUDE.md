# heliosAgent project instructions

## Overview
heliosAgent is the Helios guest agent: a small TCP daemon that runs inside the
SPARCplug emulated Solaris 2.6 image (and, later, a real Sun) and proxies
command execution and filesystem access to a Mac-side client over one
newline-delimited JSON request/response channel. It is built on the cx library.

The product context lives in the swift-x tree (`~/dev/X`): `Helios-Mission.md`
(the why), `HELIOS_PLAN.md` (the phased build plan), `DECISIONS.md`
(2026-06-17 access model, 2026-06-20 mission refinement). The wire protocol is
`PROTOCOL.md` in this directory.

## Naming
- **"cx"** -- the shared library at `../../cx` (modules base, net, json, b64,
  log, process, ...).
- **"the agent" / "heliosAgent"** -- this app (`cx/cx_apps/heliosAgent`).

## Build
```bash
make            # builds <uname_s>_<arch>/heliosAgent
make clean
make test       # builds + runs the unit suite (test/)

# Run (dev: foreground, log to stderr):
./darwin_arm64/heliosAgent [-p port]        # default port 2125
./darwin_arm64/heliosAgent 2125             # legacy positional port still works

# Run (service: detach, log to file, write pidfile) -- what the init script does:
./darwin_arm64/heliosAgent -d -p 2125 -l /var/log/heliosAgent.log -P /var/run/heliosAgent.pid
```
Flags: `-d` daemonize (double-fork/setsid/redirect stdio), `-p` port, `-l`
append logfile (CxLogFile, pid+timestamp per line), `-P` pidfile. SIGTERM stops
cleanly and removes the pidfile; the listening socket sets SO_REUSEADDR so a
restart doesn't trip over TIME_WAIT.

On the Sun, after `make`, run `./deploy.sh` as root: it installs the binary to
`/usr/local/bin`, drops `init/heliosAgent` into `/etc/init.d`, wires the rc
symlinks (S98 multiuser / K30 shutdown+single-user), and (re)starts the daemon.
It's idempotent, so it's also the upgrade path (rebuild, re-run). Always run a
full `make` after changes. Build output dirs are git-ignored and must not be
committed or hand-edited.

## Non-negotiable constraints (same as cx and cm)
- NO `std::`, NO STL, NO templates, NO new external dependencies. Use cx
  utilities (CxString, CxSList, CxJSON*, CxSocket, CxFile, CxProcess, ...).
- Old-toolchain safe (g++ 2.95 / `_SOLARIS6_`): avoid auto, nullptr, lambda,
  range-for, threads, regex. Exceptions are used by cx's net layer
  (CxSocketException) -- catch them with `catch (...)`, the established idiom;
  do not introduce new exception-based control flow of our own.
- DO NOT define classes inside function bodies (gcc 2.95 DWARF crash). File
  scope only.
- Portable makefile only: no GNU make extensions (`filter`, `wildcard`,
  `patsubst`, `else ifeq`). Mirror the patterns in this makefile and cm's.

## Conventions
- Includes use the `<cx/module/header.h>` form; the makefile's `-I../..` makes
  that resolve.
- Build JSON responses with the cx JSON object API (not sprintf) so string
  values are correctly escaped on the wire. Building on a stack root and letting
  scope delete it frees the whole tree.
- One function per verb in `Verbs.cpp`; routing in `Dispatch.cpp`; the server
  loop in `HeliosAgent.cpp`. Protocol errors return ok:false, never drop the
  connection.

## Develop on the Mac, validate on Solaris
cx is cross-platform, so build and exercise the daemon on macOS against
localhost first; recompiling and validating on the 2.6 image is a separate step,
not the dev loop.
