//-------------------------------------------------------------------------------------------------
//
//  HeliosAgent.cpp
//  heliosAgent -- the Helios guest agent
//
//  A small TCP daemon, baked into the SPARCplug Solaris image and started at
//  boot, that proxies command execution and filesystem access to a Mac-side
//  client over one newline-delimited JSON request/response channel. Built on cx
//  so it compiles identically on macOS (dev), Linux, and Solaris (deploy):
//  develop and test it on the Mac against localhost, then recompile and validate
//  on the 2.6 image. See PROTOCOL.md, and Helios-Mission.md / HELIOS_PLAN.md in
//  the swift-x tree for the why.
//
//-------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef _SUNOS_
// SunOS 4.1.4's pre-ANSI headers don't prototype getopt or its externs (the
// getopt(3) man page tells you to declare them yourself), and g++ rejects the
// undeclared optarg/optind. Declare them with C linkage here.
extern "C" {
    extern char *optarg;
    extern int   optind;
    extern int   opterr;
    int getopt( int, char **, const char * );
}
#endif

// IRIX's C++ headers declare signal(2)'s handler as `void (*)(...)` (SGI's
// legacy C++ linkage convention), so a proper void(int) handler won't
// convert and g++ dies with "prohibits conversion from (int) to (...)".
// Cast the handler to their type at the call sites there; everywhere else
// the handler passes through untouched. SIG_IGN/SIG_DFL need no cast (the
// header defines them in its own type).
#ifdef _IRIX6_
#define HELIOS_SIGHANDLER(f) ( (void (*)(...)) (f) )
#else
#define HELIOS_SIGHANDLER(f) (f)
#endif

#include <cx/base/string.h>
#include <cx/net/socket.h>
#include <cx/net/inaddr.h>
#include <cx/log/logfile.h>

#include <cx/json/json_factory.h>
#include <cx/json/json_base.h>
#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_boolean.h>

#include "Dispatch.h"
#include "Verbs.h"
#include "HeliosVersion.h"
#include "SysInfo.h"

// Provisional default listen port. The Mac side reaches this through a slirp
// hostfwd (127.0.0.1:<hostport> -> guest:<this>). Port discipline and bind/auth
// posture are open items (HELIOS_PLAN.md B7); override with argv[1] for now.
#define HELIOS_DEFAULT_PORT 2125

// Default shared-secret config file, read when no -s / HELIOS_SECRET / -S is
// given. JSON: { "secret": "...", "allow_open": false }, with an optional
// leading '#' comment banner. Physical servers drop the secret here; the QEMU
// guests get it from OBP via the init script's -s instead. See
// heliosLoadSecretFile() and PROTOCOL.md.
#define HELIOS_DEFAULT_SECRET_FILE "/etc/helios/helios.json"

// Daemon start time, for the hello verb's uptime field. Read by Verbs.cpp.
time_t g_heliosStartTime = 0;

// Logging. g_log is opened on a file only when -l is given; otherwise log lines
// go to stderr (the dev default). CxLogFile flushes every line (ALWAYS_FLUSH)
// and stamps each with pid + timestamp, so it survives the per-connection forks
// and the children's _exit() with no lost or interleaved output.
static CxLogFile g_log;
static int       g_logOpen = 0;

// Pidfile path (NULL unless -P given). Recorded so the SIGTERM handler and the
// shutdown verb can remove it on the way out.
static const char *g_pidPath = (const char*)0;


#ifdef _SUNOS_
// SunOS 4.1.4 libc has no vsnprintf (it postdates 4.1.x). Emulate a bounded one
// with vsprintf into a generous stack scratch, then a bounded copy. Safe here
// because every heliosLog format fits well under 1K -- the only large arg, the
// request line, is already %.140s-bounded at the call site. Stack scratch =
// thread-safe (no shared state). vsprintf isn't prototyped in 4.1.4 either.
extern "C" int vsprintf( char *, const char *, va_list );
static int
helios_vsnprintf( char *out, unsigned int n, const char *fmt, va_list ap )
{
    char scratch[ 4096 ];
    int  len = vsprintf( scratch, fmt, ap );
    unsigned int i = 0;
    if ( n == 0 ) return len;
    while ( i + 1 < n && scratch[ i ] != '\0' ) { out[ i ] = scratch[ i ]; i++; }
    out[ i ] = '\0';
    return len;
}
#define vsnprintf helios_vsnprintf
#endif

