#!/bin/sh
#
# deploy.sh -- install heliosAgent into the guest's boot system, after build.
#
# Multi-platform. Detects the guest OS and wires autostart the native way:
#
#   Solaris 2.6 (SunOS 5.x)  -- SysV init: /etc/init.d + rc2.d/S98 + rcN.d/K30
#   NetBSD 9.x               -- rc.d: /etc/rc.d/heliosagent + rc.conf=YES
#   SunOS 4.1.4 (SunOS 4.x)  -- old BSD: a managed stanza in /etc/rc.local
#
# Run as root on the guest, from the heliosAgent source dir, AFTER building:
#
#   <build the binary -- make, or build_helios_netbsd.sh>
#   ./deploy.sh
#
# The binary is located automatically (see "locate the binary" below): the
# makefile's <PLATFORM_OS>_<arch>/heliosAgent build dir on EVERY platform (same
# token cx's platform.mk uses -- solaris6 / solaris10 / sunos4 / netbsd), with a
# source-dir ./heliosAgent (the make-free NetBSD bootstrap) only as a last
# resort. Override explicitly with:  BIN=/path/to/heliosAgent ./deploy.sh
#
# Re-runnable / idempotent: it doubles as the upgrade path -- rebuild, re-run,
# and it restarts onto the new binary.
#
# Bourne-sh only -- this has to run under SunOS 4.1.4 /bin/sh too, so: backticks
# not $(...), no `local`, no bashisms.

# --- locations (keep LOG/PIDFILE in step with the init scripts) ------------
PORT=2125
BINDEST=/usr/local/bin/heliosAgent
LOG=/var/log/heliosAgent.log
PIDFILE=/var/run/heliosAgent.pid

# --- detect the platform ---------------------------------------------------
OS=`uname -s`
REL=`uname -r`
# PLATFORM   = autostart flavor (how we wire boot: SysV / rc.d / rc.local).
# MKOS       = the makefile's PLATFORM_OS dir token (must match cx/platform.mk),
#              used to find <MKOS>_<arch>/heliosAgent. Keep in sync with
#              platform.mk: 4.x->sunos4, 5.6/5.7->solaris6, 5.10->solaris10.
case "$OS" in
	SunOS)
		case "$REL" in
			4.*)     PLATFORM=sunos4;  MKOS=sunos4 ;;
			5.6|5.7) PLATFORM=solaris; MKOS=solaris6 ;;
			5.10)    PLATFORM=solaris; MKOS=solaris10 ;;
			5.*)     PLATFORM=solaris; MKOS=sunos ;;
			*)       PLATFORM=unknown ;;
		esac ;;
	NetBSD) PLATFORM=netbsd; MKOS=netbsd ;;
	*)      PLATFORM=unknown ;;
esac
if [ "$PLATFORM" = unknown ]; then
	echo "deploy: unsupported platform: $OS $REL (know solaris, netbsd, sunos4)"
	exit 1
fi
echo "deploy: platform = $PLATFORM ($OS $REL)"

# --- root check ------------------------------------------------------------
# SunOS 4/5 /usr/bin/id has no -u flag, so parse the uid out of "uid=0(root)...".
UID=`id | sed 's/uid=\([0-9][0-9]*\).*/\1/'`
if [ "$UID" != "0" ]; then
	echo "deploy: must run as root (try: su root -c ./deploy.sh)."
	exit 1
fi

# --- locate the binary -----------------------------------------------------
# Search order: $BIN override, then the makefile's <MKOS>_<arch>/heliosAgent
# build dir (the same on every platform -- ARCH often comes out empty on the
# Suns, so glob it), then a source-dir ./heliosAgent (the make-free NetBSD
# bootstrap) as a last resort. First executable hit wins. We deliberately do
# NOT glob */heliosAgent, because init/heliosAgent is the (executable) control
# script and must not be mistaken for the daemon.
if [ -z "$BIN" ]; then
	for cand in ${MKOS}_*/heliosAgent ./heliosAgent; do
		if [ -x "$cand" ]; then BIN="$cand"; break; fi
	done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
	echo "deploy: heliosAgent binary not found."
	echo "        looked for ${MKOS}_*/heliosAgent and ./heliosAgent"
	echo "        build it first, or set BIN=/path/to/heliosAgent ./deploy.sh"
	exit 1
fi
echo "deploy: binary = $BIN"

# --- install the binary ----------------------------------------------------
echo "deploy: installing $BIN -> $BINDEST"
mkdir -p /usr/local/bin
# Install atomically via a temp name + rename. On the upgrade path the daemon
# is still running from $BINDEST, so a direct `cp` over it fails with ETXTBSY
# ("Text file busy") and would silently leave the OLD binary in place (the
# restart below then runs stale code). rename() over a busy executable is fine
# -- the running process keeps its inode until the restart execs the new file.
cp "$BIN" "$BINDEST.new"
chmod 755 "$BINDEST.new"
mv "$BINDEST.new" "$BINDEST"

