# Helios agent wire protocol (v1)

heliosAgent speaks a line-oriented JSON request/response protocol over one
persistent TCP connection. A client (macXserver's HeliosClient, and later the
SPARCplug MCP server) connects, then sends requests and reads responses, one
per line. The same protocol reaches the bundled emulator over a slirp hostfwd
and a real Sun over its network.

## Framing

- One JSON object per line, terminated by a single newline (`\n`).
- One request in flight at a time: the client reads the response line before
  sending the next request.
- The connection is persistent (many request/response pairs per connection).

## Request

    { "verb": "<name>", "id": <number>, ...verb-specific fields... }

- `verb` (string, required): which operation.
- `id` (number, optional): echoed in the response so the client can correlate.
  Defaults to 0 if omitted.

## Response

Success:

    { "id": <number>, "ok": true, "result": { ... } }

Failure (bad JSON, missing verb, unknown verb, verb error):

    { "id": <number>, "ok": false, "error": "<message>" }

Protocol errors do not drop the connection; they come back as ok:false.

## Content encoding

File content (read_file / write_file, once implemented) travels **base64** in a
JSON string field, not as a raw JSON string. That keeps the channel byte-exact
and sidesteps the JSON parser's NUL-termination edge. Metadata stays plain JSON.

## Verbs

Ranked by priority (control holes first). Implemented verbs are marked.

**Run-as (`user`) on the file verbs.** Like `run_command`, every file verb
(`read_file`, `write_file`, `stat`, `list_dir`, `get_file`, `put_file`) accepts
an optional `"user"`. The daemon runs as root, so without it a browse sees
root's view of the filesystem and an upload lands owned by root -- wrong for
user-facing tools (the macXserver file browser). When `user` is set, the daemon
`getpwnam`-validates it, then does a reversible effective-id drop
(`initgroups` + `setegid` + `seteuid`) for the duration of that one op, so the
op is permission-checked AS the user and any new file is owned by the user; it
restores root afterward. fork-per-connection makes the process-wide euid change
safe (one short-lived child per request). An unknown user is `ok:false`
("unknown user"); a drop that can't complete (e.g. the daemon isn't root) is
`ok:false` ("cannot run as user") -- it fails closed, never silently operating
as root. Absent/empty `user` keeps the historical root behavior (admin edits
like the DNS editor). The drop needs the daemon to be root, which it is in the
shipped guest.

- `hello` -- liveness handshake. **[implemented]**
  - request: `{ "verb": "hello", "id": 1 }`
  - result: `{ "agent":"heliosAgent", "version":"0.1.0", "protocol":1,
              "host":"<hostname>", "uptime":<seconds the daemon has run> }`
- `shutdown` -- graceful shutdown. **[implemented]**
  - request: `{ "verb": "shutdown", "id": 2 }`
  - result: `{ "status": "shutting down" }`
  - The agent ACKs first, then the server runs the shutdown command, so the
    client always learns the guest is going down before it does.
  - **The ACK confirms RECEIPT, not power-off.** The command runs after the
    reply is on the wire and its result isn't awaited by the client, so a
    successful `{"status":"shutting down"}` does not prove the guest went down.
    Confirm power-off out of band (the qemu process exiting). A failed shutdown
    command is logged (`shutdown command failed (rc=...)`) but the ACK already
    went out -- this ACK-vs-poweroff gap hid a real bug for a while.
  - **The command is per-OS**, and each must be an ABSOLUTE path (the daemon
    runs it via `/bin/sh` with a minimal init PATH, so a bare `halt` is
    "command not found"). It's chosen at compile time per guest, and overridable
    at runtime via `HELIOS_SHUTDOWN_CMD` (the dev override, so the default
    doesn't power off the developer's Mac):
    - Solaris 2.6: `/usr/sbin/init 5`  (SVR4; syncs + powers off)
    - SunOS 4.1.4: `/usr/etc/halt`
    - NetBSD: `/sbin/halt`  (syncs + halts; exits qemu with no `-p` needed)

    These must stay in step with `MachineOS.shutdownCommand` in the swift-x tree.
    The former universal default `init 5` no-ops on the BSD guests (no SVR4
    runlevels), so it silently failed to power them off.
