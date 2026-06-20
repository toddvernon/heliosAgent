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

- `hello` -- liveness handshake. **[implemented]**
  - request: `{ "verb": "hello", "id": 1 }`
  - result: `{ "agent":"heliosAgent", "version":"0.1.0", "protocol":1,
              "host":"<hostname>", "uptime":<seconds the daemon has run> }`
- `shutdown` -- graceful shutdown (`init 5`). **[implemented]**
  - request: `{ "verb": "shutdown", "id": 2 }`
  - result: `{ "status": "shutting down" }`
  - The agent ACKs first, then the server runs the shutdown command, so the
    client always learns the guest is going down before it does. The command is
    `/usr/sbin/init 5`, overridable via the `HELIOS_SHUTDOWN_CMD` env var (used
    in dev so the default doesn't shut down the developer's Mac).
- `run_command` -- run a shell command on the Sun. **[implemented]**
  - request: `{ "verb":"run_command", "id":8, "cmd":"cc hello.c -o hello",
              "cwd":"/export/home/me", "timeout_ms":60000 }`
    (`cwd` and `timeout_ms` optional; absent cwd inherits, absent/<=0 timeout
    waits indefinitely)
  - result: `{ "exit_code":N, "output":"<combined stdout+stderr>",
              "timed_out":bool }`
  - `ok:true` means the command *ran*; a nonzero exit is reported in
    `exit_code` (128+signal if killed), not as a daemon error. Only a
    missing/invalid `cmd` is `ok:false`. Output is a JSON string (escaped);
    file *content* still uses base64 (read_file/write_file).
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
              "ignore_case":false, "max":1000, "timeout_ms":0 }`
    (`path` default ".", `max` default 1000; both `ignore_case` and `timeout_ms`
    optional)
  - result: `{ pattern, path, count, truncated, exit_code, timed_out,
              matches:[ { file, line, text }, ... ] }`
  - Shells native grep (`grep -rHn`, run where the files are), shell-quoting
    the pattern and path so metacharacters can't inject, and discarding grep's
    stderr so error text is never mis-parsed as a match. No-match (grep exit 1)
    is `ok:true` with `count:0`; only a missing `pattern` is `ok:false`.
    `truncated:true` means `max` was hit (matches are not silently dropped).
    NB: needs a grep that supports `-rHn -e` -- on Solaris that means GNU or
    xpg4 grep (see HELIOS_PLAN.md A4), not stock `/usr/bin/grep`.

All eight v1 verbs are implemented. Unrecognized verbs return "unknown verb";
malformed requests return a protocol error. Both are `ok:false`, never a
dropped connection.

## Example

    -> { "verb": "hello", "id": 1 }
    <- { "id":1, "ok":true, "result":{ "agent":"heliosAgent", "version":"0.1.0", "protocol":1, "host":"sparcplug", "uptime":42 } }

## Notes

- Default listen port is 2125 (provisional; override with `heliosAgent <port>`).
- Binds INADDR_ANY so it's reachable through slirp inside the guest. There is no
  authentication yet -- the posture is localhost-only-via-hostfwd. Auth for the
  bare-metal-Sun case is a documented later concern (HELIOS_PLAN.md B7).
