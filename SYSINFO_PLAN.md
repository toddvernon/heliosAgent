# sysinfo verb: agent-side system stats (scope, 2026-07-07)

**STATUS: SHIPPED + VALIDATED 2026-07-07** (agent 0.2.0). Built and unit-tested
on macOS (155/0), NetBSD 9.2 (155/0), Solaris 2.6 (151/0), SunOS 4.1.4 (151/0);
deployed to all three guest images; every field cross-checked against the
native tools on each guest (sysctl/swapctl/uptime/df, prtconf/swap -s,
dmesg/pstat -s). The SunOS kmem fault drill passed: with HELIOS_KERNEL pointed
at a bogus kernel, hello answers, sysinfo stays ok:true, and exactly the three
kmem fields (memMB/load/swap) go absent. One porting fix along the way: SunOS
4's K&R headers declare struct statfs / struct nlist but not the same-named
functions, so the C++ compile needed explicit extern "C" prototypes (see
SysInfo.cpp). Remaining: the real SS5 install (Todd), and the macXserver-side
prober/display (swift-x repo).

macXserver wants to show live system facts for every box running the agent:
identity, memory, swap, load, disk fullness, clock. The gather happens agent
side (we run as root, we can read anything) via a new `sysinfo` verb.

## The prime directive: the agent must never get worse

The agent gates everything macXserver does to a guest (file browser, launch,
shutdown, DNS admin). A bug in stats collection that hangs, crashes, or leaks
the agent costs far more than the stats are worth. So:

1. **hello stays untouched.** hello is the liveness ping and must stay
   bulletproof. sysinfo is a separate verb; if it breaks, hello still answers
   and everything else still works.
2. **Every field is optional, independently.** Each collector either produces
   its field or the field is absent from the response. A collector failure is
   silent on the wire (absent key), never an error response, never a partial
   JSON, never an abort.
