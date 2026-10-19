# offsd Local Socket Access — Design

**Date:** 2026-10-18
**Status:** Approved (brainstorm, approach 1 + Windows amendment)
**Repos:** OFFS (offsd CLI/daemon), liboffs (ClientAPI/Unix transport, Platform layer)

## Problem

offsd's local control socket is hard-coded to `/var/run/offs.sock`
(`src/Service/local_socket.h:17`; offsd defaults `--unix` to it at
`src/offsd/main.c:581`). The socket is created with the daemon process's
umask — root under systemd → mode 0755 — so only root can connect. Every
CLI command that bridges to the daemon (`put`, `get`, `peer`, `config`,
`pin`, …) therefore requires sudo, and any third-party client built on
the socket would too. `offs start` as a normal user fails outright: the
daemon's bind is denied, and the CLI's 3-second probe on the socket
times out ("Daemon failed to start (no response at /var/run/offs.sock
within 3s)").

On Windows the equivalent pipe already grants access to SYSTEM,
Administrators, and Authenticated Users
(`src/Platform/platform_local.c:249-261`, SDDL
`"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)"`), so Windows clients connect
without elevation today. POSIX is the outlier.

## Goal

A freshly installed offsd accepts local CLI and third-party client
connections over its unix socket **without sudo**. Socket mode defaults
to world-accessible (`0666`) and is configurable at runtime like other
daemon settings.

Decisions taken during brainstorm:

- Access model: **configurable at runtime** (knob in config + CLI flag).
- Default: **0666 world-accessible**, matching the Windows pipe status
  quo. Operators who want restriction set `0660`/`0600`.
- Implementation: **chmod after bind** (approach 1) — smallest surface,
  matches existing config/flag conventions.
- **Windows behavior is frozen**: the mode parameter is accepted
  everywhere but ignored on the Windows pipe path; no SDDL knob (YAGNI).

## Design

### 1. Config surface (OFFS, `src/offsd/main.c`)

- New CLI flag `--unix-socket-mode <octal>` (e.g. `0666`, `0660`,
  `0600`), parsed with `strtoull(..., 8)`; values > `0777` rejected with
  a usage error.
- New config key `network["unix-socket-mode"]` — a JSON **string in
  octal notation** (e.g. `"0666"`), stored and parsed as a string to keep
  the leading-zero octal notation unambiguous; parsed alongside
  `max-capacity-bytes`; the CLI flag overrides the config file, following
  the established precedence.
- Default when unset: `0666`.
- Takes effect at daemon start / restart, consistent with every other
  config key (the `POST /config/restart` flow already exists).

### 2. Socket mode application (liboffs, `ClientAPI/Unix/unix_transport.c`)

- `unix_transport_create()` gains a trailing `unsigned socket_mode`
  parameter, threaded from offsd (single call site at
  `src/offsd/main.c:1220`).
- Immediately after `platform_local_listen()` succeeds, chmod the
  socket path. This fixes the underlying defect: the socket currently
  inherits the process umask, making it unconnectable by ordinary users.
- chmod failure logs a warning and continues — a permission tweak must
  never prevent the daemon from running.
- Guarded out on `_WIN32`: the path maps to a named pipe there and POSIX
  file modes do not apply. The pipe DACL is untouched.

### 3. What stays as-is

- Socket path and the `--unix <path>` flag — unchanged; the mode
  combines with any path.
- HTTP/QUIC transports — unchanged; they keep bearer auth and their own
  bindings.
- No packaging group, no ACL machinery, no tmpfiles.d rule.

## Error handling

| Condition | Behavior |
|---|---|
| Invalid `--unix-socket-mode` flag value | Parse error + usage hint; refuse to start |
| Invalid `unix-socket-mode` in config | Config parse error; refuse to start |
| `chmod` fails after bind | Log warning; daemon runs with umask-derived mode |

## Security note

A mode of `0666` means any local user gets full daemon control (put/get,
peer management, config changes) — the same capability set the Windows
pipe grants to authenticated users today. Documented in `--help` for the
flag ("POSIX only; 0666 default") and in the config field reference.

## Testing

- Unit test: start the unix transport, assert the socket file's
  `st_mode` equals the requested mode; assert the default is `0666`.
- End-to-end: daemon started via systemd (root); connect and issue a
  command as a non-root user with no sudo.
- valgrind on the touched suite (no new leaks), per project convention.
- Windows-only regression watch: mode parameter must compile and be
  ignored on `_WIN32`; pipe DACL untouched.

## Rollout

No migration step in the deb: existing installs pick up `0666` on their
next daemon start. chmod runs on every bind, so stale socket permissions
from earlier versions self-correct.