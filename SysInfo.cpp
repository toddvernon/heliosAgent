//-------------------------------------------------------------------------------------------------
//
//  SysInfo.cpp
//  heliosAgent
//
//  Fork-free stats collectors behind the `sysinfo` verb. One platform block
//  per OS (the same _SUNOS_ / _SOLARIS6_ / _NETBSD_ / _OSX_ macros platform.mk
//  compiles with), each collector wrapped so failure = field absent, never an
//  error. See SysInfo.h for the prime directive and SYSINFO_PLAN.md for the
//  per-field source matrix. The SunOS 4 kernel-memory technique (nlist on
//  /vmunix + /dev/kmem reads for physmem / avenrun / anoninfo) is how ps,
//  pstat, and uptime themselves work on that OS; top 3.x's m_sunos4.c is the
//  reference implementation.
//
//-------------------------------------------------------------------------------------------------

#include "SysInfo.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/utsname.h>

//-------------------------------------------------------------------------
// small helpers (no allocation, all bounded)
//-------------------------------------------------------------------------
static void
copyStr( char *dst, int cap, const char *src )
{
    if ( cap <= 0 ) {
        return;
    }
    if ( src == (const char*)0 ) {
        dst[0] = '\0';
        return;
    }
    strncpy( dst, src, cap - 1 );
    dst[cap - 1] = '\0';
}

// df-style fullness: used / (used + available-to-users), rounded.
static int
usedPercent( double used, double avail )
{
    double denom = used + avail;
    if ( denom <= 0.0 ) {
        return 0;
    }
    return (int)( ( used * 100.0 ) / denom + 0.5 );
}

// A real local filesystem mounts a /dev-style path; NFS sources are
// "host:/path", pseudo filesystems are bare names ("proc", "mfs", "kernfs",
// "swap"). One test that holds on all four platforms.
static int
isLocalDisk( const char *source, double sizeKB )
{
    if ( source == (const char*)0 || source[0] != '/' ) {
        return 0;
    }
    if ( sizeKB <= 0.0 ) {
        return 0;
    }
    return 1;
}

static void
addDisk( SysInfoSnapshot &snap, const char *mount, double sizeKB, int pct )
{
    if ( snap.diskCount >= SYSINFO_MAX_DISKS ) {
        return;
    }
    SysInfoDisk &d = snap.disks[ snap.diskCount ];
    copyStr( d.mount, SYSINFO_STR, mount );
    d.sizeKB  = sizeKB;
    d.usedPct = pct;
    snap.diskCount++;
}

//-------------------------------------------------------------------------
// uname + hostid (portable; Solaris hostid is special-cased below)
//-------------------------------------------------------------------------
static void
collectUname( SysInfoSnapshot &snap )
{
    struct utsname un;
    if ( uname( &un ) < 0 ) {
        return;
    }
    copyStr( snap.sysname,  SYSINFO_STR, un.sysname );
    copyStr( snap.release,  SYSINFO_STR, un.release );
    copyStr( snap.machine,  SYSINFO_STR, un.machine );
    copyStr( snap.nodename, SYSINFO_STR, un.nodename );
    snap.haveUname = 1;
}

//=========================================================================
//  macOS (dev builds -- exercises the verb + BSD-ish paths on the Mac)
//=========================================================================
#if defined(_OSX_)

#include <sys/sysctl.h>
#include <sys/mount.h>

static void
collectHostid( SysInfoSnapshot &snap )
{
    long id = gethostid();
    sprintf( snap.hostid, "%08lx", (unsigned long)id & 0xffffffffUL );
    snap.haveHostid = 1;
}

static void
collectMem( SysInfoSnapshot &snap )
{
    int64_t bytes = 0;
    size_t  len   = sizeof( bytes );
    int     mib[2];
    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;
    if ( sysctl( mib, 2, &bytes, &len, (void*)0, 0 ) != 0 || bytes <= 0 ) {
        return;
    }
    snap.memMB   = (double)bytes / ( 1024.0 * 1024.0 );
    snap.haveMemMB = 1;
}

