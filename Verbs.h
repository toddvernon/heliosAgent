//-------------------------------------------------------------------------------------------------
//
//  Verbs.h
//  heliosAgent
//
//  One function per protocol verb. Each takes the request's echo id (and, as
//  verbs grow, the parsed request object) and returns a complete response line
//  as a CxString. See PROTOCOL.md for the verb set and Dispatch.cpp for routing.
//
//  File verbs (read_file, write_file, stat, list_dir, get_file, put_file) take
//  an optional "user": the daemon runs as root, so without it they see root's
//  view and create root-owned files; with it they do a reversible euid/egid
//  drop (HeliosPrivGuard in Verbs.cpp) so the op runs AS that user. An unknown
//  user or a drop that can't complete is ok:false -- it fails closed, never
//  runs as root.
//
//-------------------------------------------------------------------------------------------------

#ifndef HELIOS_VERBS_H
#define HELIOS_VERBS_H

#include <cx/base/string.h>

class CxJSONObject;   // the parsed request object (forward decl)
class CxSocket;       // the connection, for the streaming verbs (forward decl)

// Liveness handshake: agent name, version, protocol, hostname, daemon uptime.
CxString verbHello( double id );

// Run a shell command on the Sun. Reads "cmd" (required), "cwd" (optional), and
// "timeout_ms" (optional) from the request; returns
// { exit_code, output, timed_out }. ok:true means the command was *run* (a
// nonzero exit is reported in exit_code, not as a daemon error). Backed by
// CxProcess (fork/exec with cwd + timeout).
CxString verbRun( double id, CxJSONObject *req );

// Read a whole regular file and return its bytes as base64. Reads "path"
// (required). Result: { path, size, mode, encoding:"base64", content }. ok:false
// on a missing path, a non-existent file, or a non-regular file (dir/symlink/
// device): use `stat` to learn the type first. Content travels base64 so the
// channel stays byte-exact (NUL/high-bytes safe); the raw bytes never pass
// through a CxString.
CxString verbReadFile( double id, CxJSONObject *req );

// Write base64 content to a regular file, atomically. Reads "path" (required),
// "content" (required, base64), and "mode" (optional, low 12 perm bits). Writes
// a temp file in the target's directory then rename()s it over the target, so a
// crash never leaves a half-file. Permission policy (see HELIOS_PLAN.md B5):
//   - explicit "mode" wins when given (new files, or a deliberate chmod);
//   - else, overwriting an existing file PRESERVES its mode (and owner, when the
//     daemon runs as root) -- rename() installs a fresh inode, so without this
//     every write would silently reset perms to a default and break scripts/
//     configs;
//   - else (new file, no mode) defaults to 0644.
// mtime is deliberately NOT preserved: a write stamps it to now so `make`
// rebuilds. Result: { path, bytes_written, mode, created }.
CxString verbWriteFile( double id, CxJSONObject *req );

// Metadata for one path (lstat, so a symlink reports as a symlink rather than
// its target). Reads "path" (required). Result: { path, type, size, mode, uid,
// gid, mtime } and, for a symlink, "target". type is one of file/dir/symlink/
// fifo/chardev/blockdev/socket/other. ok:false if the path can't be stat'd.
CxString verbStat( double id, CxJSONObject *req );

// List a directory's entries (excluding "." and ".."), each lstat'd. Reads
// "path" (required). Result: { path, count, entries:[ { name, type, size,
// mode, mtime }, ... ] }. ok:false if the path isn't a readable directory.
CxString verbListDir( double id, CxJSONObject *req );

// Search file contents on the guest by shelling native grep (run it where the
// files are). Reads "pattern" (required), "path" (optional, default "."),
// "ignore_case" (optional bool), "max" (optional cap, default 1000), and
// "timeout_ms" (optional). Result: { pattern, path, count, truncated,
// exit_code, timed_out, matches:[ { file, line, text }, ... ] }. The pattern
// and path are shell-quoted and grep's stderr is discarded so error text never
// pollutes parsed matches. ok:false only on a missing pattern. NB: needs a
// grep that supports -rHn -e (GNU/xpg4 grep on Solaris; see HELIOS_PLAN A4).
CxString verbSearch( double id, CxJSONObject *req );

// Streaming file upload (put_file) / download (get_file). Unlike read_file/
// write_file (base64 in one JSON line, fine for small configs), these stream
// raw bytes straight to/from disk with no base64 and no full-file buffering --
// the path for bulk transfers. The header is one JSON line; a length-prefixed
// raw body follows (put_file: client sends `bytes` raw bytes after the header;
// get_file: daemon sends `bytes` raw bytes after an ok header). They own their
// own socket I/O (response + body), so they take the connection. Return:
//   1 = handled, connection may continue
//   2 = handled, connection must close (framing unrecoverable: a header with no
//       usable `bytes`, so the body length is unknown and the stream can't resync)
// A socket error mid-body throws CxSocketException (handleConnection closes).
//
// put_file: header { verb:"put_file", id, path, bytes:N [, mode] } + N raw
// bytes. Streams to a temp file, atomic-renames over path (same mode policy as
// write_file). Result: { path, bytes_written, mode, created }.
int verbPutFileStream( double id, CxJSONObject *req, CxSocket &conn );

// get_file: header { verb:"get_file", id, path }. On success the daemon replies
// { id, ok:true, result:{ path, bytes:N, mode } } then sends N raw bytes; a
// missing/non-regular path is ok:false with NO body. Regular files only.
int verbGetFileStream( double id, CxJSONObject *req, CxSocket &conn );

// Graceful shutdown. Returns an ACK ({status:"shutting down"}) and records the
// intent; it does NOT exec anything itself, so dispatching it (e.g. from a unit
// test) is side-effect-free. The server performs the real shutdown only after
// the ACK has been sent -- see heliosShutdownRequested() + HeliosAgent.cpp.
CxString verbShutdown( double id );

// True once verbShutdown has been dispatched on this (forked) connection. The
// server checks this after sending the response and then execs the shutdown
// command. Kept out of the verb so tests never trigger a real shutdown.
int heliosShutdownRequested( void );

#endif
