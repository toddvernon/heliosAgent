//-------------------------------------------------------------------------------------------------
//
//  Verbs.cpp
//  heliosAgent
//
//  Protocol verb implementations. Responses are built with the cx JSON object
//  API (not sprintf) so all string values are correctly escaped on the wire
//  (the cx json emit-escaping fix, cx commit 75b8304). Building the response on
//  a stack root and deleting via scope frees the whole tree: ~CxJSONObject
//  deletes its members and ~CxJSONMember deletes its value, recursively.
//
//-------------------------------------------------------------------------------------------------

#include "Verbs.h"
#include "HeliosVersion.h"

#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_number.h>
#include <cx/json/json_boolean.h>

#include <unistd.h>
#include <time.h>

// Daemon start time, set in main() (HeliosAgent.cpp), read here for uptime.
extern time_t g_heliosStartTime;


//-------------------------------------------------------------------------
// verbHello
//
// { "id":<id>, "ok":true, "result":{ agent, version, protocol, host, uptime } }
//-------------------------------------------------------------------------
CxString
verbHello( double id )
{
    char hostbuf[256];
    if ( gethostname( hostbuf, sizeof(hostbuf) ) != 0 ) {
        hostbuf[0] = '\0';
    }
    hostbuf[ sizeof(hostbuf) - 1 ] = '\0';

    long uptime = (long)( time( (time_t*)0 ) - g_heliosStartTime );
    if ( uptime < 0 ) {
        uptime = 0;
    }

    CxJSONObject *result = new CxJSONObject();
    result->append( new CxJSONMember( "agent",    new CxJSONString( CxString( HELIOS_AGENT_NAME ) ) ) );
    result->append( new CxJSONMember( "version",  new CxJSONString( CxString( HELIOS_VERSION ) ) ) );
    result->append( new CxJSONMember( "protocol", new CxJSONNumber( (double) HELIOS_PROTOCOL_VERSION ) ) );
    result->append( new CxJSONMember( "host",     new CxJSONString( CxString( hostbuf ) ) ) );
    result->append( new CxJSONMember( "uptime",   new CxJSONNumber( (double) uptime ) ) );

    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",     new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",     new CxJSONBoolean( 1 ) ) );
    resp.append( new CxJSONMember( "result", result ) );

    return resp.toJsonString();
}
