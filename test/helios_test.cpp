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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <cx/base/string.h>
#include <cx/base/buffer.h>
#include <cx/base/slist.h>
#include <cx/b64/b64.h>
#include <cx/json/json_factory.h>
#include <cx/json/json_base.h>
#include <cx/json/json_object.h>
#include <cx/json/json_member.h>
#include <cx/json/json_string.h>
#include <cx/json/json_number.h>
#include <cx/json/json_boolean.h>
#include <cx/json/json_array.h>

#include "Dispatch.h"
#include "Verbs.h"

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

static CxJSONArray *
getArray( CxJSONObject *o, const char *key )
{
    CxJSONMember *m = o->find( key );
    if ( m == (CxJSONMember*)0 || m->object() == (CxJSONBase*)0
         || m->object()->type() != CxJSONBase::ARRAY ) {
        return (CxJSONArray*)0;
    }
    return (CxJSONArray*) m->object();
}

// Does an array of objects contain one whose string member `key` equals `val`?
static int
arrayHasObjectWith( CxJSONArray *a, const char *key, const char *val )
{
    if ( a == (CxJSONArray*)0 ) return 0;
    for ( int i = 0; i < a->entries(); i++ ) {
        CxJSONBase *b = a->at( i );
        if ( b == (CxJSONBase*)0 || b->type() != CxJSONBase::OBJECT ) continue;
        if ( getString( (CxJSONObject*) b, key ) == CxString( val ) ) return 1;
    }
    return 0;
}

// Find the object in an array whose string member `key` equals `val` (or NULL).
static CxJSONObject *
arrayFindObjectWith( CxJSONArray *a, const char *key, const char *val )
{
    if ( a == (CxJSONArray*)0 ) return (CxJSONObject*)0;
    for ( int i = 0; i < a->entries(); i++ ) {
        CxJSONBase *b = a->at( i );
        if ( b == (CxJSONBase*)0 || b->type() != CxJSONBase::OBJECT ) continue;
        if ( getString( (CxJSONObject*) b, key ) == CxString( val ) ) return (CxJSONObject*) b;
    }
    return (CxJSONObject*)0;
}

static int
contains( CxString haystack, const char *needle )
{
    return haystack.index( CxString( needle ) ) != -1;
}

// Test-side base64, same idiom as the verb code, so we can build request
// content and verify response content independently of the daemon's helpers.
static CxString
b64EncodeT( const void *data, unsigned int len )
{
    CxB64Encoder enc;
    CxSList< CxString > lines;
    enc.process( (void*) data, len, lines );
    enc.finalize( lines );
    CxString out;
    for ( unsigned int i = 0; i < lines.entries(); i++ ) {
        out = out + lines.at( i );
    }
    return out;
}

static CxBuffer
b64DecodeT( CxString b64 )
{
    CxB64Decoder dec;
    CxSList< CxString > lines;
    lines.append( b64 );
    return dec.process( lines );
}

// Write raw bytes to a path with POSIX (the daemon-independent side of a
// round-trip test). Returns 1 on success.
static int
writeRaw( const char *path, const void *data, unsigned int len, mode_t mode )
{
    int fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    if ( fd < 0 ) return 0;
    unsigned int w = 0;
    const unsigned char *p = (const unsigned char*) data;
    while ( w < len ) {
        int n = write( fd, p + w, len - w );
        if ( n < 0 ) { close( fd ); return 0; }
        w += (unsigned int) n;
    }
    fchmod( fd, mode );
    close( fd );
    return 1;
}

// Read a whole file with POSIX into a CxBuffer. Empty buffer on failure.
static CxBuffer
readRaw( const char *path )
{
    struct stat st;
    if ( stat( path, &st ) != 0 ) return CxBuffer();
    int fd = open( path, O_RDONLY );
    if ( fd < 0 ) return CxBuffer();
    unsigned int size = (unsigned int) st.st_size;
    unsigned char *buf = (unsigned char*) malloc( size > 0 ? size : 1 );
    unsigned int got = 0;
    while ( got < size ) {
        int n = read( fd, buf + got, size - got );
        if ( n <= 0 ) break;
        got += (unsigned int) n;
    }
    close( fd );
    CxBuffer out( buf, got );
    free( buf );
    return out;
}