//-------------------------------------------------------------------------
// heliosLog -- one log line, to the file if open else stderr. Formats into a
// buffer first so the single variadic call can route either way (CxLogFile's
// printf is itself variadic and can't be forwarded a va_list).
//-------------------------------------------------------------------------
static void
heliosLog( int isErr, const char *fmt, ... )
{
    char buf[ 2048 ];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof(buf), fmt, ap );
    va_end( ap );
    buf[ sizeof(buf) - 1 ] = '\0';

    if ( g_logOpen ) {
        if ( isErr ) {
            g_log.printf( CXERR,  "%s", buf );
        } else {
            g_log.printf( CXINFO, "%s", buf );
        }
    } else {
        fprintf( stderr, "heliosAgent: %s\n", buf );
    }
}


//-------------------------------------------------------------------------
// writePidFile -- record our pid so an init stop/K-script can find us.
//-------------------------------------------------------------------------
static int
writePidFile( const char *path )
{
    FILE *f = fopen( path, "w" );
    if ( f == (FILE*)0 ) {
        return -1;
    }
    fprintf( f, "%ld\n", (long) getpid() );
    fclose( f );
    return 0;
}


//-------------------------------------------------------------------------
// daemonize -- detach into the background as a well-behaved SVR4 daemon:
// double-fork (so we can never reacquire a controlling terminal), new session,
// chdir off any mount, sane umask, and stdio redirected to /dev/null. The
// listening socket fd survives the forks. Returns 0 in the final daemon
// process, -1 on failure. Note: the log uses its own fd (CxLogFile), so it
// keeps working after stdio is sent to /dev/null.
//-------------------------------------------------------------------------
static int
daemonize( void )
{
    pid_t pid = fork();
    if ( pid < 0 ) return -1;
    if ( pid > 0 ) _exit( 0 );          // original parent leaves

    if ( setsid() < 0 ) return -1;      // new session, drop controlling tty

    pid = fork();
    if ( pid < 0 ) return -1;
    if ( pid > 0 ) _exit( 0 );          // session leader leaves; grandchild runs

    chdir( "/" );
    umask( 022 );

    int fd = open( "/dev/null", O_RDWR );
    if ( fd >= 0 ) {
        dup2( fd, 0 );
        dup2( fd, 1 );
        dup2( fd, 2 );
        if ( fd > 2 ) close( fd );
    }
    return 0;
}


//-------------------------------------------------------------------------
// termHandler -- clean stop on SIGTERM (what an init K-script sends): drop the
// pidfile and exit. Only async-signal-safe calls here (no logging/printf).
//-------------------------------------------------------------------------
static void
termHandler( int signo )
{
    if ( g_pidPath != (const char*)0 ) {
        unlink( g_pidPath );
    }
    _exit( 0 );
    (void) signo;
}


//-------------------------------------------------------------------------
// reapChildren
//
// SIGCHLD handler: reap every exited child so the per-connection forks don't
// become zombies. waitpid(WNOHANG) in a loop drains all pending exits in one
// delivery. We re-arm the handler because SVR4 signal() can reset disposition
// to default after delivery; errno is saved/restored since the handler runs
// asynchronously.
//-------------------------------------------------------------------------
static void
reapChildren( int signo )
{
    int savedErrno = errno;
    while ( waitpid( -1, (int*)0, WNOHANG ) > 0 ) {
        ;
    }
    signal( SIGCHLD, HELIOS_SIGHANDLER( reapChildren ) );
    errno = savedErrno;
    (void) signo;
}


