//-----------------------------------------------------------------------------------------
// helios_test.cpp - unit tests for heliosAgent dispatch + verbs
//
// Drives heliosDispatch() directly (request line in, response line out, no
// socket) and asserts on the parsed JSON response. Same simple check()/counter
// idiom as the cx_tests suites. Build/run with `make test` from the app dir, or
// `make && ./<objdir>/helios_test` here.
//-----------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <cx/base/string.h>
#include <cx/json/json_factory.h>
#include <cx/json/json_base.h>
#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_number.h>
#include <cx/json/json_boolean.h>

#include "Dispatch.h"

// Verbs.cpp expects this symbol (normally defined in HeliosAgent.cpp's main
// module, which we don't link into the test). Provide it here.
time_t g_heliosStartTime = 0;


//-----------------------------------------------------------------------------------------
// harness
//-----------------------------------------------------------------------------------------
static int testsPassed = 0;
static int testsFailed = 0;

static void
check( int condition, const char* testName )
{
    if ( condition ) {
        testsPassed++;
        printf( "  PASS: %s\n", testName );
    } else {
        testsFailed++;
        printf( "  FAIL: %s\n", testName );
    }
}


//-----------------------------------------------------------------------------------------
// small response accessors (file scope -- no local classes per gcc 2.95)
//-----------------------------------------------------------------------------------------
static CxJSONObject *
parseObject( CxString line )
{
    CxJSONBase *b = CxJSONFactory::parse( line );
    if ( b == (CxJSONBase*)0 ) {
        return (CxJSONObject*)0;
    }
    if ( b->type() != CxJSONBase::OBJECT ) {
        delete b;
        return (CxJSONObject*)0;
    }
    return (CxJSONObject*) b;
}

static int
getBool( CxJSONObject *o, const char *key, int dflt )
{
    CxJSONMember *m = o->find( key );
    if ( m == (CxJSONMember*)0 || m->object() == (CxJSONBase*)0
         || m->object()->type() != CxJSONBase::BOOLEAN ) {
        return dflt;
    }
    return ( (CxJSONBoolean*) m->object() )->get();
}

static double
getNumber( CxJSONObject *o, const char *key, double dflt )
{
    CxJSONMember *m = o->find( key );
    if ( m == (CxJSONMember*)0 || m->object() == (CxJSONBase*)0
         || m->object()->type() != CxJSONBase::NUMBER ) {
        return dflt;
    }
    return ( (CxJSONNumber*) m->object() )->get();
}

static CxString
getString( CxJSONObject *o, const char *key )
{
    CxJSONMember *m = o->find( key );
    if ( m == (CxJSONMember*)0 || m->object() == (CxJSONBase*)0
         || m->object()->type() != CxJSONBase::STRING ) {
        return CxString( "" );
    }
    return ( (CxJSONString*) m->object() )->get();
}

static CxJSONObject *
getObject( CxJSONObject *o, const char *key )
{
    CxJSONMember *m = o->find( key );
    if ( m == (CxJSONMember*)0 || m->object() == (CxJSONBase*)0
         || m->object()->type() != CxJSONBase::OBJECT ) {
        return (CxJSONObject*)0;
    }
    return (CxJSONObject*) m->object();
}

static int
contains( CxString haystack, const char *needle )
{
    return haystack.index( CxString( needle ) ) != -1;
}


//-----------------------------------------------------------------------------------------
// tests
//-----------------------------------------------------------------------------------------
static void
testHello( void )
{
    printf( "\n== hello ==\n" );

    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"hello\", \"id\": 1 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "hello response is a JSON object" );
    if ( o == (CxJSONObject*)0 ) return;

    check( getBool( o, "ok", 0 ) == 1, "hello ok==true" );
    check( getNumber( o, "id", -1 ) == 1, "hello echoes id" );

    CxJSONObject *result = getObject( o, "result" );
    check( result != (CxJSONObject*)0, "hello has result object" );
    if ( result != (CxJSONObject*)0 ) {
        check( getString( result, "agent" ) == CxString( "heliosAgent" ), "result.agent==heliosAgent" );
        check( getNumber( result, "protocol", -1 ) == 1, "result.protocol==1" );
        check( getString( result, "version" ).length() > 0, "result.version present" );
        check( getString( result, "host" ).length() > 0, "result.host present" );
        check( getNumber( result, "uptime", -1 ) >= 0, "result.uptime >= 0" );
    }
    delete o;
}

static void
testUnknownVerb( void )
{
    printf( "\n== unknown verb ==\n" );
    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"frobnicate\", \"id\": 2 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "unknown-verb response parses" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 1 ) == 0, "unknown verb ok==false" );
    check( getNumber( o, "id", -1 ) == 2, "unknown verb echoes id" );
    check( contains( getString( o, "error" ), "unknown verb" ), "error names unknown verb" );
    delete o;
}

static void
testPlannedVerb( void )
{
    printf( "\n== planned-but-unimplemented verb ==\n" );
    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 3 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "planned-verb response parses" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 1 ) == 0, "planned verb ok==false" );
    check( contains( getString( o, "error" ), "not implemented" ), "error says not implemented" );
    delete o;
}

static void
testMissingVerb( void )
{
    printf( "\n== missing verb field ==\n" );
    CxString resp = heliosDispatch( CxString( "{ \"id\": 4 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "missing-verb response parses" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 1 ) == 0, "missing verb ok==false" );
    check( getNumber( o, "id", -1 ) == 4, "missing verb still echoes id" );
    check( contains( getString( o, "error" ), "verb" ), "error mentions verb" );
    delete o;
}

static void
testBadJson( void )
{
    printf( "\n== malformed JSON (parser prints its own stderr diag) ==\n" );
    CxString resp = heliosDispatch( CxString( "not json at all" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "bad-json response is still valid JSON" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 1 ) == 0, "bad json ok==false" );
    check( contains( getString( o, "error" ), "parse error" ), "error says parse error" );
    delete o;
}

static void
testDefaultId( void )
{
    printf( "\n== id defaults to 0 ==\n" );
    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"hello\" }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "no-id response parses" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 0 ) == 1, "no-id hello still ok" );
    check( getNumber( o, "id", -1 ) == 0, "missing id defaults to 0" );
    delete o;
}

static void
testResponseEscaping( void )
{
    printf( "\n== response escaping (cx json emit fix, commit 75b8304) ==\n" );
    // A verb value containing a double-quote and backslash must come back as a
    // VALID JSON response (the error echoes the verb). If emit-escaping were
    // broken (the old cx bug), this response would be malformed and parseObject
    // would return NULL.
    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"a\\\"b\\\\c\", \"id\": 7 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "response with embedded quote/backslash is valid JSON" );
    if ( o == (CxJSONObject*)0 ) return;
    check( getBool( o, "ok", 1 ) == 0, "escaped-verb ok==false" );
    check( contains( getString( o, "error" ), "a\"b\\c" ), "error round-trips the escaped verb" );
    delete o;
}


//-----------------------------------------------------------------------------------------
// main
//-----------------------------------------------------------------------------------------
int
main( int argc, char **argv )
{
    (void) argc;
    (void) argv;

    g_heliosStartTime = time( (time_t*)0 );

    printf( "heliosAgent Test Suite\n" );
    printf( "======================\n" );

    testHello();
    testUnknownVerb();
    testPlannedVerb();
    testMissingVerb();
    testBadJson();
    testDefaultId();
    testResponseEscaping();

    printf( "\n======================\n" );
    printf( "Results: %d passed, %d failed\n", testsPassed, testsFailed );

    return testsFailed > 0 ? 1 : 0;
}