static void
collectLoad( SysInfoSnapshot &snap )
{
    double lav[3];
    if ( getloadavg( lav, 3 ) != 3 ) {
        return;
    }
    snap.load1 = lav[0]; snap.load5 = lav[1]; snap.load15 = lav[2];
    snap.haveLoad = 1;
}

static void
collectSwap( SysInfoSnapshot &snap )
{
    struct xsw_usage xsw;
    size_t len = sizeof( xsw );
    if ( sysctlbyname( "vm.swapusage", &xsw, &len, (void*)0, 0 ) != 0 ) {
        return;
    }
    snap.swapTotalKB = (double)xsw.xsu_total / 1024.0;
    snap.swapUsedKB  = (double)xsw.xsu_used  / 1024.0;
    snap.haveSwap = 1;
}

static void
collectDisks( SysInfoSnapshot &snap )
{
    struct statfs *mnt = (struct statfs*)0;
    int n = getmntinfo( &mnt, MNT_NOWAIT );
    for ( int i = 0; i < n; i++ ) {
        double bsize  = (double)mnt[i].f_bsize;
        double sizeKB = (double)mnt[i].f_blocks * bsize / 1024.0;
        if ( ! isLocalDisk( mnt[i].f_mntfromname, sizeKB ) ) {
            continue;
        }
        double used  = (double)( mnt[i].f_blocks - mnt[i].f_bfree ) * bsize / 1024.0;
        double avail = (double)mnt[i].f_bavail * bsize / 1024.0;
        addDisk( snap, mnt[i].f_mntonname, sizeKB, usedPercent( used, avail ) );
    }
}

void
sysInfoStartup( void )
{
    // Nothing to cache on macOS.
}

//=========================================================================
//  NetBSD 9.x
//=========================================================================
#elif defined(_NETBSD_)

#include <sys/sysctl.h>
#include <sys/swap.h>
#include <sys/statvfs.h>

static void
collectHostid( SysInfoSnapshot &snap )
{
    long id = gethostid();
    sprintf( snap.hostid, "%08lx", (unsigned long)id & 0xffffffffUL );
    snap.haveHostid = 1;
}

static void
collectMem( SysInfoSnapshot &snap )
{
    uint64_t bytes = 0;
    size_t   len   = sizeof( bytes );
    int      mib[2];
    mib[0] = CTL_HW;
    mib[1] = HW_PHYSMEM64;
    if ( sysctl( mib, 2, &bytes, &len, (void*)0, 0 ) != 0 || bytes == 0 ) {
        return;
    }
    snap.memMB   = (double)bytes / ( 1024.0 * 1024.0 );
    snap.haveMemMB = 1;
}

static void
collectLoad( SysInfoSnapshot &snap )
{
    double lav[3];
    if ( getloadavg( lav, 3 ) != 3 ) {
        return;
    }
    snap.load1 = lav[0]; snap.load5 = lav[1]; snap.load15 = lav[2];
    snap.haveLoad = 1;
}

static void
collectSwap( SysInfoSnapshot &snap )
{
    // swapctl blocks are DEV_BSIZE (512): KB = blocks / 2.
    int n = swapctl( SWAP_NSWAP, (void*)0, 0 );
    if ( n <= 0 ) {
        if ( n == 0 ) {                     // no swap configured: report 0/0
            snap.swapTotalKB = 0.0;
            snap.swapUsedKB  = 0.0;
            snap.haveSwap = 1;
        }
        return;
    }
    if ( n > 16 ) {
        n = 16;                             // bounded; nobody has 17 swap devices
    }
    struct swapent se[16];
    int got = swapctl( SWAP_STATS, (void*)se, n );
    if ( got <= 0 ) {
        return;
    }
    double totalKB = 0.0, usedKB = 0.0;
    for ( int i = 0; i < got; i++ ) {
        totalKB += (double)se[i].se_nblks / 2.0;
        usedKB  += (double)se[i].se_inuse / 2.0;
    }
    snap.swapTotalKB = totalKB;
    snap.swapUsedKB  = usedKB;
    snap.haveSwap = 1;
}