//-------------------------------------------------------------------------
// performShutdown
//
// Run the real shutdown command. Called by the server AFTER the shutdown ACK
// has been sent, so the client always learns the guest is going down before it
// does.
//
// The command is overridable via HELIOS_SHUTDOWN_CMD (the dev override on the
// Mac, where the guest default would power off the developer's machine). When
// unset, the default is chosen at COMPILE time per guest OS, because this
// daemon is built on each target -- an env var can be forgotten in an rc script
// (it was, on every BSD guest), but a compiled default can't. Each is the
// guest's canonical clean halt that also exits qemu under emulation, as an
// ABSOLUTE path: this runs via /bin/sh with a minimal init PATH, so a bare
// `halt` is "command not found". See PROTOCOL.md and MachineOS.shutdownCommand
// (swift-x), which must stay in step with these.
//   Solaris 2.6  -> /usr/sbin/init 5   (SVR4; syncs + powers off)
//   SunOS 4.1.4  -> /usr/etc/halt       (BSD; syncs + halts)
//   NetBSD       -> /sbin/halt          (syncs + halts; exits qemu, no -p)
//-------------------------------------------------------------------------
static void
performShutdown( void )
{
    const char *cmd = getenv( "HELIOS_SHUTDOWN_CMD" );
    if ( cmd == (const char*)0 || cmd[0] == '\0' ) {
#if   defined(_NETBSD_)
        cmd = "/sbin/halt";
#elif defined(_SUNOS_)
        cmd = "/usr/etc/halt";
#elif defined(_IRIX6_)
        cmd = "/etc/shutdown -y -g0 -i0";   // IRIX: init 5 is NOT power-off there
#else
        cmd = "/usr/sbin/init 5";        // Solaris (and the dev/other default)
#endif
    }
    heliosLog( 0, "shutdown requested; running: %s", cmd );
    if ( g_pidPath != (const char*)0 ) {
        unlink( g_pidPath );
    }
    // Check the result: a wrong/missing command (the old silent failure mode on
    // the BSD guests) must show up in the log, not just quietly not power off.
    int rc = system( cmd );
    if ( rc != 0 ) {
        heliosLog( 1, "shutdown command failed (rc=%d): %s -- guest may not power off", rc, cmd );
    }
}


//-------------------------------------------------------------------------
// handleConnection
//
// One client, synchronous request/response over a persistent connection: read
// a line, dispatch it, write the response line, repeat until the peer closes.
// cx's net layer signals end-of-stream and write errors by throwing
// CxSocketException, so the socket calls are wrapped in catch(...) -- the
// established cx idiom (see cm/mcp_bridge.cpp).
//-------------------------------------------------------------------------
static void
handleConnection( CxSocket conn )
{
    for ( ;; ) {

        CxString line;

        try {
            line = conn.recvUntil( '\n' );       // includes the '\n'
        } catch ( ... ) {
            break;                               // peer closed the connection
        }

        // Strip the trailing newline (and a CR, for CRLF clients). Skip blank
        // lines rather than treating them as a parse error.
        while ( line.length() > 0 ) {
            int last = line.charAt( line.length() - 1 );
            if ( last == '\n' || last == '\r' ) {
                line = line.subString( 0, line.length() - 1 );
            } else {
                break;
            }
        }
        if ( line.length() == 0 ) {
            continue;
        }

        // Streaming verbs (put_file/get_file) carry a raw body after the JSON
        // header, so they need the socket directly. 0 = not streaming (fall
        // through to the normal line dispatch); 1 = handled, keep serving;
        // 2 = handled but the stream framing is unrecoverable, so close. A
        // socket error mid-stream throws -> close the connection.
        int streamed;
        try {
            streamed = heliosHandleStreaming( conn, line );
        } catch ( ... ) {
            break;
        }
        if ( streamed == 1 ) {
            continue;
        }
        if ( streamed == 2 ) {
            break;
        }

        CxString response = heliosDispatch( line );

        // Operational log: the request (truncated so a write_file's base64 blob
        // doesn't flood the log) and whether the daemon answered ok.
        int wasOk = ( response.index( CxString( "\"ok\":true" ) ) != -1 );
        heliosLog( wasOk ? 0 : 1, "req: %.140s -> %s",
                   line.data(), wasOk ? "ok" : "ERROR" );

        response.append( '\n' );

        try {
            conn.sendAtLeast( response );
        } catch ( ... ) {
            break;                               // peer went away mid-write
        }

        // The ACK is now on the wire; stop serving this connection so the
        // server can bring the system down.
        if ( heliosShutdownRequested() ) {
            break;
        }
    }

    conn.close();

    // Run the real shutdown only after the response is sent (or the peer left).
    // The intent was set during dispatch, so it's honored even if the ACK send
    // failed. In a unit test this code path never runs (no server loop).
    if ( heliosShutdownRequested() ) {
        performShutdown();
    }
}


