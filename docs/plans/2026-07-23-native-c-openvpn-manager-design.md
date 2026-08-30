# Native C OpenVPN Manager

## Status

Approved for implementation. This document defines the first usable version and deliberately excludes NetworkManager, Python, and a large endpoint dropdown.

## Goals

- Provide a small dedicated Ubuntu desktop GUI for direct OpenVPN process management.
- Show the window immediately and keep GTK's main loop responsive during scanning, connecting, and disconnecting.
- Search and select from large directories of `.ovpn` files without constructing an unbounded popup.
- Map endpoint hostnames to named credential sets so many endpoints can share one account.
- Avoid recurring sudo or VPN credential prompts during normal use.
- Keep privileged operations and secret handling narrowly scoped and auditable.
- Make profile parsing, hostname mapping, filtering, helper validation, and process transitions testable without a live VPN.

## Non-goals

- No NetworkManager integration or dependency.
- No support for arbitrary OpenVPN command-line options supplied by the GUI.
- No in-app editor for OpenVPN profiles or passwords in the first version.
- No automatic credential discovery from arbitrary files or shell commands.
- No simultaneous VPN connections.
- No attempt to support non-Ubuntu platforms.

## Architecture

The project builds one unprivileged GTK executable. At Connect time it launches the system OpenVPN CLI through the desktop authorization prompt:

```text
openvpn-manager (unprivileged GTK process)
    |  one authorization prompt at Connect
    v
pkexec /usr/sbin/openvpn --config <trusted profile>
    |  --management-client
    v
private per-session Unix management socket owned by the GUI user
```

The GUI owns presentation, directory scanning, profile parsing, filtering, and state display. It never runs OpenVPN as root itself and never places credentials in a command generated from user text. OpenVPN itself owns the VPN lifecycle. The GUI uses OpenVPN's standard management-client behavior: if the private socket closes, OpenVPN receives its documented termination behavior and exits.

There is no runtime sudoers rule, persistent root daemon, custom helper binary, or password-cache dependency. `pkexec` is invoked only for the normal OpenVPN CLI. This is intentionally a user-authorized desktop tool, not a general privilege broker; profiles must be administrator-trusted system files.

## Configuration and data model

### Profile directory

The default directory is `/etc/openvpn/ovpn_udp`. The app accepts an optional profile directory argument for tests and installations with a different layout, but production profiles must be administrator-trusted files below `/etc/openvpn`. It scans only regular files whose names end in `.ovpn`, does not follow symlinks, and stores each profile as a relative filename plus its canonical directory-derived identity. Before Connect, the selected profile and every configured auth file must be regular files with root ownership and no group/world write permission; user-writable profile trees are rejected for privileged execution.

A profile record contains:

- display filename
- safe relative profile identifier
- the first usable `remote` hostname (and optional port/protocol for display)
- mapped credential-set label, or an explicit unmapped marker

The parser accepts OpenVPN whitespace and comments, recognizes `remote <host> [port] [proto]`, and treats malformed or missing remotes as a visible parse error rather than silently guessing. The first version requires exactly one `remote` directive and rejects profiles containing daemonization, management, plugin, script/hook, or nested `config` directives. This keeps process ownership predictable and prevents a selected profile from adding an external root command. Large files and lines are bounded and produce a visible parse error.

### User configuration

The app reads an INI-style file at `$XDG_CONFIG_HOME/openvpn-manager/config.ini`, defaulting to `~/.config/openvpn-manager/config.ini`:

```ini
[manager]
profile-directory=/etc/openvpn/ovpn_udp

[credential "nordvpn"]
hostname-regex=\\(^|\\.)nordvpn\\.com$
auth-file=/etc/openvpn/credentials/nordvpn.auth

[credential "work"]
hostname-regex=\\(^|\\.)vpn.example.org$
auth-file=/etc/openvpn/credentials/work.auth
```

Rules are evaluated in file order. A hostname must match exactly one rule; multiple matches are reported as an ambiguity and cannot connect. No-match profiles remain selectable but Connect is disabled with a useful explanation. Credential labels and auth-file paths are configuration data, not free-form helper command arguments. The setup tool validates that configured auth files are regular files owned by root with mode `0600` before installation/use.

The app writes no passwords. A documented setup command creates the root-owned auth files from an interactive terminal or the user may create them manually in OpenVPN's two-line `username`/`password` format. Passwords are never logged, displayed, or passed as command-line arguments.

## GUI and responsiveness

The first GTK frame is built before any directory or process work. The main window contains:

- a search entry
- a status label
- a bounded `GtkListBox` inside a scrolled window, capped at 100 visible result rows
- Connect and Disconnect buttons
- a small progress/status indicator

Rows show filename, remote hostname, and credential mapping status. Filtering is case-insensitive across filename and hostname and runs against the complete in-memory record set, but only the first 100 matching rows are materialized. This avoids the giant `GtkComboBoxText`/native-window failure from the previous implementation.

Directory enumeration and profile parsing run in a GLib worker thread. Results are transferred to the GTK thread using `g_idle_add()`/`GTask` completion callbacks. The worker checks cancellation between files; a newer scan invalidates the prior generation. All GTK objects are touched only on the main thread.

OpenVPN output is consumed asynchronously through GLib child-watch and nonblocking I/O callbacks. The GUI shows concise sanitized lines and an actionable failure status, never raw credentials. Waiting for a process, reading a pipe, filesystem traversal, and helper invocation must not occur synchronously in a GTK callback.

