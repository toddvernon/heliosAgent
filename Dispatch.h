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

// requestLine is one JSON object (no trailing newline). Returns the response
// JSON object as a CxString (no trailing newline; the caller adds framing).
CxString heliosDispatch( CxString requestLine );

#endif
