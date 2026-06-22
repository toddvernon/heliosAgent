//-------------------------------------------------------------------------------------------------
//
//  Dispatch.cpp
//  heliosAgent
//
//  Parse one request line, route by verb, return one response line.
//
//-------------------------------------------------------------------------------------------------

#include "Dispatch.h"
#include "Verbs.h"

#include <cx/json/json_factory.h>
#include <cx/json/json_base.h>
#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_number.h>
#include <cx/json/json_boolean.h>
#include <cx/net/socket.h>


//-------------------------------------------------------------------------
// errorResponse  -- build an ok:false response line
//-------------------------------------------------------------------------
static CxString
errorResponse( double id, CxString message )
{
    CxJSONObject resp;
    resp.append( new CxJSONMember( "id",    new CxJSONNumber( id ) ) );
    resp.append( new CxJSONMember( "ok",    new CxJSONBoolean( 0 ) ) );
    resp.append( new CxJSONMember( "error", new CxJSONString( message ) ) );
    return resp.toJsonString();
}


//-------------------------------------------------------------------------
// Shared-secret auth (HELIOS_PLAN B7). The secret is set once at startup
// (-s / HELIOS_SECRET, sourced from OBP `eeprom helios-secret` by the init
// script). Require-if-configured: with no secret set, every request is
// allowed (dev/tests). With a secret set, each request must carry a matching
// "auth" string. Honest scope: a plaintext secret over the cleartext channel
// is a speed-bump (kills unauthenticated/cross-VM/port-scan access; solid on
// loopback), NOT crypto -- an HMAC/TLS upgrade is the LAN-case follow-up.
//-------------------------------------------------------------------------
static int      s_haveSecret = 0;
static CxString s_secret;

void
heliosSetSecret( const char *secret )
{
    if ( secret != (const char*)0 && secret[0] != '\0' ) {
        s_secret = CxString( secret );
        s_haveSecret = 1;
    } else {
        s_haveSecret = 0;
    }
}

// Equality with no early-out on content (length may leak; the secret's length
// is not sensitive). Avoids a timing oracle on the secret's bytes.
static int
secretEquals( const char *got, int gotLen )
{
    int want = s_secret.length();
    if ( gotLen != want ) {
        return 0;
    }
    const char *w = s_secret.data();
    unsigned char r = 0;
    for ( int i = 0; i < want; i++ ) {
        r |= (unsigned char) ( got[ i ] ^ w[ i ] );
    }
    return r == 0;
}

// True if no secret is configured, or the request carries the matching "auth".
static int
authOk( CxJSONObject *req )
{
    if ( ! s_haveSecret ) {
        return 1;
    }
    CxJSONMember *am = req->find( "auth" );
    if ( am == (CxJSONMember*)0 || am->object() == (CxJSONBase*)0
         || am->object()->type() != CxJSONBase::STRING ) {
        return 0;
    }
    CxString got = ( (CxJSONString*) am->object() )->get();
    return secretEquals( got.data(), got.length() );
}


//-------------------------------------------------------------------------
// heliosDispatch
//-------------------------------------------------------------------------
CxString
heliosDispatch( CxString requestLine )
{
    CxJSONBase *root = CxJSONFactory::parse( requestLine );

    if ( root == (CxJSONBase*)0 ) {
        return errorResponse( 0, CxString( "parse error: invalid JSON" ) );
    }

    if ( root->type() != CxJSONBase::OBJECT ) {
        delete root;
        return errorResponse( 0, CxString( "protocol error: request must be a JSON object" ) );
    }

    CxJSONObject *req = (CxJSONObject*) root;

    // id (optional, echoed back)
    double id = 0;
    CxJSONMember *idm = req->find( "id" );
    if ( idm != (CxJSONMember*)0
         && idm->object() != (CxJSONBase*)0
         && idm->object()->type() == CxJSONBase::NUMBER ) {
        id = ( (CxJSONNumber*) idm->object() )->get();
    }

    // verb (required)
    CxJSONMember *vm = req->find( "verb" );
    if ( vm == (CxJSONMember*)0
         || vm->object() == (CxJSONBase*)0
         || vm->object()->type() != CxJSONBase::STRING ) {
        delete root;
        return errorResponse( id, CxString( "protocol error: missing or non-string 'verb'" ) );
    }
    CxString verb = ( (CxJSONString*) vm->object() )->get();

    // Auth gate (no-op when no secret is configured).
    if ( ! authOk( req ) ) {
        delete root;
        return errorResponse( id, CxString( "unauthorized" ) );
    }

    CxString out;
    if ( verb == "hello" ) {
        out = verbHello( id );
    } else if ( verb == "shutdown" ) {
        out = verbShutdown( id );
    } else if ( verb == "run_command" ) {
        out = verbRun( id, req );
    } else if ( verb == "read_file" ) {
        out = verbReadFile( id, req );
    } else if ( verb == "write_file" ) {
        out = verbWriteFile( id, req );
    } else if ( verb == "stat" ) {
        out = verbStat( id, req );
    } else if ( verb == "list_dir" ) {
        out = verbListDir( id, req );
    } else if ( verb == "search" ) {
        out = verbSearch( id, req );
    } else {
        out = errorResponse( id, CxString( "unknown verb: " ) + verb );
    }

    delete root;
    return out;
}


//-------------------------------------------------------------------------
// heliosHandleStreaming
//
// Peek the verb; only put_file/get_file are streaming. Anything else (incl. a
// parse error or missing verb) returns 0 so the caller falls back to the normal
// line dispatch (which renders the proper error). The streaming verbs own their
// response + raw body via the socket.
//-------------------------------------------------------------------------
int
heliosHandleStreaming( CxSocket &conn, CxString requestLine )
{
    CxJSONBase *root = CxJSONFactory::parse( requestLine );
    if ( root == (CxJSONBase*)0 ) {
        return 0;
    }
    if ( root->type() != CxJSONBase::OBJECT ) {
        delete root;
        return 0;
    }
    CxJSONObject *req = (CxJSONObject*) root;

    CxJSONMember *vm = req->find( "verb" );
    if ( vm == (CxJSONMember*)0 || vm->object() == (CxJSONBase*)0
         || vm->object()->type() != CxJSONBase::STRING ) {
        delete root;
        return 0;
    }
    CxString verb = ( (CxJSONString*) vm->object() )->get();
    if ( verb != "put_file" && verb != "get_file" ) {
        delete root;
        return 0;
    }

    double id = 0;
    CxJSONMember *idm = req->find( "id" );
    if ( idm != (CxJSONMember*)0 && idm->object() != (CxJSONBase*)0
         && idm->object()->type() == CxJSONBase::NUMBER ) {
        id = ( (CxJSONNumber*) idm->object() )->get();
    }

    // Auth gate. On failure we close the connection: an unauthorized client's
    // declared body length can't be trusted to re-frame the stream.
    if ( ! authOk( req ) ) {
        CxString resp = errorResponse( id, CxString( "unauthorized" ) );
        resp.append( '\n' );
        conn.sendAtLeast( resp );
        delete root;
        return 2;
    }

    int rc;
    if ( verb == "put_file" ) {
        rc = verbPutFileStream( id, req, conn );
    } else {
        rc = verbGetFileStream( id, req, conn );
    }

    delete root;
    return rc;
}