- `run_command` -- run a shell command on the Sun. **[implemented]**
  - request: `{ "verb":"run_command", "id":8, "cmd":"cc hello.c -o hello",
              "cwd":"/export/home/me", "timeout_ms":60000, "user":"me" }`
    (`cwd`, `timeout_ms`, `user` all optional; absent cwd inherits, absent/<=0
    timeout waits indefinitely, absent/empty user runs as the daemon)
  - result: `{ "exit_code":N, "output":"<combined stdout+stderr>",
              "timed_out":bool [, "user":"<user>"] }`
  - `ok:true` means the command *ran*; a nonzero exit is reported in
    `exit_code` (128+signal if killed), not as a daemon error. A
    missing/invalid `cmd`, or an unknown `user`, is `ok:false`. Output is a JSON
    string (escaped); file *content* still uses base64 (read_file/write_file).
  - **`user` (run-as):** when set, the daemon (which runs as root) drops
    privileges to that user before exec -- `getpwnam`-validated, then
    `initgroups`+`setgid`+`setuid` with a login-ish `HOME`/`USER`/`LOGNAME`/
    `SHELL` environment. A failed drop exits 127 rather than running as root.
    Absent/empty `user` runs as the daemon (root) -- intended for admin tasks;
    user-facing work (e.g. launching X clients) should always name a user so it
    never runs as root. `user` is echoed back in the result when it was applied.
    Requires the daemon to be root (it is, for shutdown / system writes).
- `read_file` -- read a whole regular file. **[implemented]**
  - request: `{ "verb":"read_file", "id":20, "path":"/etc/hosts" }`
  - result: `{ "path":"...", "size":N, "mode":<low 12 perm bits, decimal>,
              "encoding":"base64", "content":"<base64>" }`
  - Content is base64 so the channel is byte-exact (NUL / high bytes safe);
    raw bytes never pass through a JSON string. Regular files only: a missing
    path, a non-existent file, or a non-regular target (dir/symlink/device) is
    `ok:false` -- use `stat` to learn the type first.
- `write_file` -- write a whole regular file, atomically. **[implemented]**
  - request: `{ "verb":"write_file", "id":21, "path":"/etc/hosts",
              "content":"<base64>", "mode":420 }` (`mode` optional, decimal of
    the low 12 perm bits, e.g. 420 == 0644)
  - result: `{ "path":"...", "bytes_written":N, "mode":<final perm bits>,
              "created":bool }`
  - Writes a temp file in the target's directory then `rename()`s over the
    target, so a crash never leaves a half-file. Permission policy: an explicit
    `mode` wins; else overwriting an existing file **preserves** its mode (and
    owner, when the daemon runs as root) -- `rename()` installs a fresh inode,
    so without this every write would silently reset perms and break scripts/
    configs; else a new file defaults to 0644. mtime is deliberately **not**
    preserved (a write stamps it to now so `make` rebuilds). Missing/invalid
    `path` or `content`, or a non-regular existing target, is `ok:false`.
  - **For small files only** (a config, a script). The whole payload rides one
    base64 JSON line, so multi-MB files are slow/heavy -- use `put_file` for bulk.
- `put_file` -- streaming file **upload**, for bulk transfers. **[implemented]**
  - request header (one JSON line): `{ "verb":"put_file", "id":22,
    "path":"/dest", "bytes":N, "mode":420 }` (`mode` optional) **followed
    immediately by exactly N raw bytes** on the same connection.
  - result: `{ "path":"...", "bytes_written":N, "mode":<final>, "created":bool }`
  - This is the **one place the protocol carries a raw body** instead of pure
    one-line JSON. The daemon streams the N bytes straight to a temp file in
    bounded chunks (no base64, no full-file buffering) then atomic-renames over
    the target -- same mode policy as `write_file`. It's how bulk bytes should
    move (FTP/scp-speed), vs. `write_file`'s base64-in-one-line (fine only for
    small files). Errors: a header with no usable `bytes` can't be framed, so
    the daemon answers `ok:false` and **closes the connection**; a recoverable
    error after the byte count is known (non-regular target, write failure)
    drains the body to stay framed and answers `ok:false`.
- `get_file` -- streaming file **download**, the `put_file` mirror. **[implemented]**
  - request: `{ "verb":"get_file", "id":23, "path":"/src" }`
  - on success: a header line `{ "id":23, "ok":true, "result":{ "path":"...",
    "bytes":N, "mode":<perm> } }` **followed by exactly N raw bytes** (the file).
    The client reads the header, then reads `result.bytes` raw bytes.
  - Regular files only; a missing/non-regular path is `ok:false` with **no body**.
    Same rationale as `put_file`: bulk bytes stream raw, not base64 (`read_file`
    stays the small-file/base64 path).
- `stat` -- metadata for one path. **[implemented]**
  - request: `{ "verb":"stat", "id":40, "path":"/etc/passwd" }`
  - result: `{ path, type, size, mode, uid, gid, mtime[, target] }`
  - Uses `lstat`, so a symlink reports `type:"symlink"` and a `target` field
    rather than its destination. type is one of file/dir/symlink/fifo/chardev/
    blockdev/socket/other. ok:false if the path can't be stat'd.
