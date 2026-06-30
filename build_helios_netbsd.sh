#!/bin/sh
#
# build_helios_netbsd.sh -- make-free build of heliosAgent for NetBSD.
#
# Compiles the 6 cx libraries heliosAgent needs (base, net, json, b64, log,
# process) plus the daemon itself, using g++ directly. No make/gmake required.
#
# Run it from the project ROOT -- the directory that contains both cx/ and
# cx_apps/ as siblings (the standard tarball-extraction layout):
#
#     sh cx_apps/heliosAgent/build_helios_netbsd.sh
#
# Override the compiler if g++ is not the one you want:
#     CXX=c++ sh cx_apps/heliosAgent/build_helios_netbsd.sh
#
set -e

# --- locate the project root -------------------------------------------------
ROOT=$(pwd)
if [ ! -d "$ROOT/cx/base" ] || [ ! -d "$ROOT/cx_apps/heliosAgent" ]; then
    echo "FAIL: run this from the dir that contains cx/ and cx_apps/ (project root)."
    echo "      You are in: $ROOT"
    exit 1
fi

CXX=${CXX:-g++}
CC=${CC:-gcc}          # nxjson.c is C, not C++ -- it must be built with a C compiler
ARCH=$(uname -m | tr '[A-Z]' '[a-z]')
OBJ="$ROOT/build_netbsd_$ARCH"
FLAGS="-D_NETBSD_ -g -I$ROOT"

mkdir -p "$OBJ"
echo "ROOT=$ROOT"
echo "CXX =$CXX   ($($CXX --version | head -1))"
echo "ARCH=$ARCH   OBJ=$OBJ"
echo

# --- compile one source to an object; record the object path -----------------
# Object names are the source path with '/' -> '_' so nothing collides.
OBJS=""
compile() {        # $1 = source path relative to ROOT (no extension assumed)
    src="$ROOT/$1"
    o="$OBJ/$(echo "$1" | tr / _).o"
    printf '  CC   %s\n' "$1"
    $CXX $FLAGS -c "$src" -o "$o"
    OBJS="$OBJS $o"
}

# --- library sources (the authoritative list from each lib's makefile) -------
echo "=== compiling cx libraries ==="

for n in buffer cntbody directory double err exception file fileaccess filei \
         filename prop rbuffer rule star string time timeval tokenizer \
         utfcharacter utfstring ; do
    compile "cx/base/$n.cpp"
done

for n in asocket inaddr inaddri socket socketi ; do
    compile "cx/net/$n.cpp"
done

for n in json_array json_base json_boolean json_factory json_member json_null \
         json_number json_object json_string json_utf_array json_utf_base \
         json_utf_boolean json_utf_member json_utf_null json_utf_number \
         json_utf_object json_utf_string ; do
    compile "cx/json/$n.cpp"
done
# nxjson.c is C source and MUST be compiled as C (it uses C-only implicit
# conversions that a C++ compiler rejects). The cx json/makefile builds it with
# gcc for exactly this reason.
nx_o="$OBJ/cx_json_nxjson.o"
printf '  CC   cx/json/nxjson.c   (as C, via %s)\n' "$CC"
$CC -D_NETBSD_ -g -c "$ROOT/cx/json/nxjson.c" -o "$nx_o"
OBJS="$OBJS $nx_o"

compile "cx/b64/b64.cpp"
compile "cx/log/logfile.cpp"
compile "cx/process/process.cpp"

LIB_OBJS="$OBJS"      # everything compiled so far = the cx libraries

# --- application objects ------------------------------------------------------
echo "=== compiling heliosAgent ==="
OBJS=""
compile "cx_apps/heliosAgent/Dispatch.cpp"
compile "cx_apps/heliosAgent/Verbs.cpp"
SHARED_APP_OBJS="$OBJS"          # Dispatch + Verbs (shared by daemon and test)

OBJS=""
compile "cx_apps/heliosAgent/HeliosAgent.cpp"   # contains main()
MAIN_OBJ="$OBJS"

# --- link the daemon ----------------------------------------------------------
BIN="$ROOT/cx_apps/heliosAgent/heliosAgent"
echo "=== linking $BIN ==="
$CXX $FLAGS $LIB_OBJS $SHARED_APP_OBJS $MAIN_OBJ -o "$BIN" -lpthread
echo "OK: built $BIN"
file "$BIN" || true
echo

# --- optionally build the unit test (drives heliosDispatch() directly) -------
TEST_SRC="$ROOT/cx_apps/heliosAgent/test/helios_test.cpp"
if [ -f "$TEST_SRC" ]; then
    echo "=== compiling + linking unit test ==="
    # helios_test.cpp lives in test/ but #include "Dispatch.h"/"Verbs.h" from the
    # parent app dir, so that dir must be on the include path too.
    test_o="$OBJ/cx_apps_heliosAgent_test_helios_test.o"
    printf '  CC   cx_apps/heliosAgent/test/helios_test.cpp\n'
    $CXX $FLAGS -I"$ROOT/cx_apps/heliosAgent" -c "$TEST_SRC" -o "$test_o"
    OBJS="$test_o"
    TESTBIN="$ROOT/cx_apps/heliosAgent/helios_test"
    # test links the libs + Dispatch/Verbs, but NOT HeliosAgent.o (would dup main)
    $CXX $FLAGS $LIB_OBJS $SHARED_APP_OBJS $OBJS -o "$TESTBIN" -lpthread
    echo "OK: built $TESTBIN"
    echo
fi

echo "=== DONE ==="
echo "daemon : $BIN"
[ -f "$ROOT/cx_apps/heliosAgent/helios_test" ] && echo "test   : $ROOT/cx_apps/heliosAgent/helios_test"
echo
echo "Next:"
echo "  # run the unit test (expect exit 0):"
echo "  $ROOT/cx_apps/heliosAgent/helios_test"
echo
echo "  # live handshake on port 21250:"
echo "  $BIN -p 21250 &"
echo "  sleep 1; printf '{\"verb\":\"hello\",\"id\":1}\\n' | nc -w 2 127.0.0.1 21250; echo"
echo "  kill %1"