# --- secret directory (never clobber an existing secret) -------------------
# On real hardware (no OBP eeprom secret) the daemon reads its shared secret from
# /etc/helios/helios.json. Make the directory so an admin has a locked-down place
# to drop it, but NEVER create or overwrite the file itself: on an over-helios
# redeploy that would change the secret out from under the client doing the
# deploy and lock it out (recoverable only via ssh/console).
SECRETDIR=/etc/helios
SECRETFILE=$SECRETDIR/helios.json
mkdir -p "$SECRETDIR"
chmod 700 "$SECRETDIR"
# If a secret file already exists, tighten its perms (never touch its contents):
# it holds the plaintext shared secret and must not be group/other-readable.
if [ -f "$SECRETFILE" ]; then
	chmod 600 "$SECRETFILE"
fi

# --- lockout guard ---------------------------------------------------------
# After the restart below the new daemon is fail-closed: with no OBP secret and
# no secret file it DENIES every request. If this deploy is running over helios
# itself, that drops the deploying client with no way back except ssh/console.
# Warn loudly -- don't block, since a hand/ssh deploy is a legitimate way to
# bootstrap the very first secret.
# Resolve eeprom to an absolute path (it's /usr/kvm/eeprom on SunOS 4.1.4, off
# the PATH; /usr/sbin on Solaris) so this guard doesn't false-warn there.
EEPROM=eeprom
for _e in /usr/sbin/eeprom /usr/kvm/eeprom /usr/bin/eeprom; do
	if [ -x "$_e" ]; then EEPROM="$_e"; break; fi
done
EEPROM_SECRET=`$EEPROM helios-secret 2>/dev/null | grep '^helios-secret=' | sed 's/^helios-secret=//'`
if [ -z "$EEPROM_SECRET" ] && [ ! -f "$SECRETFILE" ]; then
	echo "deploy: WARNING -- no OBP secret and no $SECRETFILE:"
	echo "deploy:   the (re)started agent will DENY ALL requests (fail-closed)."
	echo "deploy:   if you are deploying over helios you will lose the channel."
	echo "deploy:   create $SECRETFILE (mode 600) or recover via ssh/console."
fi

# --- per-platform autostart wiring -----------------------------------------
case "$PLATFORM" in

solaris|sunos4)
	# Both use the SysV-style control script init/heliosAgent. It's framework-
	# agnostic (pidfile + kill + eeprom secret), so it serves as the daemon
	# control on SunOS 4 as well; only the *autostart* wiring differs.
	INITSRC=init/heliosAgent
	INITDEST=/etc/init.d/heliosAgent
	if [ ! -f "$INITSRC" ]; then
		echo "deploy: $INITSRC not found -- run from the heliosAgent source dir."
		exit 1
	fi
	echo "deploy: installing control script -> $INITDEST"
	mkdir -p /etc/init.d
	cp "$INITSRC" "$INITDEST"
	chmod 755 "$INITDEST"

	if [ "$PLATFORM" = solaris ]; then
		# SysV run-level symlinks: S98 start near end of multiuser, K30 kill
		# on the way down (shutdown rc0, single-user rc1, single-user boot rcS).
		echo "deploy: wiring SysV rc symlinks"
		rm -f /etc/rc2.d/S98heliosAgent
		ln -s ../init.d/heliosAgent /etc/rc2.d/S98heliosAgent
		for d in rc0.d rc1.d rcS.d; do
			rm -f /etc/$d/K30heliosAgent
			ln -s ../init.d/heliosAgent /etc/$d/K30heliosAgent
		done
	else
		# SunOS 4.1.4: no SysV run levels. Hook /etc/rc.local with a stanza
		# bounded by markers, so re-running replaces it cleanly (idempotent).
		echo "deploy: wiring /etc/rc.local stanza"
		RCL=/etc/rc.local
		if [ ! -f "$RCL" ]; then
			echo "#!/bin/sh" > "$RCL"
			chmod 755 "$RCL"
		fi
		TMP=/tmp/rc.local.$$
		sed '/# BEGIN heliosAgent/,/# END heliosAgent/d' "$RCL" > "$TMP"
		cat >> "$TMP" <<EOF
# BEGIN heliosAgent (managed by deploy.sh -- do not edit between markers)
if [ -x $INITDEST ]; then $INITDEST start; fi
# END heliosAgent
EOF
		cp "$TMP" "$RCL"
		rm -f "$TMP"
	fi

	echo "deploy: (re)starting heliosAgent"
	"$INITDEST" restart
	sleep 1
	"$INITDEST" status
	;;

netbsd)
	INITSRC=init/heliosagent.netbsd
	INITDEST=/etc/rc.d/heliosagent
	if [ ! -f "$INITSRC" ]; then
		echo "deploy: $INITSRC not found -- run from the heliosAgent source dir."
		exit 1
	fi
	echo "deploy: installing rc.d script -> $INITDEST"
	cp "$INITSRC" "$INITDEST"
	chmod 755 "$INITDEST"
	# Enable in rc.conf (idempotent) so it starts at boot.
	grep -q '^heliosagent=' /etc/rc.conf || echo 'heliosagent=YES' >> /etc/rc.conf
	echo "deploy: (re)starting via rc.d"
	"$INITDEST" restart
	sleep 1
	"$INITDEST" status
	;;

esac

echo "deploy: done. platform=$PLATFORM  logs: $LOG  pid: $PIDFILE"