static int
modeOf( const char *path )
{
    struct stat st;
    if ( stat( path, &st ) != 0 ) return -1;
    return (int) ( st.st_mode & 07777 );
}

static int
buffersEqual( CxBuffer b, const void *data, unsigned int len )
{
    if ( b.length() != len ) return 0;
    if ( len == 0 ) return 1;
    return memcmp( b.data(), data, len ) == 0;
}

// Fixture-setup shell (test-only; not the daemon's exec path).
static void
shellRun( CxString cmd )
{
    system( cmd.data() );
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


static void
testRunCommand( void )
{
    printf( "\n== run_command ==\n" );

    // basic: echo, exit 0, output captured
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 8, \"cmd\": \"echo hello\" }" ) );
        CxJSONObject *o = parseObject( resp );
        check( o != (CxJSONObject*)0, "run_command response parses" );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "run_command ok==true" );
            check( getNumber( o, "id", -1 ) == 8, "run_command echoes id" );
            CxJSONObject *r = getObject( o, "result" );
            check( r != (CxJSONObject*)0, "run_command has result" );
            if ( r != (CxJSONObject*)0 ) {
                check( getNumber( r, "exit_code", -99 ) == 0, "echo: exit_code 0" );
                check( contains( getString( r, "output" ), "hello" ), "echo: output has hello" );
                check( getBool( r, "timed_out", 1 ) == 0, "echo: not timed out" );
            }
            delete o;
        }
    }

    // nonzero exit: command ran, so ok==true; exit_code reports the failure
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 9, \"cmd\": \"exit 4\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "nonzero exit still ok==true (command ran)" );
            CxJSONObject *r = getObject( o, "result" );
            check( r != (CxJSONObject*)0 && getNumber( r, "exit_code", -99 ) == 4, "nonzero: exit_code 4" );
            delete o;
        }
    }

    // missing cmd -> ok:false
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 10 }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 1 ) == 0, "missing cmd ok==false" );
            check( contains( getString( o, "error" ), "cmd" ), "missing cmd error mentions cmd" );
            delete o;
        }
    }

    // cwd honored
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 11, \"cmd\": \"/bin/pwd\", \"cwd\": \"/\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            CxJSONObject *r = getObject( o, "result" );
            check( r != (CxJSONObject*)0 && contains( getString( r, "output" ), "/" ), "cwd=/ output has /" );
            delete o;
        }
    }

    // timeout: ok==true (verb ran), result flags timed_out, exit_code -1
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 12, \"cmd\": \"sleep 5\", \"timeout_ms\": 300 }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "timeout: ok==true (verb ran)" );
            CxJSONObject *r = getObject( o, "result" );
            check( r != (CxJSONObject*)0 && getBool( r, "timed_out", 0 ) == 1, "timeout: timed_out true" );
            check( r != (CxJSONObject*)0 && getNumber( r, "exit_code", 0 ) == -1, "timeout: exit_code -1" );
            delete o;
        }
    }

    // run_as_user: an unknown user is rejected up front (ok:false), so the
    // client gets a clean error rather than an opaque exit 127 from the failed
    // in-child privilege drop. (A *successful* drop needs the daemon to be root
    // and is validated on the guest, not here.)
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 13, \"cmd\": \"id\", \"user\": \"no_such_user_zzz\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 1 ) == 0, "unknown user ok==false" );
            check( contains( getString( o, "error" ), "unknown user" ), "unknown user error mentions it" );
            delete o;
        }
    }

    // run_as_user: an empty user string is treated as absent (no drop) -- the
    // command still runs as the daemon, ok==true.
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\": \"run_command\", \"id\": 14, \"cmd\": \"echo hi\", \"user\": \"\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "empty user ok==true (no drop)" );
            delete o;
        }
    }
}


