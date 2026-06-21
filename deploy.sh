#!/bin/sh
#
# deploy.sh -- install heliosAgent into the Solaris init system, after build.
#
# Run as root on the Sun, from the heliosAgent source dir, AFTER `make`:
#
#   make
#   ./deploy.sh
#
# It installs the freshly built binary to /usr/local/bin, drops the init script
# into /etc/init.d, wires the rc symlinks (start at multiuser, kill on shutdown/
# single-user), and (re)starts the daemon. Re-runnable: it's idempotent, so it
# doubles as the upgrade path (rebuild, re-run, it restarts onto the new binary).
#
# Bourne-sh only (no bashisms): this has to run under Solaris 2.6 /bin/sh.

# --- locations (keep LOG/PIDFILE in step with init/heliosAgent) ------------
ARCH=`uname -s | tr '[A-Z]' '[a-z]'`_`uname -m | tr '[A-Z]' '[a-z]'`
BIN="$ARCH/heliosAgent"
BINDEST=/usr/local/bin/heliosAgent
INITSRC=init/heliosAgent
INITDEST=/etc/init.d/heliosAgent

# --- preflight -------------------------------------------------------------
if [ ! -x "$BIN" ]; then
	echo "deploy: $BIN not found -- run 'make' first."
	exit 1
fi
if [ ! -f "$INITSRC" ]; then
	echo "deploy: $INITSRC not found -- run from the heliosAgent source dir."
	exit 1
fi

# Root check that works on Solaris 2.6 /usr/bin/id (no -u flag there): parse the
# uid out of `id`'s "uid=0(root) ..." output.
UID=`id | sed 's/uid=\([0-9][0-9]*\).*/\1/'`
if [ "$UID" != "0" ]; then
	echo "deploy: must run as root (try: su root -c ./deploy.sh)."
	exit 1
fi

# --- install the binary ----------------------------------------------------
echo "deploy: installing $BIN -> $BINDEST"
mkdir -p /usr/local/bin
cp "$BIN" "$BINDEST"
chmod 755 "$BINDEST"

# --- install the init script -----------------------------------------------
echo "deploy: installing init script -> $INITDEST"
cp "$INITSRC" "$INITDEST"
chmod 755 "$INITDEST"

# --- wire the rc symlinks (idempotent: remove then relink) -----------------
# S98 = start near the end of multiuser bring-up; K30 = kill on the way down
# (shutdown rc0, single-user rc1, single-user boot rcS).
echo "deploy: wiring rc symlinks"
rm -f /etc/rc2.d/S98heliosAgent
ln -s ../init.d/heliosAgent /etc/rc2.d/S98heliosAgent
for d in rc0.d rc1.d rcS.d; do
	rm -f /etc/$d/K30heliosAgent
	ln -s ../init.d/heliosAgent /etc/$d/K30heliosAgent
done

# --- (re)start onto the new binary -----------------------------------------
echo "deploy: (re)starting heliosAgent"
"$INITDEST" restart

# --- report ----------------------------------------------------------------
sleep 1
"$INITDEST" status
echo "deploy: done. logs: /var/log/heliosAgent.log   pid: /var/run/heliosAgent.pid"
