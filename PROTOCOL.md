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
- `read_file` / `write_file` -- base64 content. [planned]
- `list_dir` / `stat` -- directory listing + file metadata. [planned]
- `search` -- grep/find on the Sun. [planned]

Known-but-unimplemented verbs return "not implemented yet"; unrecognized verbs
return "unknown verb".

## Example

    -> { "verb": "hello", "id": 1 }
    <- { "id":1, "ok":true, "result":{ "agent":"heliosAgent", "version":"0.1.0", "protocol":1, "host":"sparcplug", "uptime":42 } }

## Notes

- Default listen port is 2125 (provisional; override with `heliosAgent <port>`).
- Binds INADDR_ANY so it's reachable through slirp inside the guest. There is no
  authentication yet -- the posture is localhost-only-via-hostfwd. Auth for the
  bare-metal-Sun case is a documented later concern (HELIOS_PLAN.md B7).