static void
testShutdown( void )
{
    printf( "\n== shutdown (ACK + intent flag, no real exec) ==\n" );

    // Nothing should have requested shutdown before this test runs.
    check( heliosShutdownRequested() == 0, "shutdown not requested initially" );

    CxString resp = heliosDispatch( CxString( "{ \"verb\": \"shutdown\", \"id\": 5 }" ) );
    CxJSONObject *o = parseObject( resp );
    check( o != (CxJSONObject*)0, "shutdown response parses" );
    if ( o == (CxJSONObject*)0 ) return;

    check( getBool( o, "ok", 0 ) == 1, "shutdown ok==true" );
    check( getNumber( o, "id", -1 ) == 5, "shutdown echoes id" );
    CxJSONObject *result = getObject( o, "result" );
    check( result != (CxJSONObject*)0, "shutdown has result object" );
    if ( result != (CxJSONObject*)0 ) {
        check( contains( getString( result, "status" ), "shutting down" ), "result.status says shutting down" );
    }

    // Dispatching the verb records intent but execs NOTHING (the server, not the
    // verb, runs the real command -- and only this test, not the server, ran).
    check( heliosShutdownRequested() == 1, "shutdown intent recorded after dispatch" );

    delete o;
}


static void
testReadFile( void )
{
    printf( "\n== read_file ==\n" );

    const char *path = "/tmp/helios_test_read.bin";
    unsigned char payload[] = { 'H', 'e', 'l', 'l', 'o', 0x00, 0xFF, 0x41, '\n' };
    unsigned int plen = (unsigned int) sizeof( payload );

    check( writeRaw( path, payload, plen, 0644 ), "read_file: fixture created" );

    CxString req = CxString( "{ \"verb\":\"read_file\", \"id\":20, \"path\":\"" )
                 + CxString( path ) + CxString( "\" }" );
    CxJSONObject *o = parseObject( heliosDispatch( req ) );
    check( o != (CxJSONObject*)0, "read_file response parses" );
    if ( o != (CxJSONObject*)0 ) {
        check( getBool( o, "ok", 0 ) == 1, "read_file ok==true" );
        check( getNumber( o, "id", -1 ) == 20, "read_file echoes id" );
        CxJSONObject *r = getObject( o, "result" );
        check( r != (CxJSONObject*)0, "read_file has result" );
        if ( r != (CxJSONObject*)0 ) {
            check( getNumber( r, "size", -1 ) == (double) plen, "read_file: size matches" );
            check( getString( r, "encoding" ) == CxString( "base64" ), "read_file: encoding base64" );
            check( getNumber( r, "mode", -1 ) == 0644, "read_file: mode reported" );
            CxBuffer got = b64DecodeT( getString( r, "content" ) );
            check( buffersEqual( got, payload, plen ), "read_file: content byte-exact (NUL + high byte)" );
        }
        delete o;
    }
    unlink( path );

    // nonexistent path
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"read_file\", \"id\":21, \"path\":\"/tmp/helios_nope_xyz\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "read_file missing file ok==false" );
            delete e;
        }
    }

    // directory is not a regular file
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"read_file\", \"id\":22, \"path\":\"/tmp\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "read_file on dir ok==false" );
            check( contains( getString( e, "error" ), "regular file" ), "read_file dir error says regular file" );
            delete e;
        }
    }

    // missing path field
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"read_file\", \"id\":23 }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "read_file missing path ok==false" );
            delete e;
        }
    }
}