static void
collectDisks( SysInfoSnapshot &snap )
{
    struct statvfs *mnt = (struct statvfs*)0;
    int n = getmntinfo( &mnt, MNT_NOWAIT );
    for ( int i = 0; i < n; i++ ) {
        double bsize  = (double)mnt[i].f_frsize;
        if ( bsize <= 0.0 ) {
            bsize = (double)mnt[i].f_bsize;
        }
        double sizeKB = (double)mnt[i].f_blocks * bsize / 1024.0;
        if ( ! isLocalDisk( mnt[i].f_mntfromname, sizeKB ) ) {
            continue;
        }
        double used  = (double)( mnt[i].f_blocks - mnt[i].f_bfree ) * bsize / 1024.0;
        double avail = (double)mnt[i].f_bavail * bsize / 1024.0;
        addDisk( snap, mnt[i].f_mntonname, sizeKB, usedPercent( used, avail ) );
    }
}

void
sysInfoStartup( void )
{
    // Nothing to cache on NetBSD.
}

//=========================================================================
//  Solaris 2.6 / 10 (SVR4)
//=========================================================================
#elif defined(_SOLARIS6_) || defined(_SOLARIS10_)

#include <sys/systeminfo.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/statvfs.h>
#include <sys/mnttab.h>
#include <kstat.h>

static void
collectHostid( SysInfoSnapshot &snap )
{
    // sysinfo(SI_HW_SERIAL) returns the hostid as a DECIMAL string; hostid(1)
    // displays hex, so convert.
    char buf[64];
    if ( sysinfo( SI_HW_SERIAL, buf, sizeof( buf ) ) < 0 ) {
        return;
    }
    unsigned long id = strtoul( buf, (char**)0, 10 );
    sprintf( snap.hostid, "%08lx", id & 0xffffffffUL );
    snap.haveHostid = 1;
}

static void
collectMem( SysInfoSnapshot &snap )
{
    long pages = sysconf( _SC_PHYS_PAGES );
    long psize = sysconf( _SC_PAGESIZE );
    if ( pages <= 0 || psize <= 0 ) {
        return;
    }
    snap.memMB   = (double)pages * (double)psize / ( 1024.0 * 1024.0 );
    snap.haveMemMB = 1;
}

static void
collectLoad( SysInfoSnapshot &snap )
{
    // 2.6 predates getloadavg (Solaris 8); the documented source is kstat's
    // unix:system_misc avenrun_* (32-bit fixed point, scale 256). Open/close
    // per call: cheap at a minutes cadence and immune to kstat chain updates.
    kstat_ctl_t *kc = kstat_open();
    if ( kc == (kstat_ctl_t*)0 ) {
        return;
    }
    kstat_t *ks = kstat_lookup( kc, (char*)"unix", 0, (char*)"system_misc" );
    if ( ks == (kstat_t*)0 || kstat_read( kc, ks, (void*)0 ) < 0 ) {
        kstat_close( kc );
        return;
    }
    static const char *names[3] = { "avenrun_1min", "avenrun_5min", "avenrun_15min" };
    double vals[3];
    int i;
    for ( i = 0; i < 3; i++ ) {
        kstat_named_t *kn = (kstat_named_t*) kstat_data_lookup( ks, (char*)names[i] );
        if ( kn == (kstat_named_t*)0 ) {
            kstat_close( kc );
            return;
        }
        vals[i] = (double)kn->value.ui32 / 256.0;
    }
    kstat_close( kc );
    snap.load1 = vals[0]; snap.load5 = vals[1]; snap.load15 = vals[2];
    snap.haveLoad = 1;
}