//-------------------------------------------------------------------------
// heliosLoadSecretFile
//
// Read a JSON secret config that may carry a leading comment banner: blank
// lines or lines whose first non-blank char is '#', ahead of the JSON object
// (JSON has no native comments, so we strip the banner, then parse the rest).
// Pulls "secret" (string) and "allow_open" (bool, default false).
//
// Returns 0 on success (outSecret empty if the file has no "secret"), or -1 on
// error with outWarn set to a one-line reason. The caller treats both an empty
// secret and an error as "no secret", which -- unless allow_open -- means the
// daemon runs but denies every request. Never leaks the secret into outWarn.
//-------------------------------------------------------------------------
static int
heliosLoadSecretFile( const char *path, CxString &outSecret,
                      int &outAllowOpen, CxString &outWarn )
{
    outAllowOpen = 0;

    struct stat st;
    if ( stat( path, &st ) != 0 ) {
        outWarn = CxString( "secret file " ) + path + ": " + CxString( strerror( errno ) );
        return -1;
    }
    if ( ! S_ISREG( st.st_mode ) ) {
        outWarn = CxString( "secret file " ) + path + ": not a regular file";
        return -1;
    }

    int fd = open( path, O_RDONLY );
    if ( fd < 0 ) {
        outWarn = CxString( "secret file " ) + path + ": " + CxString( strerror( errno ) );
        return -1;
    }

    unsigned int size = (unsigned int) st.st_size;
    char *buf = (char*) malloc( size + 1 );              // +1 for the NUL
    if ( buf == (char*)0 ) {
        close( fd );
        outWarn = CxString( "secret file " ) + path + ": out of memory";
        return -1;
    }
    unsigned int got = 0;
    while ( got < size ) {
        int n = read( fd, buf + got, size - got );
        if ( n < 0 ) {
            CxString e( strerror( errno ) );
            free( buf );
            close( fd );
            outWarn = CxString( "secret file " ) + path + ": " + e;
            return -1;
        }
        if ( n == 0 ) {
            break;                                        // file shrank; use what we got
        }
        got += (unsigned int) n;
    }
    close( fd );
    buf[ got ] = '\0';

    // Strip a leading comment banner: whole lines that are blank or whose first
    // non-whitespace char is '#', up to the start of the JSON payload.
    char *p = buf;
    for ( ;; ) {
        char *lineStart = p;
        while ( *p == ' ' || *p == '\t' ) {
            p++;
        }
        if ( *p == '#' ) {                                // comment line -> skip to EOL
            while ( *p != '\0' && *p != '\n' ) {
                p++;
            }
            if ( *p == '\n' ) {
                p++;
            }
            continue;
        }
        if ( *p == '\n' ) {                               // blank line -> skip
            p++;
            continue;
        }
        if ( *p == '\0' ) {                               // nothing but banner
            p = lineStart;
            break;
        }
        break;                                             // first JSON line
    }

    CxString json( p );
    free( buf );

    CxJSONBase *root = CxJSONFactory::parse( json );
    if ( root == (CxJSONBase*)0 || root->type() != CxJSONBase::OBJECT ) {
        if ( root != (CxJSONBase*)0 ) {
            delete root;
        }
        outWarn = CxString( "secret file " ) + path + ": malformed JSON";
        return -1;
    }
    CxJSONObject *obj = (CxJSONObject*) root;

    CxJSONMember *am = obj->find( "allow_open" );
    if ( am != (CxJSONMember*)0 && am->object() != (CxJSONBase*)0
         && am->object()->type() == CxJSONBase::BOOLEAN ) {
        outAllowOpen = ( (CxJSONBoolean*) am->object() )->get();
    }

    CxJSONMember *sm = obj->find( "secret" );
    if ( sm != (CxJSONMember*)0 && sm->object() != (CxJSONBase*)0
         && sm->object()->type() == CxJSONBase::STRING ) {
        outSecret = ( (CxJSONString*) sm->object() )->get();   // copies; safe after delete
    }

    delete root;
    return 0;
}


