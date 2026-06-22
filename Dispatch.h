//-------------------------------------------------------------------------------------------------
//
//  Dispatch.h
//  heliosAgent
//
//  Turns one request line into one response line: parse the JSON, pull `verb`
//  and `id`, route to the verb handler, and return the response. All protocol
//  errors (bad JSON, missing verb, unknown verb) come back as well-formed
//  ok:false responses rather than dropping the connection. See PROTOCOL.md.
//
//-------------------------------------------------------------------------------------------------

#ifndef HELIOS_DISPATCH_H
#define HELIOS_DISPATCH_H

#include <cx/base/string.h>

class CxSocket;   // forward decl (the streaming verbs need the connection)

// Configure the shared secret required in each request's "auth" field. NULL or
// "" means no auth (every request allowed) -- the require-if-configured posture.
// Set once at startup before serving. See Dispatch.cpp / PROTOCOL.md.
void heliosSetSecret( const char *secret );

// requestLine is one JSON object (no trailing newline). Returns the response
// JSON object as a CxString (no trailing newline; the caller adds framing).
CxString heliosDispatch( CxString requestLine );

// Handle the streaming verbs (put_file/get_file), which need the socket for a
// raw body after the JSON header. Returns 0 if `requestLine` is NOT a streaming
// verb (caller should fall back to heliosDispatch); 1 if handled and the
// connection may continue; 2 if handled and the connection must close (framing
// unrecoverable). A socket error throws (handleConnection closes).
int heliosHandleStreaming( CxSocket &conn, CxString requestLine );

#endif