3. **No forks, no popen, no shelling out.** Everything comes from syscalls,
   libc, or /dev/kmem reads. Nothing can hang on a pipe, leave a zombie, or
   depend on PATH. (This is the reason to do it agent side at all; the app
   could already get these numbers by run_command'ing prtconf/pstat/uptime.)
4. **Bounded everything.** Fixed-size buffers, disk list capped (8 mounts),
   one bounded read per kmem symbol. Response stays well under a KB.
5. **Startup caching is defensive.** Static facts (uname, hostid, physmem,
   kmem symbol addresses) are collected once at startup inside the same
   never-fail wrappers. A failed startup collector marks its field
   permanently absent; it must not stop the agent from serving.
6. **Auth unchanged.** sysinfo sits behind the same fail-closed auth gate as
   every other verb.

## Verb shape

Request:  `{ "verb": "sysinfo", "auth": "...", "id": 7 }`

Response (every key inside `result` optional):

```json
{ "id": 7, "ok": true, "result": {
    "uname":   { "sysname": "SunOS", "release": "4.1.4",
                 "machine": "sun4m", "nodename": "sunos" },
    "hostid":  "80eff4e5",
    "memMB":   112,
    "swap":    { "totalKB": 262144, "usedKB": 47000 },
    "load":    [0.12, 0.08, 0.03],
    "disks":   [ { "mount": "/",    "sizeKB": 245000, "usedPct": 61 },
                 { "mount": "/usr", "sizeKB": 812000, "usedPct": 74 } ],
    "time":    1783300000,
    "agentUptime": 259200
} }
```

Notes on fields:
- `memMB` is physical RAM as the kernel sees it (SunOS reports less than
  installed because the kernel eats some; that's fine, it's the truth).
- `load` is the 1/5/15 averages as floats.
- `disks` reports real local filesystems only (skip nfs/proc/swap mounts),
  capped at 8, from getmntent/getmntinfo; if mount enumeration fails, fall
  back to trying the fixed set "/", "/usr", "/var", "/home" and skipping
  ENOENT.
- `time` is guest epoch seconds; the app compares against Mac time and shows
  drift when it matters.
- `agentUptime` duplicates hello's uptime so a sysinfo alone is self-contained.

## Per-field source matrix

The whole point of the scoping: where each number comes from on each OS,
fork-free. The reference implementation for nearly all of this is top 3.x's
machine-dependent modules (m_sunos4.c, m_sunos5.c, m_netbsd*.c), which solved
exactly this problem on exactly these systems. Lift, don't re-derive.

| Field   | SunOS 4.1.4 (`_SUNOS_`)             | Solaris 2.6 (`_SOLARIS6_`)                  | NetBSD 9.2 (`_NETBSD_`)          |
|---------|--------------------------------------|---------------------------------------------|----------------------------------|
| uname   | uname(2)                             | uname(2)                                    | uname(2)                         |
| hostid  | gethostid(2)                         | sysinfo(SI_HW_SERIAL) or gethostid          | gethostid(2)                     |
| memMB   | kmem: nlist("/vmunix","_physmem"), read pages, * pagesize | sysconf(_SC_PHYS_PAGES) * _SC_PAGESIZE | sysctl(HW_PHYSMEM64)             |
| load    | kmem: nlist "_avenrun", 3 longs, / FSCALE | kstat: unix:system_misc avenrun_* (or kmem fallback) | getloadavg(3)                    |
| swap    | kmem: "_anoninfo" (ani_max/ani_free), pages | swapctl(SC_AINFO) anoninfo               | swapctl(SWAP_STATS/SWAP_NSWAP)   |
| disks   | statfs(2) + getmntent(/etc/mtab)     | statvfs(2) + getmntent(/etc/mnttab)         | statvfs(2) + getmntinfo(3)       |
| time    | time(2)                              | time(2)                                     | time(2)                          |

The SunOS 4 column is the fiddly one and the reason the agent's root privilege
matters: physmem/avenrun/anonymous-swap live in the kernel, read via
nlist(3) on /vmunix once at startup (cache the addresses) plus a short
lseek+read on /dev/kmem per call. That is precisely how ps, pstat, and uptime
do it on that OS. Guards: nlist failure, /vmunix missing (booted kernel named
differently), kmem open/seek/read failure. Any of those = fields absent,
agent unbothered.

Solaris 2.6 has no getloadavg (arrived in Solaris 8); kstat is the clean
source. If linking libkstat annoys the build, the kmem approach works there
too (avenrun via nlist on /dev/ksyms). Decide when building; kstat first.

## Code structure

- `SysInfo.h` / `SysInfo.cpp`, new files beside Verbs.cpp. One
  `struct SysInfoSnapshot` of value+present flags; `sysInfoStartup()` called
  once from HeliosAgent.cpp main (collect statics, nlist, never fatal);
  `sysInfoCollect(SysInfoSnapshot&)` per call (dynamics).
- Platform splits via the existing `_SUNOS_` / `_SOLARIS6_` / `_NETBSD_`
  compile macros (platform.mk), same pattern as the grep-binary switch in
  Verbs.cpp. `_OSX_` builds (dev machine) get uname/load/disks/time via the
  BSD paths so the verb is testable on the Mac.
- `verbSysInfo(id)` in Verbs.cpp assembles JSON from the snapshot, appending
  only present fields. Dispatch.cpp gets one else-if.
- SunOS 4 portability rules already learned apply (getopt decl, vsnprintf
  shim, K&R-tolerant C++ for gcc on 4.1.4; see the cx porting notes).

## Versioning + rollout

- Additive verb: no HELIOS_PROTOCOL_VERSION bump needed. Old agents answer
  `{"ok":false,"error":"unknown verb: sysinfo"}`; the app treats that as
  "agent predates sysinfo" and simply shows less. Bump HELIOS_VERSION to
  0.2.0 so hello reveals who has it.
- macXserver side (separate change, swift-x repo): the external-host prober
  calls hello for liveness, then sysinfo best-effort; emulated machines get
  the same sysinfo call once ready. Absent fields just don't render. A
  sysinfo failure NEVER affects reachability state; only hello decides that.
- Deploy via the existing per-OS deploy.sh path to the three guest images,
  then the real SS5 (never clobber its /etc/helios/helios.json).

## Test plan

- helios_test.cpp gains a sysinfo case (schema-level: ok:true, result is an
  object, no required fields).
- Per guest, telnet-test the verb and cross-check numbers against the native
  tools: prtconf/swap -s/uptime (Solaris), dmesg mem line/pstat -s/uptime
  (SunOS), sysctl hw.physmem64/swapctl -s/uptime (NetBSD).
- Fault drills, at least on one OS: run with a bogus /vmunix name (nlist
  fails), chmod /dev/kmem unreadable, confirm: agent up, hello fine, sysinfo
  returns ok:true with the affected fields absent.
- Soak: hammer sysinfo in a loop for an hour on the slowest guest (4.1.4),
  watch agent RSS and fd count for leaks (kmem fd must not leak per call).

## Effort guess

Half a day for NetBSD + Solaris + the verb + tests (their sources are all
clean calls), plus half a day for the SunOS 4 kmem trio and the fault drills,
plus the usual deploy round. The kmem code is the only part with real teeth,
and top's m_sunos4.c is a working crib for every line of it.