- `list_dir` -- list a directory. **[implemented]**
  - request: `{ "verb":"list_dir", "id":50, "path":"/etc" }`
  - result: `{ path, count, entries:[ { name, type, size, mode, mtime }, ... ] }`
  - Excludes "." and ".."; each entry is `lstat`'d. ok:false if the path isn't
    a readable directory.
- `search` -- grep file contents on the guest. **[implemented]**
  - request: `{ "verb":"search", "id":60, "pattern":"TODO", "path":"src",
              "ignore_case":false, "max":1000, "timeout_ms":30000 }`
    (`path` default ".", `max` default 1000, `timeout_ms` default 60000; all of
    `ignore_case`, `max`, `timeout_ms` optional)
  - result: `{ pattern, path, count, truncated, exit_code, timed_out,
              matches:[ { file, line, text }, ... ] [, error ] }`
  - Shells grep (`<grep> -rHn`, run where the files are), shell-quoting the
    pattern and path so metacharacters can't inject, and discarding grep's stderr
    so error text is never mis-parsed as a match. No-match (grep exit 1) is
    `ok:true` with `count:0`; a missing `pattern` is `ok:false`.
    `truncated:true` means `max` was hit (matches are not silently dropped).
  - **grep must be an ABSOLUTE path** to a capable grep -- a bare `grep` on the
    daemon's minimal PATH resolves to a base grep with no `-r` on Solaris 2.6 /
    SunOS 4.1.4, which errors. It's a per-OS compiled default (GNU grep at
    `/usr/local/bin/grep` where installed; NetBSD's `/usr/bin/grep` is fine),
    overridable via `HELIOS_GREP`. A grep error (exit >= 2: missing/incompatible
    grep, or unreadable path) is now `ok:false` with an `error` field -- it no
    longer masquerades as an empty result set.
  - `timeout_ms` defaults to 60s rather than unbounded, so a path-less search
    from the daemon's cwd (`/`) can't become a whole-filesystem grep; pass a
    larger value for big searches.

All eight v1 verbs are implemented. Unrecognized verbs return "unknown verb";
malformed requests return a protocol error. Both are `ok:false`, never a
dropped connection.

## Example

    -> { "verb": "hello", "id": 1 }
    <- { "id":1, "ok":true, "result":{ "agent":"heliosAgent", "version":"0.1.0", "protocol":1, "host":"sparcplug", "uptime":42 } }

## Notes

- Default listen port is 2125 (`-p` to change; a bare positional port still
  works for back-compat).
- Binds INADDR_ANY so it's reachable through slirp inside the guest, with
  SO_REUSEADDR set so a restart doesn't trip over a port in TIME_WAIT.
- Shared-secret auth (require-always / fail-closed). Each request must carry an
  `auth` string matching the daemon's secret; a wrong or absent one returns
  `ok:false` "unauthorized" (the streaming verbs close the connection instead).

  The secret is resolved once at startup from the **first source that yields a
  non-empty value -- higher wins, and the lower sources are then ignored**:

  1. `-s <secret>`                         (highest)
  2. `HELIOS_SECRET` environment variable
  3. `-S <file>` / `HELIOS_SECRET_FILE`
  4. `/etc/helios/helios.json`             (default file; lowest)

  Sources 3 and 4 are a JSON file `{ "secret": "...", "allow_open": false }`,
  which may carry a leading `#` comment banner above the object. On the QEMU
  guests the init script passes the OBP secret (`eeprom helios-secret`, set
  per-boot by macXserver) as `-s`, so it sits at rank 1 -- which means a
  `/etc/helios/helios.json` on a guest is silently ignored while an eeprom secret
  is set. Physical servers have no eeprom secret, so they fall through to the
  file.

  With NO secret from any source the daemon keeps running but DENIES every
  request -- it never silently falls open, and a present-but-broken file (bad
  JSON/perms, or an explicit `-S` path that doesn't exist) also lands in
  deny-all. Serving unauthenticated requires either the `-O` flag (always) or
  `"allow_open": true` in whichever file is actually read; open then wins over
  any secret. Honest scope: a plaintext secret on a cleartext channel is a
  speed-bump, not crypto -- an HMAC/TLS upgrade is the LAN follow-up
  (HELIOS_PLAN.md B7).
- Runs as a proper daemon for the guest: `-d` double-forks and detaches, `-l`
  appends a timestamped/pid-stamped log (CxLogFile), `-P` writes a pidfile, and
  SIGTERM stops cleanly (removes the pidfile). `init/heliosAgent` is the SVR4
  init script. In dev it runs in the foreground logging to stderr.
