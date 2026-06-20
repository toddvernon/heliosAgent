//-------------------------------------------------------------------------------------------------
//
//  Verbs.h
//  heliosAgent
//
//  One function per protocol verb. Each takes the request's echo id (and, as
//  verbs grow, the parsed request object) and returns a complete response line
//  as a CxString. See PROTOCOL.md for the verb set and Dispatch.cpp for routing.
//
//-------------------------------------------------------------------------------------------------

#ifndef HELIOS_VERBS_H
#define HELIOS_VERBS_H

#include <cx/base/string.h>

class CxJSONObject;   // the parsed request object (forward decl)

// Liveness handshake: agent name, version, protocol, hostname, daemon uptime.
CxString verbHello( double id );

// Run a shell command on the Sun. Reads "cmd" (required), "cwd" (optional), and
// "timeout_ms" (optional) from the request; returns
// { exit_code, output, timed_out }. ok:true means the command was *run* (a
// nonzero exit is reported in exit_code, not as a daemon error). Backed by
// CxProcess (fork/exec with cwd + timeout).
CxString verbRun( double id, CxJSONObject *req );

// Graceful shutdown. Returns an ACK ({status:"shutting down"}) and records the
// intent; it does NOT exec anything itself, so dispatching it (e.g. from a unit
// test) is side-effect-free. The server performs the real shutdown only after
// the ACK has been sent -- see heliosShutdownRequested() + HeliosAgent.cpp.
CxString verbShutdown( double id );

// True once verbShutdown has been dispatched on this (forked) connection. The
// server checks this after sending the response and then execs the shutdown
// command. Kept out of the verb so tests never trigger a real shutdown.
int heliosShutdownRequested( void );

#endif
