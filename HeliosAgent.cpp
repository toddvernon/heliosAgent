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
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cx/base/string.h>
#include <cx/net/socket.h>
#include <cx/net/inaddr.h>
#include <cx/log/logfile.h>

#include "Dispatch.h"
#include "Verbs.h"
#include "HeliosVersion.h"

// Provisional default listen port. The Mac side reaches this through a slirp
// hostfwd (127.0.0.1:<hostport> -> guest:<this>). Port discipline and bind/auth
// posture are open items (HELIOS_PLAN.md B7); override with argv[1] for now.
#define HELIOS_DEFAULT_PORT 2125

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
    signal( SIGCHLD, reapChildren );
    errno = savedErrno;
    (void) signo;
}


//-------------------------------------------------------------------------
// performShutdown
//
// Run the real shutdown command. Called by the server AFTER the shutdown ACK
// has been sent, so the client always learns the guest is going down before it
// does. The command is overridable via HELIOS_SHUTDOWN_CMD -- essential for dev
// on the Mac, where the default `init 5` would shut down the developer's
// machine. In the shipped guest it is left unset and defaults to init 5.
//-------------------------------------------------------------------------
static void
performShutdown( void )
{
    const char *cmd = getenv( "HELIOS_SHUTDOWN_CMD" );
    if ( cmd == (const char*)0 || cmd[0] == '\0' ) {
        cmd = "/usr/sbin/init 5";
    }
    heliosLog( 0, "shutdown requested; running: %s", cmd );
    if ( g_pidPath != (const char*)0 ) {
        unlink( g_pidPath );
    }
    system( cmd );
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

    int c;
    while ( ( c = getopt( argc, argv, "dp:l:P:s:" ) ) != -1 ) {
        switch ( c ) {
            case 'd': doDaemonize = 1;      break;
            case 'p': port    = atoi( optarg ); break;
            case 'l': logPath = optarg;     break;
            case 'P': pidPath = optarg;     break;
            case 's': secret  = optarg;     break;
            default:
                fprintf( stderr,
                    "usage: %s [-d] [-p port] [-l logfile] [-P pidfile] [-s secret] [port]\n"
                    "  -d            daemonize (detach; for init). default: foreground\n"
                    "  -p port       listen port (default %d)\n"
                    "  -l logfile    append log here (default: stderr)\n"
                    "  -P pidfile    write pid here (for init stop/restart)\n"
                    "  -s secret     require this secret in each request's \"auth\"\n"
                    "                (default: HELIOS_SECRET env; absent = no auth)\n",
                    argv[0], HELIOS_DEFAULT_PORT );
                return 1;
        }
    }
    // Shared-secret auth: -s wins, else HELIOS_SECRET env, else none (open --
    // the require-if-configured posture, so dev/tests with neither run open).
    // The Solaris init script reads the secret from OBP (eeprom helios-secret),
    // which macXserver sets per-boot via qemu -prom-env. See PROTOCOL.md / DECISIONS.
    if ( secret == (const char*)0 ) {
        secret = getenv( "HELIOS_SECRET" );
    }
    heliosSetSecret( secret );
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
    signal( SIGCHLD, reapChildren );
    // Clean stop on the signal an init K-script sends.
    signal( SIGTERM, termHandler );

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