static void
testWriteFile( void )
{
    printf( "\n== write_file ==\n" );

    const char *path = "/tmp/helios_test_write.bin";
    unlink( path );

    unsigned char payload[] = { 'W', 'r', 0x00, 0xFE, 'i', 't', 'e', '\n' };
    unsigned int plen = (unsigned int) sizeof( payload );
    CxString b64 = b64EncodeT( payload, plen );

    // new file with explicit mode 0640
    CxString req = CxString( "{ \"verb\":\"write_file\", \"id\":30, \"path\":\"" ) + CxString( path )
                 + CxString( "\", \"content\":\"" ) + b64
                 + CxString( "\", \"mode\":" ) + CxString( (int) 0640 ) + CxString( " }" );
    CxJSONObject *o = parseObject( heliosDispatch( req ) );
    check( o != (CxJSONObject*)0, "write_file response parses" );
    if ( o != (CxJSONObject*)0 ) {
        check( getBool( o, "ok", 0 ) == 1, "write_file ok==true" );
        CxJSONObject *r = getObject( o, "result" );
        check( r != (CxJSONObject*)0, "write_file has result" );
        if ( r != (CxJSONObject*)0 ) {
            check( getBool( r, "created", 0 ) == 1, "write_file: created true for new file" );
            check( getNumber( r, "bytes_written", -1 ) == (double) plen, "write_file: bytes_written matches" );
            check( getNumber( r, "mode", -1 ) == 0640, "write_file: mode reported 0640" );
        }
        delete o;
    }
    check( buffersEqual( readRaw( path ), payload, plen ), "write_file: on-disk content byte-exact" );
    check( modeOf( path ) == 0640, "write_file: on-disk mode 0640" );

    // overwrite WITHOUT mode: content changes, perms preserved
    unsigned char payload2[] = { 'v', '2', 0x00, '!' };
    unsigned int plen2 = (unsigned int) sizeof( payload2 );
    CxString req2 = CxString( "{ \"verb\":\"write_file\", \"id\":31, \"path\":\"" ) + CxString( path )
                  + CxString( "\", \"content\":\"" ) + b64EncodeT( payload2, plen2 ) + CxString( "\" }" );
    CxJSONObject *o2 = parseObject( heliosDispatch( req2 ) );
    if ( o2 != (CxJSONObject*)0 ) {
        CxJSONObject *r2 = getObject( o2, "result" );
        check( r2 != (CxJSONObject*)0 && getBool( r2, "created", 1 ) == 0, "write_file: created false on overwrite" );
        delete o2;
    }
    check( buffersEqual( readRaw( path ), payload2, plen2 ), "write_file: overwrite content byte-exact" );
    check( modeOf( path ) == 0640, "write_file: mode PRESERVED on overwrite (still 0640)" );
    unlink( path );

    // new file, no mode: default 0644
    const char *path2 = "/tmp/helios_test_write_default.bin";
    unlink( path2 );
    CxString req3 = CxString( "{ \"verb\":\"write_file\", \"id\":32, \"path\":\"" ) + CxString( path2 )
                  + CxString( "\", \"content\":\"" ) + b64EncodeT( "hi", 2 ) + CxString( "\" }" );
    heliosDispatch( req3 );
    check( modeOf( path2 ) == 0644, "write_file: new-file default mode 0644" );
    unlink( path2 );

    // write -> read round-trip is byte-exact (all-bytes payload)
    {
        const char *rt = "/tmp/helios_test_roundtrip.bin";
        unlink( rt );
        unsigned char rp[] = { 0x00, 0x01, 0x02, 0xFD, 0xFE, 0xFF, 'A', 'B' };
        unsigned int rl = (unsigned int) sizeof( rp );
        CxString wreq = CxString( "{ \"verb\":\"write_file\", \"id\":33, \"path\":\"" ) + CxString( rt )
                      + CxString( "\", \"content\":\"" ) + b64EncodeT( rp, rl ) + CxString( "\" }" );
        heliosDispatch( wreq );
        CxString rreq = CxString( "{ \"verb\":\"read_file\", \"id\":34, \"path\":\"" ) + CxString( rt ) + CxString( "\" }" );
        CxJSONObject *ro = parseObject( heliosDispatch( rreq ) );
        if ( ro != (CxJSONObject*)0 ) {
            CxJSONObject *rr = getObject( ro, "result" );
            check( rr != (CxJSONObject*)0 && buffersEqual( b64DecodeT( getString( rr, "content" ) ), rp, rl ),
                   "write->read round-trip byte-exact" );
            delete ro;
        }
        unlink( rt );
    }

    // missing path
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"write_file\", \"id\":35, \"content\":\"aGk=\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "write_file missing path ok==false" );
            delete e;
        }
    }

    // missing content
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"write_file\", \"id\":36, \"path\":\"/tmp/helios_x\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "write_file missing content ok==false" );
            delete e;
        }
    }
}