//-------------------------------------------------------------------------
// main
//-------------------------------------------------------------------------
int
main( int argc, char** argv )
{
    g_heliosStartTime = time( (time_t*)0 );

    // Options. Defaults keep the dev workflow (foreground, log to stderr); the
    // Solaris init script passes -d -l <logfile> -P <pidfile>.
    int         port        = HELIOS_DEFAULT_PORT;
    int         doDaemonize = 0;
    const char *logPath     = (const char*)0;
    const char *pidPath     = (const char*)0;
    const char *secret      = (const char*)0;
    const char *secretFile  = (const char*)0;   // -S / HELIOS_SECRET_FILE (JSON)
    int         allowOpen   = 0;                 // -O / "allow_open":true

    int c;
    while ( ( c = getopt( argc, argv, "dp:l:P:s:S:O" ) ) != -1 ) {
        switch ( c ) {
            case 'd': doDaemonize = 1;      break;
            case 'p': port    = atoi( optarg ); break;
            case 'l': logPath = optarg;     break;
            case 'P': pidPath = optarg;     break;
            case 's': secret     = optarg;  break;
            case 'S': secretFile = optarg;  break;
            case 'O': allowOpen  = 1;       break;
            default:
                fprintf( stderr,
                    "usage: %s [-d] [-p port] [-l logfile] [-P pidfile] [-s secret] [-S file] [-O] [port]\n"
                    "  -d            daemonize (detach; for init). default: foreground\n"
                    "  -p port       listen port (default %d)\n"
                    "  -l logfile    append log here (default: stderr)\n"
                    "  -P pidfile    write pid here (for init stop/restart)\n"
                    "  -s secret     shared secret required in each request's \"auth\"\n"
                    "  -S file       read the secret from a JSON file { \"secret\": \"...\" }\n"
                    "                (default: HELIOS_SECRET env, then " HELIOS_DEFAULT_SECRET_FILE ")\n"
                    "  -O            run OPEN (no auth) -- dev only. Default: require a\n"
                    "                secret, denying every request until one is set\n",
                    argv[0], HELIOS_DEFAULT_PORT );
                return 1;
        }
    }
    // Shared-secret auth, require-always (fail-closed). Secret precedence: -s,
    // then HELIOS_SECRET env, then -S / HELIOS_SECRET_FILE, then the default
    // file /etc/helios/helios.json. On the QEMU guests the init script passes -s
    // from OBP (eeprom helios-secret, set per-boot by macXserver); physical
    // servers use the file. With NO secret from any source the daemon still runs
    // but DENIES every request -- it serves unauthenticated only when -O /
    // "allow_open":true is set explicitly (dev). A present-but-broken secret
    // file (bad perms / bad JSON) also lands in deny-all, never open. The posture
    // is applied (heliosSetSecret/Open) further down, once the logfile is open,
    // so any warning lands in the log rather than a daemon's /dev/null'd stderr.
    // See PROTOCOL.md.
    CxString    fileSecret;                       // backs a file-sourced secret;
                                                  // must outlive heliosSetSecret
    CxString    cfgWarn;                          // deferred config warning
    const char *secretSource = "none";

    if ( secret != (const char*)0 && secret[0] != '\0' ) {
        secretSource = "-s flag";
    } else {
        secret = (const char*)0;
        const char *env = getenv( "HELIOS_SECRET" );
        if ( env != (const char*)0 && env[0] != '\0' ) {
            secret = env;
            secretSource = "HELIOS_SECRET env";
        } else {
            const char *path         = secretFile;
            int         explicitPath = 0;
            if ( path == (const char*)0 ) {
                path = getenv( "HELIOS_SECRET_FILE" );
            }
            if ( path != (const char*)0 ) {
                explicitPath = 1;
            } else {
                path = HELIOS_DEFAULT_SECRET_FILE;
            }

            struct stat cst;
            if ( stat( path, &cst ) == 0 ) {
                int fileAllowOpen = 0;
                if ( heliosLoadSecretFile( path, fileSecret, fileAllowOpen, cfgWarn ) == 0 ) {
                    if ( fileSecret.length() > 0 ) {
                        secret       = fileSecret.data();
                        secretSource = path;
                    }
                    if ( fileAllowOpen ) {
                        allowOpen = 1;
                    }
                }
                // load error -> cfgWarn set, secret stays null -> deny-all
            } else if ( explicitPath ) {
                cfgWarn = CxString( "secret file " ) + path + ": "
                        + CxString( strerror( errno ) );
            }
            // default path merely absent: no warning, silent deny-all
        }
    }
    // Back-compat: a bare positional port (the old `heliosAgent <port>` form).
    if ( optind < argc ) {
        int p = atoi( argv[ optind ] );
        if ( p > 0 ) {
            port = p;
        }
    }
    if ( port <= 0 ) {
        fprintf( stderr, "heliosAgent: invalid port\n" );
        return 1;
    }

    // A client vanishing mid-write must not take the daemon down.
    signal( SIGPIPE, SIG_IGN );
    // Reap per-connection child processes so they don't zombie.
    signal( SIGCHLD, HELIOS_SIGHANDLER( reapChildren ) );
    // Clean stop on the signal an init K-script sends.
    signal( SIGTERM, HELIOS_SIGHANDLER( termHandler ) );

    CxSocket server( AF_INET, SOCK_STREAM, 0 );
    if ( ! server.good() ) {
        fprintf( stderr, "heliosAgent: could not create socket\n" );
        return 1;
    }

    // Allow an immediate restart while a prior instance's port is in TIME_WAIT.
    server.setReuseAddr( 1 );

    // Empty hostname binds INADDR_ANY, which is what we need inside the guest:
    // slirp forwards to the guest's address, not loopback. No auth yet --
    // localhost-only-via-hostfwd posture; see HELIOS_PLAN.md B7.
    CxInetAddress addr( port );
    addr.process();

    // Bind + listen BEFORE daemonizing, while stderr is still attached, so a
    // failure is visible to whoever started us (and yields a nonzero exit the
    // init script can see) rather than vanishing into a detached process. cx's
    // net layer reports bind/listen failure by THROWING CxSocketException, so
    // catch it and exit cleanly instead of aborting on an uncaught exception.
    try {
        server.bind( addr );
        server.listen( 5 );
    } catch ( ... ) {
        fprintf( stderr, "heliosAgent: bind/listen failed on port %d (in use?)\n", port );
        return 1;
    }

    // Detach now (the listening socket survives the forks). After this point
    // stderr is /dev/null, so all output must go through the logfile.
    if ( doDaemonize ) {
        if ( daemonize() != 0 ) {
            fprintf( stderr, "heliosAgent: daemonize failed\n" );
            return 1;
        }
    }

    // Open the logfile in the final process (CxLogFile keeps its own fd, so it
    // is unaffected by the /dev/null stdio redirect above). Append mode so
    // restarts don't wipe history.
    if ( logPath != (const char*)0 ) {
        if ( g_log.open( CxString( logPath ), CxString( "a" ) ) ) {
            g_logOpen = 1;
        } else {
            fprintf( stderr, "heliosAgent: could not open logfile %s\n", logPath );
        }
    }

    // Record our pid for the init stop/restart path.
    if ( pidPath != (const char*)0 ) {
        g_pidPath = pidPath;
        if ( writePidFile( pidPath ) != 0 ) {
            heliosLog( 1, "could not write pidfile %s", pidPath );
        }
    }

    // Apply the auth posture now that the logfile is open, so a deny-all/open
    // warning lands in the log rather than on a daemon's /dev/null'd stderr.
    // require-always: with no secret and no explicit -O/allow_open, every
    // request is denied (the daemon keeps running, but serves no one).
    heliosSetSecret( secret );
    heliosSetOpen( allowOpen );
    if ( cfgWarn.length() > 0 ) {
        heliosLog( 1, "%s", cfgWarn.data() );
    }
    if ( allowOpen ) {
        heliosLog( 1, "auth DISABLED: running OPEN (unauthenticated) -- "
                      "allow_open/-O set; dev only" );
    } else if ( secret == (const char*)0 ) {
        heliosLog( 1, "no secret configured (-s, HELIOS_SECRET, "
                      "-S/HELIOS_SECRET_FILE, %s): auth required, DENYING every "
                      "request until one is set", HELIOS_DEFAULT_SECRET_FILE );
    } else {
        heliosLog( 0, "shared-secret auth enabled (source: %s)", secretSource );
    }

    // One-time sysinfo setup (SunOS 4: kernel symbol lookup + a long-lived
    // /dev/kmem fd). AFTER daemonize, so the detach can't close the fd.
    // Never fatal: failure just means sysinfo omits the affected fields.
    sysInfoStartup();

    heliosLog( 0, "heliosAgent %s (protocol %d) listening on port %d, pid %ld%s",
               HELIOS_VERSION, HELIOS_PROTOCOL_VERSION, port, (long) getpid(),
               doDaemonize ? " (daemon)" : "" );

    // Concurrency model: fork per connection. The parent does nothing but
    // accept and fork, so a slow or long-running request on one connection
    // (a multi-minute `make`, a held-open agent session) never blocks another
    // client -- multiple Claude Code agents and macXserver's own control plane
    // can all be served at once. We use fork rather than threads because cx is
    // built for targets without pthreads (e.g. SunOS 4.x) and the verbs are
    // stateless (each acts on the local filesystem / exec), so there is no
    // shared in-memory state for separate processes to coordinate. A crashing
    // handler takes down only its own child. Children are reaped by
    // reapChildren (SIGCHLD).
    for ( ;; ) {
        try {
            CxSocket conn = server.accept();
            if ( ! conn.good() ) {
                continue;
            }

            pid_t pid = fork();

            if ( pid == 0 ) {
                // Child: stop accepting, serve this one connection, exit. We
                // _exit() so no destructors run -- handleConnection has already
                // closed the connection fd, and the OS closes the rest.
                server.close();
                // Restore default SIGCHLD: this child serves one connection and
                // never forks more connections, but run_command's CxProcess
                // forks and waitpid()s a command. The inherited reapChildren
                // handler would reap that command first and steal its exit
                // status, so hand SIGCHLD back to the default here.
                signal( SIGCHLD, SIG_DFL );
                handleConnection( conn );
                _exit( 0 );
            }

            if ( pid < 0 ) {
                // fork failed: serve inline rather than drop the client.
                handleConnection( conn );
            }

            // Parent: `conn` goes out of scope here and its (refcounted)
            // destructor closes the parent's copy of the connection fd. The
            // child keeps serving on its own copy.
        } catch ( ... ) {
            // accept() interrupted (e.g. SIGCHLD/EINTR) or errored; the daemon
            // must not die -- keep serving.
            continue;
        }
    }

    return 0;
}
