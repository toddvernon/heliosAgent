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

// Liveness handshake: agent name, version, protocol, hostname, daemon uptime.
CxString verbHello( double id );

#endif