static void
testStat( void )
{
    printf( "\n== stat ==\n" );

    const char *path = "/tmp/helios_test_stat.bin";
    unsigned char payload[] = { 'a', 'b', 'c', 'd', 'e' };
    unsigned int plen = (unsigned int) sizeof( payload );
    check( writeRaw( path, payload, plen, 0644 ), "stat: fixture created" );

    CxString req = CxString( "{ \"verb\":\"stat\", \"id\":40, \"path\":\"" ) + CxString( path ) + CxString( "\" }" );
    CxJSONObject *o = parseObject( heliosDispatch( req ) );
    check( o != (CxJSONObject*)0, "stat response parses" );
    if ( o != (CxJSONObject*)0 ) {
        check( getBool( o, "ok", 0 ) == 1, "stat ok==true" );
        CxJSONObject *r = getObject( o, "result" );
        check( r != (CxJSONObject*)0, "stat has result" );
        if ( r != (CxJSONObject*)0 ) {
            check( getString( r, "type" ) == CxString( "file" ), "stat: type file" );
            check( getNumber( r, "size", -1 ) == (double) plen, "stat: size matches" );
            check( getNumber( r, "mode", -1 ) == 0644, "stat: mode 0644" );
            check( getNumber( r, "mtime", -1 ) > 0, "stat: mtime present" );
            check( getNumber( r, "uid", -1 ) >= 0, "stat: uid present" );
        }
        delete o;
    }
    unlink( path );

    // a real directory reports type dir (NB: /tmp on macOS is itself a symlink,
    // so use a dir we make -- lstat is truthful about symlinks, see below)
    {
        const char *d = "/tmp/helios_test_statdir";
        rmdir( d );
        mkdir( d, 0755 );
        CxString rq = CxString( "{ \"verb\":\"stat\", \"id\":41, \"path\":\"" ) + CxString( d ) + CxString( "\" }" );
        CxJSONObject *e = parseObject( heliosDispatch( rq ) );
        if ( e != (CxJSONObject*)0 ) {
            CxJSONObject *r = getObject( e, "result" );
            check( r != (CxJSONObject*)0 && getString( r, "type" ) == CxString( "dir" ), "stat dir: type dir" );
            delete e;
        }
        rmdir( d );
    }

    // a symlink reports type symlink and its target (lstat, not stat)
    {
        const char *tgt = "/tmp/helios_test_stat_target";
        const char *lnk = "/tmp/helios_test_stat_link";
        writeRaw( tgt, "x", 1, 0644 );
        unlink( lnk );
        check( symlink( tgt, lnk ) == 0, "stat: symlink fixture created" );
        CxString rq = CxString( "{ \"verb\":\"stat\", \"id\":43, \"path\":\"" ) + CxString( lnk ) + CxString( "\" }" );
        CxJSONObject *e = parseObject( heliosDispatch( rq ) );
        if ( e != (CxJSONObject*)0 ) {
            CxJSONObject *r = getObject( e, "result" );
            check( r != (CxJSONObject*)0 && getString( r, "type" ) == CxString( "symlink" ), "stat: type symlink" );
            check( r != (CxJSONObject*)0 && getString( r, "target" ) == CxString( tgt ), "stat: symlink target reported" );
            delete e;
        }
        unlink( lnk );
        unlink( tgt );
    }

    // nonexistent -> ok:false
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"stat\", \"id\":42, \"path\":\"/tmp/helios_nope_stat\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "stat missing path ok==false" );
            delete e;
        }
    }
}