static void
collectSwap( SysInfoSnapshot &snap )
{
    // Anonymous-memory accounting, same numbers `swap -s` reports: ani_max =
    // total reservable pages, ani_resv = reserved. Reserved-vs-touched nuance
    // doesn't matter for a display gauge.
    struct anoninfo ai;
    if ( swapctl( SC_AINFO, &ai ) < 0 ) {
        return;
    }
    long psize = sysconf( _SC_PAGESIZE );
    if ( psize <= 0 ) {
        return;
    }
    double pageKB = (double)psize / 1024.0;
    snap.swapTotalKB = (double)ai.ani_max  * pageKB;
    snap.swapUsedKB  = (double)ai.ani_resv * pageKB;
    snap.haveSwap = 1;
}

static void
collectDisks( SysInfoSnapshot &snap )
{
    FILE *fp = fopen( "/etc/mnttab", "r" );
    if ( fp == (FILE*)0 ) {
        return;
    }
    struct mnttab mt;
    while ( getmntent( fp, &mt ) == 0 ) {
        struct statvfs sv;
        if ( statvfs( mt.mnt_mountp, &sv ) != 0 ) {
            continue;
        }
        double bsize  = (double)( sv.f_frsize > 0 ? sv.f_frsize : sv.f_bsize );
        double sizeKB = (double)sv.f_blocks * bsize / 1024.0;
        if ( ! isLocalDisk( mt.mnt_special, sizeKB ) ) {
            continue;
        }
        double used  = (double)( sv.f_blocks - sv.f_bfree ) * bsize / 1024.0;
        double avail = (double)sv.f_bavail * bsize / 1024.0;
        addDisk( snap, mt.mnt_mountp, sizeKB, usedPercent( used, avail ) );
    }
    fclose( fp );
}

void
sysInfoStartup( void )
{
    // Nothing to cache on Solaris (kstat opens per call).
}

//=========================================================================
//  IRIX 6.5
//
//  Memory comes from sysmp(MP_SAGET, MPSA_RMINFO) -- the same interface
//  top/osview use, no kernel memory involved. Swap totals come from IRIX's
//  swapctl query extensions (SC_GETSWAPTOT / SC_GETFREESWAP, units of
//  512-byte blocks). Load average has no syscall on 6.5; era-correct
//  practice (xload, X11R6 contrib get_load.c, `#ifdef sgi`) is nlist(3) on
//  /unix for `avenrun` (no underscore, FSCALE 1024) plus a /dev/kmem read
//  -- the same machinery as the SunOS 4 branch below. Every step guarded: a
//  failed nlist or open marks load permanently absent and the agent serves
//  on. Disks are SVR4-style: getmntent over /etc/mtab + statvfs.
//=========================================================================
#elif defined(_IRIX6_)

#include <nlist.h>
#include <sys/sysmp.h>
#include <sys/sysinfo.h>
#include <sys/swap.h>
#include <sys/statvfs.h>
#include <mntent.h>

// Kernel fixed-point scale for avenrun on sgi (xload get_load.c).
#define HELIOS_IRIX_FSCALE 1024.0

// avenrun's address in /unix, cached by sysInfoStartup; 0 = unavailable.
static unsigned long s_addrAvenrun = 0;
static int           s_kmemFd      = -1;

// Bounded positioned read from kernel memory. 0 on failure.
static int
kmemRead( unsigned long addr, void *dst, int len )
{
    if ( s_kmemFd < 0 || addr == 0 ) {
        return 0;
    }
    if ( lseek( s_kmemFd, (off_t)addr, SEEK_SET ) == (off_t)-1 ) {
        return 0;
    }
    if ( read( s_kmemFd, dst, len ) != len ) {
        return 0;
    }
    return 1;
}

static void
collectHostid( SysInfoSnapshot &snap )
{
    long id = gethostid();
    sprintf( snap.hostid, "%08lx", (unsigned long)id & 0xffffffffUL );
    snap.haveHostid = 1;
}