Connect is disabled while connecting or disconnecting and while no mapped profile is selected. Selecting another profile while connected does not silently change the active connection. Disconnect is available during connecting and connected states.

## OpenVPN process and management boundary

The GUI creates a fresh Unix-domain listening socket below a private `0700` directory in `$XDG_RUNTIME_DIR`, then launches the ordinary command:

```text
pkexec /usr/sbin/openvpn --config PROFILE --auth-user-pass AUTH \
  --auth-nocache --management-client --management SOCKET \
  --management-signal --verb 3
```

Arguments are passed as an argv array, never through a shell. The app passes only files selected from the configured profile/auth roots and refuses unsafe metadata before launching. The profile parser rejects daemonization, management, plugin, script/hook, and nested config directives; production profiles remain administrator-trusted because OpenVPN configuration is executable behavior.

The management socket is `0600` and randomized per connection. OpenVPN connects to it as the authenticated root process. The app continuously drains OpenVPN stdout/stderr with bounded buffers and reads management events asynchronously. On Disconnect, window close, or app shutdown, it closes the management connection; `--management-client` causes OpenVPN to terminate. The app then waits asynchronously for the `GSubprocess` exit with a monotonic bounded shutdown deadline. It never sends signals to a root PID from the unprivileged process.

A private lock prevents two instances of this manager from claiming the same session directory. It does not claim to prevent unrelated OpenVPN or NetworkManager connections. No password is placed in argv or logs; only the auth-file pathname is passed to OpenVPN. Polkit authorization behavior is delegated to the system `pkexec` policy and may be cached by the desktop session; the app does not promise a fixed prompt frequency.

## Process state machine

The GUI states are:

- `IDLE`: no managed connection
- `STARTING`: helper accepted a start request
- `CONNECTED`: OpenVPN has reached a successful connection indication
- `STOPPING`: stop requested, awaiting helper/process completion
- `ERROR`: last operation failed; a new Connect may retry

Only explicit helper results and recognized asynchronous OpenVPN lifecycle events may advance state. A failed start returns to `ERROR`; a completed stop returns to `IDLE`; helper disappearance is treated as `ERROR` and controls are restored. Window close requests Disconnect and delays exit until the child/helper is stopped or a bounded shutdown timeout is reached.

## Build and installation

Use a dependency-free Makefile around `pkg-config --cflags --libs gtk+-3.0 gio-2.0 gio-unix-2.0`. Compile with warnings enabled and hardening flags (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -D_FORTIFY_SOURCE=2`, PIE/RELRO where supported). Do not add a new library dependency.

Targets:

- `make`: build the GUI and unit-test binary
- `make test`: build and run unit tests
- `make install-user`: install the GUI and desktop entry for the current user

The repository must include a README covering dependencies, profile trust requirements, credential file creation, configuration examples, and removal. No installer modifies sudoers, creates a service, installs a root helper, or silently creates credentials.

## Testing strategy

Unit tests run without GTK display access or root privileges where possible:

- parse valid/invalid `remote` directives, comments, whitespace, host forms, and missing remotes
- apply ordered hostname regex rules, no-match, and ambiguity behavior
- filter records case-insensitively and enforce the 100-row materialization bound
- accept/reject helper commands, identifiers, path traversal, bad arguments, and unsafe file metadata
- exercise process state transitions for start success, start failure, stop success, timeout, helper exit, and repeated commands

Integration/manual checks document:

- opening the app with a large endpoint directory and seeing the window immediately
- search responsiveness and absence of oversized popups
- connecting and disconnecting using a test OpenVPN configuration/helper stub
- GUI close while connecting/connected
- wrong credentials, missing credential mappings, malformed profiles, and helper permission failures

Tests must not require a real VPN account. A fake OpenVPN executable or injected process-launch abstraction will be used for lifecycle tests rather than changing production security checks.

## Implementation sequence

1. Add build files, core data types, parser, hostname mapping, bounded filtering, and unit tests.
2. Add the GTK window, asynchronous scan worker, bounded result list, and UI state handling.
3. Add the helper protocol, secure validation functions, process-group lifecycle, and helper tests using temporary fixtures.
4. Add asynchronous GUI/helper integration, status/error handling, and shutdown behavior.
5. Add setup/install scripts, sample configuration, desktop entry, and README security/setup documentation.
6. Build, run the complete test suite, compile with strict warnings, manually exercise the large-directory and stubbed-process paths, and perform a static security/responsiveness review.

## Acceptance criteria

- `make test` passes without root, a display, NetworkManager, or a live VPN.
- The first window is displayed before scanning starts, and scanning/connecting/disconnecting never blocks the GTK main loop.
- A large profile directory remains usable through search and a bounded result list.
- A profile cannot start without an unambiguous hostname-to-credential mapping.
- No password is present in process arguments, logs, GUI status, or repository fixtures.
- Connect displays the desktop authorization prompt and starts the restricted helper; Disconnect requires no second authorization prompt because the helper owns the active OpenVPN process through its control pipe.
- The app rejects unsafe profile/auth files and profiles containing unsupported process/script directives before privileged launch.
- Disconnect and window close close the management-client socket and observe OpenVPN exit asynchronously within a bounded timeout.
- Documentation explains the one-time privileged setup and its security trade-offs.