static void
testListDir( void )
{
    printf( "\n== list_dir ==\n" );

    const char *dirp = "/tmp/helios_test_lsdir";
    // clean any prior run, then build: two files + one subdir
    {
        CxString rm = CxString( "rm -rf " ) + CxString( dirp );
        shellRun( rm );
    }
    check( mkdir( dirp, 0755 ) == 0, "list_dir: fixture dir created" );
    writeRaw( "/tmp/helios_test_lsdir/alpha.txt", "a", 1, 0644 );
    writeRaw( "/tmp/helios_test_lsdir/beta.txt",  "bb", 2, 0644 );
    mkdir( "/tmp/helios_test_lsdir/sub", 0755 );

    CxString req = CxString( "{ \"verb\":\"list_dir\", \"id\":50, \"path\":\"" ) + CxString( dirp ) + CxString( "\" }" );
    CxJSONObject *o = parseObject( heliosDispatch( req ) );
    check( o != (CxJSONObject*)0, "list_dir response parses" );
    if ( o != (CxJSONObject*)0 ) {
        check( getBool( o, "ok", 0 ) == 1, "list_dir ok==true" );
        CxJSONObject *r = getObject( o, "result" );
        check( r != (CxJSONObject*)0, "list_dir has result" );
        if ( r != (CxJSONObject*)0 ) {
            check( getNumber( r, "count", -1 ) == 3, "list_dir: count 3 (excludes . and ..)" );
            CxJSONArray *ents = getArray( r, "entries" );
            check( ents != (CxJSONArray*)0 && ents->entries() == 3, "list_dir: 3 entries" );
            check( arrayHasObjectWith( ents, "name", "alpha.txt" ), "list_dir: has alpha.txt" );
            check( arrayHasObjectWith( ents, "name", "beta.txt" ), "list_dir: has beta.txt" );
            CxJSONObject *sub = arrayFindObjectWith( ents, "name", "sub" );
            check( sub != (CxJSONObject*)0 && getString( sub, "type" ) == CxString( "dir" ), "list_dir: sub is type dir" );
            CxJSONObject *al = arrayFindObjectWith( ents, "name", "alpha.txt" );
            check( al != (CxJSONObject*)0 && getString( al, "type" ) == CxString( "file" ), "list_dir: alpha.txt is type file" );
        }
        delete o;
    }

    // a non-directory -> ok:false
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"list_dir\", \"id\":51, \"path\":\"/tmp/helios_test_lsdir/alpha.txt\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "list_dir on a file ok==false" );
            delete e;
        }
    }

    // missing path -> ok:false
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"list_dir\", \"id\":52 }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "list_dir missing path ok==false" );
            delete e;
        }
    }

    shellRun( CxString( "rm -rf " ) + CxString( dirp ) );
}


