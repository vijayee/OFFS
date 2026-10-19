# offsd Local Socket Access Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make offsd's local control socket connectable without sudo — default mode `0666` (chown/chmod after bind), configurable via `--unix-socket-mode` flag and `network["unix-socket-mode"]` config key; Windows named-pipe behavior frozen.

**Architecture:** liboffs's `unix_transport_create()` gains a trailing `unsigned socket_mode` parameter and chmods the bound socket path (POSIX-only; parameter ignored on `_WIN32`, where the pipe DACL already grants authenticated users access). offsd parses the new flag/config key and threads the value into the transport. Spec: `docs/superpowers/specs/2026-10-18-user-socket-access-design.md`.

**Tech Stack:** C, CMake, googletest (liboffs tests), cJSON, POSIX sockets.

**Two-repo note:** `unix_transport` lives in **liboffs** (`/home/victor/Workspace/src/github.com/vijayee/liboffs`); offsd/CLI live in **OFFS** (`/home/victor/Workspace/src/github.com/vijayee/OFFS`, which pins liboffs as `deps/liboffs`). Task 1 touches liboffs only; Task 2 pins OFFS's submodule to the new liboffs commit. Commits land in each repo separately. Harmony epic "offsd Local Socket Access", tickets OFFS-250…253. Never run tests or valgrind in parallel (single process at a time).

---

### Task 1: unix transport socket mode — tests, parameter, chmod (Harmony OFFS-250)

**Files:**
- Modify: `liboffs/src/ClientAPI/Unix/unix_transport.h:63` (signature)
- Modify: `liboffs/src/ClientAPI/Unix/unix_transport.c:91-137` (body + chmod)
- Test: `liboffs/test/test_unix_transport.cpp` (new tests + 6 call sites)
- Modify: `liboffs/test/test_offs_client.cpp:256,1242`, `liboffs/test/test_offs_client_integration.cpp:152`, `liboffs/test/test_large_file_upload.cpp:225`, `liboffs/examples/off_server/main.c:324` (call sites)
- Modify: `OFFS/src/offsd/main.c:1220` (call site — temporary `0666`, replaced in Task 2)

- [ ] **Step 1: Start the harmony ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket start OFFS-250
```

- [ ] **Step 2: Write the failing tests** (append at the end of `liboffs/test/test_unix_transport.cpp`)

The harness class `TestUnixTransport` ( SetUp creates a transport on `socket_path` with mode `0`) already exists; add:

```cpp
#ifndef _WIN32
#include <sys/stat.h>
#endif

/* POSIX file modes only: on Windows the socket path maps to a named pipe
 * whose DACL (platform_local.c) governs access — no st_mode to assert. */
