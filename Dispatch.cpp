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