// The optional "user" field on the file verbs (run-as for the file browser).
// Two layers, both deterministic:
//   - an unknown user is always rejected cleanly (ok:false, "unknown user");
//   - a *known* user exercises the privilege drop. The drop uses initgroups,
//     which requires root, so the outcome splits on who runs the suite:
//       * as root (the production daemon): the drop succeeds, so a list/read
//         as ourselves works;
//       * as a normal user (Mac dev): the drop FAILS CLOSED -- ok:false,
//         "cannot run as user" -- rather than silently operating as root.
//     The fail-closed branch is the safety property that matters; we assert
//     whichever branch applies so the test is green on either footing.
static void
testRunAsUser( void )
{
    printf( "\n== run-as user (file verbs) ==\n" );

    int amRoot = ( geteuid() == 0 );

    // who are we? getpwuid on the real uid -- a guaranteed-valid username.
    struct passwd *self = getpwuid( getuid() );
    CxString me = ( self != (struct passwd*)0 ) ? CxString( self->pw_name ) : CxString( "" );
    check( me.length() > 0, "run-as: resolved current username" );

    const char *dirp = "/tmp/helios_test_user";
    shellRun( CxString( "rm -rf " ) + CxString( dirp ) );
    check( mkdir( dirp, 0755 ) == 0, "run-as: fixture dir created" );
    writeRaw( "/tmp/helios_test_user/owned.txt", "hi", 2, 0644 );

    // unknown user -> ok:false, error names it. Deterministic (no drop attempted).
    {
        CxJSONObject *e = parseObject( heliosDispatch( CxString(
            "{ \"verb\":\"list_dir\", \"id\":60, \"path\":\"" ) + CxString( dirp )
            + CxString( "\", \"user\":\"no_such_user_xyz\" }" ) ) );
        check( e != (CxJSONObject*)0, "run-as: unknown-user response parses" );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "run-as: unknown user ok==false" );
            check( contains( getString( e, "error" ), "unknown user" ), "run-as: error says unknown user" );
            delete e;
        }
    }

    // known user -> drop is attempted. Root: succeeds. Non-root: fails closed.
    {
        CxJSONObject *o = parseObject( heliosDispatch( CxString(
            "{ \"verb\":\"list_dir\", \"id\":61, \"path\":\"" ) + CxString( dirp )
            + CxString( "\", \"user\":\"" ) + me + CxString( "\" }" ) ) );
        check( o != (CxJSONObject*)0, "run-as: known-user response parses" );
        if ( o != (CxJSONObject*)0 ) {
            if ( amRoot ) {
                check( getBool( o, "ok", 0 ) == 1, "run-as(root): list as self ok==true" );
                CxJSONObject *r = getObject( o, "result" );
                check( r != (CxJSONObject*)0 && getNumber( r, "count", -1 ) == 1, "run-as(root): sees the one file" );
            } else {
                check( getBool( o, "ok", 1 ) == 0, "run-as(non-root): drop fails closed, ok==false" );
                check( contains( getString( o, "error" ), "cannot run as user" ), "run-as(non-root): error says cannot run as user" );
            }
            delete o;
        }
    }

    // read_file with a user takes the same drop path -- same root/non-root split.
    {
        CxJSONObject *o = parseObject( heliosDispatch( CxString(
            "{ \"verb\":\"read_file\", \"id\":62, \"path\":\"/tmp/helios_test_user/owned.txt\", \"user\":\"" )
            + me + CxString( "\" }" ) ) );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == ( amRoot ? 1 : 0 ),
                   amRoot ? "run-as(root): read as self ok==true"
                          : "run-as(non-root): read drop fails closed" );
            delete o;
        }
    }

    shellRun( CxString( "rm -rf " ) + CxString( dirp ) );
}