#ifndef _WIN32
TEST_F(TestUnixTransport, ZeroModeMeansWorldAccessibleDefault) {
    struct stat st;
    ASSERT_EQ(stat(socket_path, &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0666u);
}

TEST_F(TestUnixTransport, ExplicitSocketModeAppliesToSocketFile) {
    char mode_path[128];
    _make_socket_path(mode_path, sizeof(mode_path), "mode");
    platform_local_cleanup(mode_path);

    tuple_cache_t* mode_tc = tuple_cache_create(100, pool);
    unix_transport_t* mode_transport = unix_transport_create(
        pool, bc, ofd_cache, mode_tc, mode_path, NULL, NULL, 0640);
    ASSERT_NE(mode_transport, nullptr);
    unix_transport_start(mode_transport);

    struct stat st;
    ASSERT_EQ(stat(mode_path, &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);

    unix_transport_stop(mode_transport);
    tuple_cache_destroy(mode_tc);
    platform_local_cleanup(mode_path);
}
#endif
```

- [ ] **Step 3: Verify the new tests fail (compile error is the failure)**

```bash
cd liboffs && cmake -S test -B build-test && cmake --build build-test -j$(nproc) --target testliboffs
```
Expected: FAIL — `unix_transport_create` called with 7 args (`socket_mode` missing). This is the red state.

- [ ] **Step 4: Update the signature** in `liboffs/src/ClientAPI/Unix/unix_transport.h:63`:

```c
unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                         block_cache_t* bc,
                                         ofd_cache_t* ofd_cache,
                                         tuple_cache_t* tc,
                                         const char* socket_path,
                                         const char* api_key_hash,
                                         health_context_t* health_ctx,
                                         unsigned socket_mode);
```

- [ ] **Step 5: Apply the chmod** in `liboffs/src/ClientAPI/Unix/unix_transport.c`. Ensure the file includes `<sys/stat.h>` and `<errno.h>` in the POSIX include block (add `#include <sys/stat.h>` under an existing `#ifndef _WIN32` guard if absent). Then, immediately after the `platform_local_listen` NULL-check block and before `platform_socket_set_nonblocking`:

```c
#ifndef _WIN32
  /* The socket inherits the process umask at bind (root umask -> 0755),
   * locking out non-root clients. Publish the requested mode on the
   * bound inode; a chmod failure keeps the daemon running with the
   * umask-derived mode rather than failing startup. */
  unsigned effective_socket_mode = (socket_mode == 0) ? 0666 : socket_mode;
  if (chmod(socket_path, (mode_t)effective_socket_mode) != 0) {
    log_warn("unix_transport_create: chmod(%s, %04o) failed (errno=%d); "
             "socket keeps its umask-derived mode",
             socket_path, effective_socket_mode, errno);
  }
#else
  (void)socket_mode; /* named-pipe DACL (see platform_local.c) governs access */
#endif
```

Also update the definition signature at `unix_transport.c:91` to match the header (append `, unsigned socket_mode`).

- [ ] **Step 6: Update every remaining call site** (append `, 0` as the last argument — `0` means "default 0666"):

  - `liboffs/test/test_unix_transport.cpp:151` (SetUp), `:603`, `:696`, `:963`, `:1170`, `:1330`
  - `liboffs/test/test_offs_client.cpp:256`, `:1242`
  - `liboffs/test/test_offs_client_integration.cpp:152`
  - `liboffs/test/test_large_file_upload.cpp:225`
  - `liboffs/examples/off_server/main.c:324`
  - `OFFS/src/offsd/main.c:1220` — pass `0666` (temporary; Task 2 replaces it with the parsed value)

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cd liboffs && cmake --build build-test -j$(nproc) --target testliboffs && ./build-test/testliboffs --gtest_filter='TestUnixTransport.*'
```
Expected: PASS — all `TestUnixTransport.*`, including the two new tests. Do not run other suites concurrently.

- [ ] **Step 8: Commit in liboffs, then commit the OFFS call-site patch**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
git add src/ClientAPI/Unix/unix_transport.h src/ClientAPI/Unix/unix_transport.c test/test_unix_transport.cpp test/test_offs_client.cpp test/test_offs_client_integration.cpp test/test_large_file_upload.cpp examples/off_server/main.c
git commit -m "feat(unix): configurable local socket permission mode applied after bind"
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offsd/main.c
git commit -m "chore(offsd): adapt unix_transport_create call site for socket mode parameter"
```

- [ ] **Step 9: Close the harmony ticket with the unit-test part noted**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H comment add OFFS-250 "unix_transport_create gained 'unsigned socket_mode'; chmod applied after bind (POSIX-only, warn-not-fail); 0 maps to 0666. Windows branch ignores the parameter. Unit tests: ZeroModeMeansWorldAccessibleDefault, ExplicitSocketModeAppliesToSocketFile. valgrind + e2e tracked in OFFS-252/OFFS-253."
$H ticket close OFFS-250 "Transport socket mode implemented and unit-tested."
```

---

### Task 2: offsd `--unix-socket-mode` flag + config key (Harmony OFFS-251)

**Files:**
- Modify: `OFFS/src/offsd/main.c` — args struct (~line 200), usage text (~line 240), config parse (~line 338), flag parse (~line 527), create call (~line 1220)
- Modify: `OFFS/deps/liboffs` submodule (pin to local liboffs master)
- Modify: `OFFS/src/offs/commands/config.c:52` (field reference)

- [ ] **Step 1: Start the harmony ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket start OFFS-251
```

- [ ] **Step 2: Pin deps/liboffs to local liboffs master**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git -C deps/liboffs fetch /home/victor/Workspace/src/github.com/vijayee/liboffs master && git -C deps/liboffs checkout FETCH_HEAD
git -C deps/liboffs submodule update --init --recursive || true
cmake -S . -B build-deb && cmake --build build-deb -j$(nproc)
```
Expected: configure OK; build FAILS only if the `:1220` call site was missed in Task 1 — fix it here (append `, 0666`) and amend nothing; fold the fix into this task's commit.

- [ ] **Step 3: Add the args field** to `offsd_args_t` (after `size_t max_capacity_bytes;` at ~line 198):

```c
  unsigned    unix_socket_mode;  // 0 = default 0666; parsed octal in [0600, 0777]
```

- [ ] **Step 4: Add the usage line** right after the `--unix` help line (~line 226):

```c
  fprintf(stderr, "  --unix-socket-mode <mode>  Local socket permissions as 3-4 octal\n"
                  "                       digits (POSIX only; default: 0666; e.g. 0660 restricts\n"
                  "                       to group users, 0600 to the daemon's user)\n");
```

- [ ] **Step 5: Add the config-file parse** in the `[network]` section of `_parse_config_file`, immediately after the `max-capacity-bytes` block (~line 341), following the surrounding `cJSON_Delete(root); return -1;` error style:

```c
    cJSON* unix_mode = cJSON_GetObjectItem(network, "unix-socket-mode");
    if (cJSON_IsString(unix_mode) && args->unix_socket_mode == 0) {
      char* end = NULL;
      unsigned long long parsed_mode = strtoull(unix_mode->valuestring, &end, 8);
      if (end == NULL || *end != '\0' || parsed_mode > 0777) {
        fprintf(stderr, "Invalid network[unix-socket-mode]: '%s' (octal string like \"0666\", range 0600-0777)\n",
                unix_mode->valuestring);
        cJSON_Delete(root);
        return -1;
      }
      args->unix_socket_mode = (unsigned)parsed_mode;
    }
```

- [ ] **Step 6: Add the flag parse** in `_parse_args`, right after the `--max-capacity-bytes` branch (~line 529):

```c
    } else if (strcmp(argv[i], "--unix-socket-mode") == 0 && i + 1 < argc) {
      char* end = NULL;
      unsigned long long parsed_mode = strtoull(argv[++i], &end, 8);
      if (end == NULL || *end != '\0' || parsed_mode > 0777) {
        fprintf(stderr, "Invalid --unix-socket-mode '%s' (3-4 octal digits, e.g. 0666, range 0600-0777)\n",
                argv[i]);
        return -1;
      }
      args->unix_socket_mode = (unsigned)parsed_mode;
    }
