//-------------------------------------------------------------------------------------------------
//
//  SysInfo.h
//  heliosAgent
//
//  Fork-free system stats for the `sysinfo` verb: identity, memory, swap,
//  load, disk fullness, clock. Everything comes from syscalls / libc / kernel
//  memory -- never a shell-out -- so nothing here can hang on a pipe, leave a
//  zombie, or depend on PATH.
//
//  THE PRIME DIRECTIVE (see SYSINFO_PLAN.md): the agent gates everything
//  macXserver does to a guest, so stats collection must never be able to make
//  the agent worse. Every field is independently optional: each collector
//  either produces its value and sets the matching have-flag, or leaves the
//  flag 0 and the field is simply absent from the response. No collector may
//  block, abort, or error the verb.
//
//  sysInfoStartup() runs once from main(), AFTER daemonization (it may open a
//  long-lived /dev/kmem fd on SunOS 4, which a later daemonize would close).
//  It caches what can be cached (kernel symbol addresses); a failed startup
//  marks the affected fields permanently absent and the agent serves on.
//
//-------------------------------------------------------------------------------------------------

#ifndef HELIOS_SYSINFO_H
#define HELIOS_SYSINFO_H

#define SYSINFO_STR       128
#define SYSINFO_MAX_DISKS 8

struct SysInfoDisk {
    char   mount[SYSINFO_STR];
    double sizeKB;
    int    usedPct;
};

struct SysInfoSnapshot {
    int    haveUname;
    char   sysname[SYSINFO_STR];
    char   release[SYSINFO_STR];
    char   machine[SYSINFO_STR];
    char   nodename[SYSINFO_STR];

    int    haveHostid;
    char   hostid[32];            // hex, no 0x prefix (matches hostid(1))

    int    haveMemMB;
    double memMB;                 // physical RAM as the kernel reports it

    int    haveSwap;
    double swapTotalKB;
    double swapUsedKB;

    int    haveLoad;
    double load1;
    double load5;
    double load15;

    int          diskCount;       // 0 = none collected
    SysInfoDisk  disks[SYSINFO_MAX_DISKS];

    long   guestTime;             // epoch seconds, always present (time(2))
};

// One-time setup (kernel symbol lookup + /dev/kmem on SunOS 4; no-op
// elsewhere). Never fatal. Call after daemonization.
void sysInfoStartup( void );

// Fill a snapshot, best-effort. Never fails; check the have-flags.
void sysInfoCollect( SysInfoSnapshot &snap );

#endif