static void
testSearch( void )
{
    printf( "\n== search ==\n" );

    const char *dirp = "/tmp/helios_test_search";
    shellRun( CxString( "rm -rf " ) + CxString( dirp ) );
    mkdir( dirp, 0755 );
    writeRaw( "/tmp/helios_test_search/a.txt", "apple\nbanana\ncherry\n", 20, 0644 );
    writeRaw( "/tmp/helios_test_search/b.txt", "banana split\n", 13, 0644 );

    CxString req = CxString( "{ \"verb\":\"search\", \"id\":60, \"pattern\":\"banana\", \"path\":\"" )
                 + CxString( dirp ) + CxString( "\" }" );
    CxJSONObject *o = parseObject( heliosDispatch( req ) );
    check( o != (CxJSONObject*)0, "search response parses" );
    if ( o != (CxJSONObject*)0 ) {
        check( getBool( o, "ok", 0 ) == 1, "search ok==true" );
        CxJSONObject *r = getObject( o, "result" );
        check( r != (CxJSONObject*)0, "search has result" );
        if ( r != (CxJSONObject*)0 ) {
            check( getNumber( r, "count", -1 ) == 2, "search: 2 matches for banana" );
            check( getBool( r, "truncated", 1 ) == 0, "search: not truncated" );
            CxJSONArray *ms = getArray( r, "matches" );
            check( ms != (CxJSONArray*)0 && ms->entries() == 2, "search: matches array has 2" );
            if ( ms != (CxJSONArray*)0 && ms->entries() > 0 ) {
                CxJSONObject *m0 = (CxJSONObject*) ms->at( 0 );
                check( contains( getString( m0, "file" ), "txt" ), "search: match has file" );
                check( getNumber( m0, "line", -1 ) > 0, "search: match has line number" );
                check( contains( getString( m0, "text" ), "banana" ), "search: match text has banana" );
            }
        }
        delete o;
    }

    // no matches -> ok:true, count 0
    {
        CxString req2 = CxString( "{ \"verb\":\"search\", \"id\":61, \"pattern\":\"zzznotfound\", \"path\":\"" )
                      + CxString( dirp ) + CxString( "\" }" );
        CxJSONObject *e = parseObject( heliosDispatch( req2 ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 0 ) == 1, "search no-match still ok==true" );
            CxJSONObject *r = getObject( e, "result" );
            check( r != (CxJSONObject*)0 && getNumber( r, "count", -1 ) == 0, "search no-match count 0" );
            delete e;
        }
    }

    // missing pattern -> ok:false
    {
        CxJSONObject *e = parseObject( heliosDispatch(
            CxString( "{ \"verb\":\"search\", \"id\":62, \"path\":\"/tmp\" }" ) ) );
        if ( e != (CxJSONObject*)0 ) {
            check( getBool( e, "ok", 1 ) == 0, "search missing pattern ok==false" );
            delete e;
        }
    }

    shellRun( CxString( "rm -rf " ) + CxString( dirp ) );
}


//-----------------------------------------------------------------------------------------
// testAuth -- shared-secret gate (require-if-configured). Sets a secret, checks
// missing/wrong/right "auth", then restores the open posture so the rest of the
// suite (which sends no auth) keeps passing.
//-----------------------------------------------------------------------------------------
static void
testAuth( void )
{
    printf( "\n== auth (shared secret) ==\n" );

    heliosSetSecret( "s3cr3t-xyz" );

    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\":\"hello\", \"id\":70 }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 1 ) == 0, "no auth -> ok==false" );
            check( contains( getString( o, "error" ), "unauthorized" ), "no auth -> 'unauthorized'" );
            delete o;
        }
    }
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\":\"hello\", \"id\":71, \"auth\":\"wrong\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 1 ) == 0, "wrong auth -> ok==false" );
            delete o;
        }
    }
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\":\"hello\", \"id\":72, \"auth\":\"s3cr3t-xyz\" }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "correct auth -> ok==true" );
            delete o;
        }
    }

    heliosSetSecret( (const char*)0 );   // restore open posture
    {
        CxString resp = heliosDispatch( CxString( "{ \"verb\":\"hello\", \"id\":73 }" ) );
        CxJSONObject *o = parseObject( resp );
        if ( o != (CxJSONObject*)0 ) {
            check( getBool( o, "ok", 0 ) == 1, "no secret configured -> open (ok==true)" );
            delete o;
        }
    }
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
    testMissingVerb();
    testBadJson();
    testDefaultId();
    testResponseEscaping();
    testRunCommand();
    testReadFile();
    testWriteFile();
    testStat();
    testListDir();
    testRunAsUser();
    testSearch();
    testAuth();
    testShutdown();

    printf( "\n======================\n" );
    printf( "Results: %d passed, %d failed\n", testsPassed, testsFailed );

    return testsFailed > 0 ? 1 : 0;
}
