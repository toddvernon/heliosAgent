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
#include <cx/b64/b64.h>
#include <cx/base/buffer.h>
#include <cx/base/slist.h>

#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

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
// b64 helpers -- encode raw bytes to one continuous base64 CxString, and
// decode a base64 CxString back to raw bytes. The cx encoder emits a line
// list (RFC1341-style); joining the lines with no separator yields one
// continuous base64 string, which is what we want on the wire. The decoder
// ignores line boundaries, so the whole string goes in as a single line.
// Raw file bytes only ever live in unsigned char* / CxBuffer, never in a
// CxString, so an embedded NUL can't truncate content.
//-------------------------------------------------------------------------
static CxString
b64Encode( const void *data, unsigned int len )
{
    CxB64Encoder encoder;
    CxSList< CxString > lines;
    encoder.process( (void*) data, len, lines );
    encoder.finalize( lines );

    CxString out;
    for ( unsigned int i = 0; i < lines.entries(); i++ ) {
        out = out + lines.at( i );
    }
    return out;
}

static CxBuffer
b64Decode( CxString b64 )
{
    CxB64Decoder decoder;
    CxSList< CxString > lines;
    lines.append( b64 );
    return decoder.process( lines );
}


//-------------------------------------------------------------------------
// verbReadFile
//
// { "id":<id>, "ok":true, "result":{ path, size, mode, encoding:"base64",
//                                     content } }
// Regular files only; a missing/non-regular path is ok:false.
//-------------------------------------------------------------------------
CxString
verbReadFile( double id, CxJSONObject *req )
{
    // path (required string)
    CxJSONMember *pm = req->find( "path" );
    if ( pm == (CxJSONMember*)0 || pm->object() == (CxJSONBase*)0
         || pm->object()->type() != CxJSONBase::STRING ) {
        return runError( id, CxString( "read_file: missing or non-string 'path'" ) );
    }
    CxString path = ( (CxJSONString*) pm->object() )->get();

    struct stat st;
    if ( stat( path.data(), &st ) != 0 ) {
        return runError( id, CxString( "read_file: " ) + CxString( strerror( errno ) ) + ": " + path );
    }
    if ( ! S_ISREG( st.st_mode ) ) {
        return runError( id, CxString( "read_file: not a regular file: " ) + path );
    }

    int fd = open( path.data(), O_RDONLY );
    if ( fd < 0 ) {
        return runError( id, CxString( "read_file: " ) + CxString( strerror( errno ) ) + ": " + path );
    }

    unsigned int size = (unsigned int) st.st_size;
    unsigned char *buf = (unsigned char*) malloc( size > 0 ? size : 1 );
    if ( buf == (unsigned char*)0 ) {
        close( fd );
        return runError( id, CxString( "read_file: out of memory: " ) + path );
    }

    unsigned int got = 0;
    while ( got < size ) {
        int n = read( fd, buf + got, size - got );
        if ( n < 0 ) {
            CxString err = CxString( strerror( errno ) );
            free( buf );
            close( fd );
            return runError( id, CxString( "read_file: " ) + err + ": " + path );
        }
        if ( n == 0 ) {
            break;   // file shrank under us; return what we got
        }
        got += (unsigned int) n;
    }
    close( fd );

    CxString content = b64Encode( buf, got );
    free( buf );

    CxJSONObject *result = new CxJSONObject();
    result->append( new CxJSONMember( "path",     new CxJSONString( path ) ) );
    result->append( new CxJSONMember( "size",     new CxJSONNumber( (double) got ) ) );
    result->append( new CxJSONMember( "mode",     new CxJSONNumber( (double) ( st.st_mode & 07777 ) ) ) );
    result->append( new CxJSONMember( "encoding", new CxJSONString( CxString( "base64" ) ) ) );
    result->append( new CxJSONMember( "content",  new CxJSONString( content ) ) );

    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",     new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",     new CxJSONBoolean( 1 ) ) );
    resp.append( new CxJSONMember( "result", result ) );

    return resp.toJsonString();
}