static void
collectMem( SysInfoSnapshot &snap )
{
    struct rminfo rm;
    if ( sysmp( MP_SAGET, MPSA_RMINFO, (char*)&rm, (int)sizeof( rm ) ) < 0 ) {
        return;
    }
    if ( rm.physmem == 0 ) {
        return;
    }
    snap.memMB   = (double)rm.physmem * (double)getpagesize() / ( 1024.0 * 1024.0 );
    snap.haveMemMB = 1;
}

static void
collectLoad( SysInfoSnapshot &snap )
{
    long avenrun[3];
    if ( ! kmemRead( s_addrAvenrun, avenrun, sizeof( avenrun ) ) ) {
        return;
    }
    snap.load1  = (double)avenrun[0] / HELIOS_IRIX_FSCALE;
    snap.load5  = (double)avenrun[1] / HELIOS_IRIX_FSCALE;
    snap.load15 = (double)avenrun[2] / HELIOS_IRIX_FSCALE;
    snap.haveLoad = 1;
}

static void
collectSwap( SysInfoSnapshot &snap )
{
    off_t total = 0;
    off_t freeb = 0;
    if ( swapctl( SC_GETSWAPTOT, &total ) < 0 ) {
        return;
    }
    if ( swapctl( SC_GETFREESWAP, &freeb ) < 0 ) {
        return;
    }
    if ( total <= 0 ) {
        return;
    }
    // 512-byte blocks -> KB.
    snap.swapTotalKB = (double)total / 2.0;
    snap.swapUsedKB  = (double)( total - freeb ) / 2.0;
    snap.haveSwap = 1;
}

static void
collectDisks( SysInfoSnapshot &snap )
{
    FILE *fp = setmntent( MOUNTED, "r" );     // /etc/mtab
    if ( fp == (FILE*)0 ) {
        return;
    }
    struct mntent *mt;
    while ( ( mt = getmntent( fp ) ) != (struct mntent*)0 ) {
        // IRIX pseudo filesystems mount from a '/'-prefixed source (/proc,
        // /hw), so the shared isLocalDisk source test can't screen them
        // (validated live: /proc reported itself as a 500MB disk). Gate on
        // the fs type instead: efs and xfs are the local-disk types.
        if ( strcmp( mt->mnt_type, "efs" ) != 0 &&
             strcmp( mt->mnt_type, "xfs" ) != 0 ) {
            continue;
        }
        struct statvfs sv;
        if ( statvfs( mt->mnt_dir, &sv ) != 0 ) {
            continue;
        }
        double bsize  = (double)( sv.f_frsize > 0 ? sv.f_frsize : sv.f_bsize );
        double sizeKB = (double)sv.f_blocks * bsize / 1024.0;
        if ( ! isLocalDisk( mt->mnt_fsname, sizeKB ) ) {
            continue;
        }
        double used  = (double)( sv.f_blocks - sv.f_bfree ) * bsize / 1024.0;
        double avail = (double)sv.f_bavail * bsize / 1024.0;
        addDisk( snap, mt->mnt_dir, sizeKB, usedPercent( used, avail ) );
    }
    endmntent( fp );
}

void
sysInfoStartup( void )
{
    // Preferred source for avenrun's address: ask the RUNNING kernel via
    // sysmp(MP_KERNADDR, MPKA_AVENRUN) -- no dependence on /unix matching
    // the booted kernel, no symbol table needed. Returns -1 on failure.
    long kaddr = sysmp( MP_KERNADDR, MPKA_AVENRUN );
    if ( kaddr != -1 ) {
        s_addrAvenrun = (unsigned long)kaddr;
    }

    // Fallback: nlist(3) on the kernel image, the xload way (needs -lelf).
    // Kernel path overridable for odd setups (and the fault drill) via
    // HELIOS_KERNEL, same as the SunOS 4 branch.
    if ( s_addrAvenrun == 0 ) {
        const char *kernel = getenv( "HELIOS_KERNEL" );
        if ( kernel == (const char*)0 || kernel[0] == '\0' ) {
            kernel = "/unix";
        }

        struct nlist nl[2];
        memset( (char*)nl, 0, sizeof( nl ) );
        nl[0].n_name = (char*)"avenrun";
        nl[1].n_name = (char*)0;

        if ( nlist( (char*)kernel, nl ) >= 0 ) {
            s_addrAvenrun = (unsigned long)nl[0].n_value;
        }
    }

    s_kmemFd = open( "/dev/kmem", O_RDONLY );
    if ( s_kmemFd >= 0 ) {
        // Never leak the kernel-memory fd into run_command children.
        fcntl( s_kmemFd, F_SETFD, 1 );        // FD_CLOEXEC
    }
}

