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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cx/base/string.h>
#include <cx/net/socket.h>
#include <cx/net/inaddr.h>

#include "Dispatch.h"
#include "HeliosVersion.h"

// Provisional default listen port. The Mac side reaches this through a slirp
// hostfwd (127.0.0.1:<hostport> -> guest:<this>). Port discipline and bind/auth
// posture are open items (HELIOS_PLAN.md B7); override with argv[1] for now.
#define HELIOS_DEFAULT_PORT 2125

// Daemon start time, for the hello verb's uptime field. Read by Verbs.cpp.
time_t g_heliosStartTime = 0;


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

        CxString response = heliosDispatch( line );
        response.append( '\n' );

        try {
            conn.sendAtLeast( response );
        } catch ( ... ) {
            break;                               // peer went away mid-write
        }
    }

    conn.close();
}


//-------------------------------------------------------------------------
// main
//-------------------------------------------------------------------------
int
main( int argc, char** argv )
{
    g_heliosStartTime = time( (time_t*)0 );

    int port = HELIOS_DEFAULT_PORT;
    if ( argc > 1 ) {
        port = atoi( argv[1] );
        if ( port <= 0 ) {
            fprintf( stderr, "usage: %s [port]\n", argv[0] );
            return 1;
        }
    }

    // A client vanishing mid-write must not take the daemon down.
    signal( SIGPIPE, SIG_IGN );
    // Reap per-connection child processes so they don't zombie.
    signal( SIGCHLD, reapChildren );

    CxSocket server( AF_INET, SOCK_STREAM, 0 );
    if ( ! server.good() ) {
        fprintf( stderr, "heliosAgent: could not create socket\n" );
        return 1;
    }

    // Empty hostname binds INADDR_ANY, which is what we need inside the guest:
    // slirp forwards to the guest's address, not loopback. No auth yet --
    // localhost-only-via-hostfwd posture; see HELIOS_PLAN.md B7.
    CxInetAddress addr( port );
    addr.process();

    if ( server.bind( addr ) != 0 ) {
        fprintf( stderr, "heliosAgent: bind failed on port %d\n", port );
        return 1;
    }
    if ( server.listen( 5 ) != 0 ) {
        fprintf( stderr, "heliosAgent: listen failed on port %d\n", port );
        return 1;
    }

    fprintf( stderr, "heliosAgent %s (protocol %d) listening on port %d\n",
             HELIOS_VERSION, HELIOS_PROTOCOL_VERSION, port );

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