```

(Flag parse runs before the config-file parse inside `_parse_args`, but the config parse only applies when `args->unix_socket_mode == 0`, so an explicit flag — including an explicitly rejected-value check — always wins, matching the `--max-capacity-bytes` precedence comment at main.c:804.)

- [ ] **Step 7: Thread the value into the transport** at the create call (~line 1220):

```c
    server->unix_transport = unix_transport_create(
        server->pool, server->block_cache, server->ofd_cache,
        server->tuple_cache, args->unix_path, NULL,
        &server->health_ctx, args->unix_socket_mode);
```

- [ ] **Step 8: Update the CLI config field reference** in `OFFS/src/offs/commands/config.c`, after the `max_capacity_bytes` entry (~line 54), matching the surrounding string-literal style:

```c
    "    unix_socket_mode      Local unix-socket permission mode as an octal\n"
    "                          string like \"0666\" (POSIX only; default \"0666\")\n"
```

- [ ] **Step 9: Verify — invalid flags refuse, valid mode lands on the socket**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS && cmake --build build-deb -j$(nproc)
./build-deb/offsd --unix-socket-mode 0999 2>&1; echo "exit: $?"
./build-deb/offsd --unix-socket-mode 1000 2>&1; echo "exit: $?"
```
Expected: both refuse with the "Invalid --unix-socket-mode" message and nonzero exit.

```bash
mkdir -p /tmp/offsmode-test/cfg /tmp/offsmode-test/cache
timeout 6 ./build-deb/offsd --foreground --config-dir /tmp/offsmode-test/cfg \
  --cache-dir /tmp/offsmode-test/cache --port 0 --quic-port 0 \
  --unix /tmp/offsmode-test/sock --unix-socket-mode 0640 >/dev/null 2>&1 &
sleep 3; stat -c '%a' /tmp/offsmode-test/sock; kill %1 2>/dev/null
rm -rf /tmp/offsmode-test
```
Expected: `640`. Note: `--port 0` and `--quic-port 0` disable the network listeners so the test stays unprivileged (help line 220 confirms "0 to disable"); if QUIC still warns, that is fine — the socket stat is what matters.