//=========================================================================
//  SunOS 4.1.x (BSD)
//
//  physmem / avenrun / anoninfo live in kernel memory: nlist(3) resolves
//  their addresses in /vmunix once at startup, then each collect is a short
//  lseek+read on a long-lived /dev/kmem fd (root-only, which the agent is).
//  Exactly how ps/pstat/uptime work on this OS. Every step guarded: a failed
//  nlist or open marks those fields permanently absent and the agent serves
//  on; the fault drill in the plan exercises this.
//=========================================================================
#elif defined(_SUNOS_)

#include <nlist.h>
#include <sys/vfs.h>
#include <mntent.h>

#ifndef FSCALE
#define FSCALE 256
#endif

// SunOS 4's K&R-era headers declare `struct statfs` / `struct nlist` but NOT
// the same-named functions, so a C++ compile resolves `statfs(...)` /
// `nlist(...)` to the struct's constructor and fails ("no matching function
// for call to statfs::statfs"). Declare the functions explicitly; ditto the
// implicit-decl pair so gethostid's long return isn't truncated by a guess.
extern "C" {
    int  statfs( char *path, struct statfs *buf );
    int  nlist( char *filename, struct nlist *nl );
    long gethostid( void );
    int  getpagesize( void );
}

// vm/anon.h's anoninfo, declared locally so we don't drag kernel VM headers
// into a C++ compile: three u_ints, max / free / resv (in pages). Verified
// against pstat -s on the live guest.
struct sunos4AnonInfo {
    unsigned int ani_max;
    unsigned int ani_free;
    unsigned int ani_resv;
};

// Symbol addresses cached by sysInfoStartup; 0 = unavailable.
static unsigned long s_addrPhysmem  = 0;
static unsigned long s_addrAvenrun  = 0;
static unsigned long s_addrAnoninfo = 0;
static int           s_kmemFd       = -1;

// Bounded positioned read from kernel memory. 0 on failure.
static int
kmemRead( unsigned long addr, void *dst, int len )
{
    if ( s_kmemFd < 0 || addr == 0 ) {
        return 0;
    }
    if ( lseek( s_kmemFd, (off_t)addr, SEEK_SET ) == (off_t)-1 ) {
        return 0;
    }
    if ( read( s_kmemFd, dst, len ) != len ) {
        return 0;
    }
    return 1;
}

static void
collectHostid( SysInfoSnapshot &snap )
{
    long id = gethostid();
    sprintf( snap.hostid, "%08lx", (unsigned long)id & 0xffffffffUL );
    snap.haveHostid = 1;
}

static void
collectMem( SysInfoSnapshot &snap )
{
    unsigned int pages = 0;
    if ( ! kmemRead( s_addrPhysmem, &pages, sizeof( pages ) ) || pages == 0 ) {
        return;
    }
    snap.memMB   = (double)pages * (double)getpagesize() / ( 1024.0 * 1024.0 );
    snap.haveMemMB = 1;
}

static void
collectLoad( SysInfoSnapshot &snap )
{
    long avenrun[3];
    if ( ! kmemRead( s_addrAvenrun, avenrun, sizeof( avenrun ) ) ) {
        return;
    }
    snap.load1  = (double)avenrun[0] / (double)FSCALE;
    snap.load5  = (double)avenrun[1] / (double)FSCALE;
    snap.load15 = (double)avenrun[2] / (double)FSCALE;
    snap.haveLoad = 1;
}