//-------------------------------------------------------------------------
// verbWriteFile
//
// Atomic: write a temp file in the target's directory, set perms/owner, then
// rename() over the target. See HELIOS_PLAN.md B5 for the permission policy.
//
// { "id":<id>, "ok":true, "result":{ path, bytes_written, mode, created } }
//-------------------------------------------------------------------------
CxString
verbWriteFile( double id, CxJSONObject *req )
{
    // path (required string)
    CxJSONMember *pm = req->find( "path" );
    if ( pm == (CxJSONMember*)0 || pm->object() == (CxJSONBase*)0
         || pm->object()->type() != CxJSONBase::STRING ) {
        return runError( id, CxString( "write_file: missing or non-string 'path'" ) );
    }
    CxString path = ( (CxJSONString*) pm->object() )->get();

    // content (required base64 string)
    CxJSONMember *cm = req->find( "content" );
    if ( cm == (CxJSONMember*)0 || cm->object() == (CxJSONBase*)0
         || cm->object()->type() != CxJSONBase::STRING ) {
        return runError( id, CxString( "write_file: missing or non-string 'content'" ) );
    }
    CxString content = ( (CxJSONString*) cm->object() )->get();

    // mode (optional number; low 12 perm bits)
    int haveMode = 0;
    long explicitMode = 0;
    CxJSONMember *mm = req->find( "mode" );
    if ( mm != (CxJSONMember*)0 && mm->object() != (CxJSONBase*)0
         && mm->object()->type() == CxJSONBase::NUMBER ) {
        explicitMode = (long) ( (CxJSONNumber*) mm->object() )->get();
        haveMode = 1;
    }

    // Inspect the target: capture perms/owner for preservation, reject a
    // non-regular target rather than clobbering a dir/device.
    struct stat st;
    int existed = ( stat( path.data(), &st ) == 0 );
    if ( existed && ! S_ISREG( st.st_mode ) ) {
        return runError( id, CxString( "write_file: not a regular file: " ) + path );
    }

    CxBuffer data = b64Decode( content );

    // Temp file in the SAME directory so rename() is an atomic same-fs swap.
    // pid keeps concurrent fork-per-connection writers from colliding.
    CxString tmp = path + CxString( ".helios-tmp-" ) + CxString( (int) getpid() );

    int fd = open( tmp.data(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600 );
    if ( fd < 0 ) {
        return runError( id, CxString( "write_file: create temp: " ) + CxString( strerror( errno ) ) + ": " + tmp );
    }

    unsigned int len = data.length();
    unsigned char *p = (unsigned char*) data.data();
    unsigned int written = 0;
    while ( written < len ) {
        int n = write( fd, p + written, len - written );
        if ( n < 0 ) {
            CxString err = CxString( strerror( errno ) );
            close( fd );
            unlink( tmp.data() );
            return runError( id, CxString( "write_file: " ) + err + ": " + path );
        }
        written += (unsigned int) n;
    }
    fsync( fd );

    // Decide the final mode: explicit param wins; else preserve on overwrite;
    // else a sane default for a brand-new file.
    mode_t finalMode;
    if ( haveMode ) {
        finalMode = (mode_t) ( explicitMode & 07777 );
    } else if ( existed ) {
        finalMode = (mode_t) ( st.st_mode & 07777 );
    } else {
        finalMode = (mode_t) 0644;
    }

    // chown BEFORE chmod: chown can clear setuid/setgid, so set the mode last.
    // Owner preservation only works as root; otherwise we already own the file.
    if ( existed && geteuid() == 0 ) {
        fchown( fd, st.st_uid, st.st_gid );
    }
    fchmod( fd, finalMode );

    close( fd );

    if ( rename( tmp.data(), path.data() ) != 0 ) {
        CxString err = CxString( strerror( errno ) );
        unlink( tmp.data() );
        return runError( id, CxString( "write_file: rename: " ) + err + ": " + path );
    }

    CxJSONObject *result = new CxJSONObject();
    result->append( new CxJSONMember( "path",          new CxJSONString( path ) ) );
    result->append( new CxJSONMember( "bytes_written", new CxJSONNumber( (double) written ) ) );
    result->append( new CxJSONMember( "mode",          new CxJSONNumber( (double) finalMode ) ) );
    result->append( new CxJSONMember( "created",        new CxJSONBoolean( existed ? 0 : 1 ) ) );

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