- [ ] **Step 10: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offsd/main.c src/offs/commands/config.c deps/liboffs
git commit -m "feat(offsd): unix-socket-mode flag and config key for local socket access"
```

- [ ] **Step 11: Close the harmony ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket close OFFS-251 "Flag + network[unix-socket-mode] config key, octal-validated (0600-0777), flag-overrides-config, threaded into unix_transport_create; help + config reference updated; invalid values refuse startup."
```

---

### Task 3: valgrind on the touched suite (Harmony OFFS-252)

**Files:**
- No source changes; verification only. The two new unit tests from Task 1 are the valgrind targets.

- [ ] **Step 1: Start the harmony ticket (if not already)**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket start OFFS-252
```

- [ ] **Step 2: Run valgrind on the socket-mode tests** (one process at a time)

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs
valgrind --leak-check=full --error-exitcode=99 --show-leak-kinds=definite,indirect ./build-test/testliboffs --gtest_filter='TestUnixTransport.*'
```
Expected: no errors, "All heap blocks were freed", exit 0. If valgrind crashes on DWARF5 info, rebuild with `-DCMAKE_C_FLAGS="-gdwarf-4" -DCMAKE_CXX_FLAGS="-gdwarf-4"` and rerun (known local tooling quirk).

- [ ] **Step 3: Comment and close the harmony ticket** (the non-root e2e runs as part of Task 4 packaging verification, which is what OFFS-253 covers — note that in the comment)

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H comment add OFFS-252 "valgrind clean on TestUnixTransport.* including both new socket-mode tests. Non-root e2e deferred to OFFS-253 packaging verification (requires a root systemd daemon)."
$H ticket close OFFS-252 "valgrind: 0 leaks / 0 errors."
```

---

### Task 4: rebuild deb, reinstall, verify unprivileged CLI (Harmony OFFS-253)

**Files:** none modified — packaging + live verification. Steps marked **[sudo — user]** cannot be run by the executing agent; hand the exact command to the human operator.

- [ ] **Step 1: Start the harmony ticket**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket start OFFS-253
```

- [ ] **Step 2: Rebuild the package**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS/build-deb && cmake --build . -j$(nproc) && cpack -G DEB 2>&1 | tail -3 && ls -lh offsd-*.deb
```
Expected: `offsd-0.1.1-Linux.deb` regenerated.

- [ ] **Step 3: [sudo — user] Reinstall and start**

```bash
sudo dpkg -i /home/victor/Workspace/src/github.com/vijayee/OFFS/build-deb/offsd-0.1.1-Linux.deb
sudo systemctl start offsd
```

- [ ] **Step 4: Verify the socket mode and unprivileged CLI (no sudo)**

```bash
ls -l /var/run/offs.sock        # expected: srw-rw-rw- (0666), root root
offs cache size                 # expected: live response from the daemon, no sudo
```
Expected: `offs cache size` succeeds as the normal user (before this change it printed "Cannot connect to daemon").

- [ ] **Step 5: Verify stale-socket self-correction** — chmod the socket down, restart, confirm it is re-widened on bind:

```bash
sudo chmod 0600 /var/run/offs.sock
sudo systemctl restart offsd
ls -l /var/run/offs.sock        # expected: 0666 again
```

- [ ] **Step 6: [sudo — user] Stop the daemon if the operator wants it idle** — otherwise leave it running.

- [ ] **Step 7: Close the harmony ticket, emit, and clear status**

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H ticket close OFFS-253 "Deb rebuilt/reinstalled; /var/run/offs.sock is 0666; 'offs cache size' works without sudo; stale-perm self-correction verified across restart."
$H emit clear
```

Then run the de-wonk audit (per CLAUDE.md) over both repos' touched files before declaring the epic done:

```bash
H=/home/victor/.claude/skills/harmony/harmony
$H epic update "offsd Local Socket Access" --status done
```