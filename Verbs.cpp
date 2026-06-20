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

#include <cx/json/json_base.h>
#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_number.h>
#include <cx/json/json_boolean.h>
#include <cx/process/process.h>

#include <unistd.h>
#include <time.h>

// Daemon start time, set in main() (HeliosAgent.cpp), read here for uptime.
extern time_t g_heliosStartTime;

// Per-connection shutdown intent. verbShutdown sets it; the server reads it via
// heliosShutdownRequested() after sending the ACK, then execs the real command.
// A static (not a global) so the unit test, which links Verbs.cpp but never runs
// the server loop, can dispatch shutdown safely without anything executing.
static int s_shutdownRequested = 0;

int
heliosShutdownRequested( void )
{
    return s_shutdownRequested;
}


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


//-------------------------------------------------------------------------
// runError -- local ok:false response builder (Dispatch's is file-static).
//-------------------------------------------------------------------------
static CxString
runError( double id, CxString message )
{
    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",    new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",    new CxJSONBoolean( 0 ) ) );
    resp.append( new CxJSONMember( "error", new CxJSONString( message ) ) );
    return resp.toJsonString();
}


//-------------------------------------------------------------------------
// verbRun
//
// { "id":<id>, "ok":true, "result":{ "exit_code":N, "output":"...",
//                                    "timed_out":bool } }
// ok:true means the command ran; a nonzero exit is reported in exit_code,
// not as a daemon error. Missing/invalid "cmd" is the only ok:false case.
//-------------------------------------------------------------------------
CxString
verbRun( double id, CxJSONObject *req )
{
    // cmd (required string)
    CxJSONMember *cm = req->find( "cmd" );
    if ( cm == (CxJSONMember*)0 || cm->object() == (CxJSONBase*)0
         || cm->object()->type() != CxJSONBase::STRING ) {
        return runError( id, CxString( "run_command: missing or non-string 'cmd'" ) );
    }
    CxString cmd = ( (CxJSONString*) cm->object() )->get();

    // cwd (optional string; empty/absent -> inherit)
    CxString cwd;
    const char *cwdPtr = (const char*)0;
    CxJSONMember *cwm = req->find( "cwd" );
    if ( cwm != (CxJSONMember*)0 && cwm->object() != (CxJSONBase*)0
         && cwm->object()->type() == CxJSONBase::STRING ) {
        cwd = ( (CxJSONString*) cwm->object() )->get();
        if ( cwd.length() > 0 ) {
            cwdPtr = cwd.data();
        }
    }

    // timeout_ms (optional number; absent/<=0 -> no timeout)
    int timeout = 0;
    CxJSONMember *tm = req->find( "timeout_ms" );
    if ( tm != (CxJSONMember*)0 && tm->object() != (CxJSONBase*)0
         && tm->object()->type() == CxJSONBase::NUMBER ) {
        timeout = (int) ( (CxJSONNumber*) tm->object() )->get();
    }

    CxProcess proc;
    proc.run( cmd.data(), cwdPtr, timeout );

    CxJSONObject *result = new CxJSONObject();
    result->append( new CxJSONMember( "exit_code", new CxJSONNumber( (double) proc.getExitCode() ) ) );
    result->append( new CxJSONMember( "output",    new CxJSONString( proc.getOutput() ) ) );
    result->append( new CxJSONMember( "timed_out", new CxJSONBoolean( proc.wasTimedOut() ) ) );

    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",     new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",     new CxJSONBoolean( 1 ) ) );
    resp.append( new CxJSONMember( "result", result ) );

    return resp.toJsonString();
}


//-------------------------------------------------------------------------
// verbShutdown
//
// Record the intent and ACK. The actual `init 5` is run by the server after
// this response is on the wire (HeliosAgent.cpp), so a guest shutting itself
// down still tells the client first -- and dispatching this verb in isolation
// (a test) executes nothing.
//
// { "id":<id>, "ok":true, "result":{ "status":"shutting down" } }
//-------------------------------------------------------------------------
CxString
verbShutdown( double id )
{
    s_shutdownRequested = 1;

    CxJSONObject *result = new CxJSONObject();
    result->append( new CxJSONMember( "status", new CxJSONString( CxString( "shutting down" ) ) ) );

    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",     new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",     new CxJSONBoolean( 1 ) ) );
    resp.append( new CxJSONMember( "result", result ) );

    return resp.toJsonString();
}