static void
collectSwap( SysInfoSnapshot &snap )
{
    struct sunos4AnonInfo ai;
    if ( ! kmemRead( s_addrAnoninfo, &ai, sizeof( ai ) ) ) {
        return;
    }
    double pageKB = (double)getpagesize() / 1024.0;
    snap.swapTotalKB = (double)ai.ani_max  * pageKB;
    snap.swapUsedKB  = (double)ai.ani_resv * pageKB;
    snap.haveSwap = 1;
}

static void
collectDisks( SysInfoSnapshot &snap )
{
    FILE *fp = setmntent( MOUNTED, "r" );     // /etc/mtab
    if ( fp == (FILE*)0 ) {
        return;
    }
    struct mntent *mt;
    while ( ( mt = getmntent( fp ) ) != (struct mntent*)0 ) {
        struct statfs sf;
        if ( statfs( mt->mnt_dir, &sf ) != 0 ) {
            continue;
        }
        double bsize  = (double)sf.f_bsize;
        double sizeKB = (double)sf.f_blocks * bsize / 1024.0;
        if ( ! isLocalDisk( mt->mnt_fsname, sizeKB ) ) {
            continue;
        }
        double used  = (double)( sf.f_blocks - sf.f_bfree ) * bsize / 1024.0;
        double avail = (double)sf.f_bavail * bsize / 1024.0;
        addDisk( snap, mt->mnt_dir, sizeKB, usedPercent( used, avail ) );
    }
    endmntent( fp );
}

void
sysInfoStartup( void )
{
    // The booted kernel is /vmunix on every stock 4.1.x install; overridable
    // for odd setups (and for the fault drill) via HELIOS_KERNEL.
    const char *kernel = getenv( "HELIOS_KERNEL" );
    if ( kernel == (const char*)0 || kernel[0] == '\0' ) {
        kernel = "/vmunix";
    }

    struct nlist nl[4];
    memset( (char*)nl, 0, sizeof( nl ) );
    nl[0].n_name = (char*)"_physmem";
    nl[1].n_name = (char*)"_avenrun";
    nl[2].n_name = (char*)"_anoninfo";
    nl[3].n_name = (char*)0;

    if ( nlist( (char*)kernel, nl ) >= 0 ) {
        s_addrPhysmem  = (unsigned long)nl[0].n_value;
        s_addrAvenrun  = (unsigned long)nl[1].n_value;
        s_addrAnoninfo = (unsigned long)nl[2].n_value;
    }

    s_kmemFd = open( "/dev/kmem", O_RDONLY );
    if ( s_kmemFd >= 0 ) {
        // Never leak the kernel-memory fd into run_command children.
        fcntl( s_kmemFd, F_SETFD, 1 );        // FD_CLOEXEC
    }
}

//=========================================================================
//  Anything else: identity + clock only, everything else absent.
//=========================================================================
#else

static void collectHostid( SysInfoSnapshot & ) {}
static void collectMem( SysInfoSnapshot & )    {}
static void collectLoad( SysInfoSnapshot & )   {}
static void collectSwap( SysInfoSnapshot & )   {}
static void collectDisks( SysInfoSnapshot & )  {}

void
sysInfoStartup( void )
{
}

#endif

//-------------------------------------------------------------------------
// sysInfoCollect -- fill a snapshot, best-effort, never fails
//-------------------------------------------------------------------------
void
sysInfoCollect( SysInfoSnapshot &snap )
{
    memset( (char*)&snap, 0, sizeof( snap ) );

    collectUname( snap );
    collectHostid( snap );
    collectMem( snap );
    collectLoad( snap );
    collectSwap( snap );
    collectDisks( snap );

    snap.guestTime = (long) time( (time_t*)0 );
}
