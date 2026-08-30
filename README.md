# OpenVPN Manager

A small native C/GTK desktop frontend for direct OpenVPN connections. It does not use NetworkManager.

Written to allow the author to easily use NordVPN from Ubuntu without using the official NordVPN client. Uses lightweight/naive approached by design.

## What it does

- Displays the window immediately, then scans profiles in a worker thread.
- Searches endpoint filenames and remote hostnames asynchronously.
- Materializes at most 100 result rows.
- Starts the normal `/usr/sbin/openvpn` command through `pkexec` when Connect is pressed.
- Uses OpenVPN's management-client Unix socket for asynchronous state updates and clean Disconnect.
- Maps hostnames such as `uk2242.nordvpn.com` to shared credential files.

The GUI never waits synchronously for OpenVPN, reads a process pipe synchronously, or scans the profile directory on GTK's main loop.

## Build

On Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev openvpn policykit-1
make
make test
```

No installation is needed to run a build in the project directory:

```sh
./openvpn-manager
```

To install for the current user:

```sh
make install-user
```

This installs the application under `~/.local/bin` and its desktop file under `~/.local/share/applications`. It does not modify sudoers, install a daemon, or install a privileged helper.

## Configuration

Create `~/.config/openvpn-manager/config.ini`, starting from `config.example.ini`:

```ini
[manager]
profile-directory=/etc/openvpn/ovpn_udp

[credential.nordvpn]
hostname-regex=(^|\.)nordvpn\.com$
auth-file=/etc/openvpn/credentials/nordvpn.auth
```

Rules are evaluated in file order. A hostname must match exactly one rule. Profiles with no match or multiple matches are shown but cannot connect. After a successful connection, the selected profile path is remembered in `~/.config/openvpn-manager/state.ini` and selected automatically on the next launch.

Create each auth file as a root-owned file readable only by root, for example:

```sh
sudo install -d -o root -g root -m 700 /etc/openvpn/credentials
sudo sh -c 'umask 077; printf "%s\\n%s\\n" "VPN_USERNAME" "VPN_PASSWORD" > /etc/openvpn/credentials/nordvpn.auth'
sudo chown root:root /etc/openvpn/credentials/nordvpn.auth
sudo chmod 600 /etc/openvpn/credentials/nordvpn.auth
```

Replace the example values. Passwords are not stored by the application, put in process arguments, or written to logs. The auth file is plaintext at rest, protected by filesystem permissions.

## Security and profile requirements

Connect uses the desktop `pkexec` authorization prompt to run the system OpenVPN binary as root. The application does not install a sudoers rule and cannot guarantee whether the desktop policy asks every time or caches authorization.

OpenVPN configuration is executable behavior: directives can invoke scripts, plugins, or nested configuration. For that reason, production profiles must be administrator-trusted and live in a root-owned, non-user-writable directory below `/etc/openvpn`. The application rejects common process/script directives (`daemon`, `management`, `plugin`, `config`, hooks, and related output/path directives) so it can retain process ownership. It requires one `remote` directive per profile.

The app passes only selected profile and auth-file paths to OpenVPN using an argument vector. It never invokes a shell. It checks both files and their parent directories for root ownership and group/world non-writability immediately before launch. A path race after that check is outside the capabilities of an unprivileged frontend; keep the profile tree administrator-owned.

The management socket is a random `0600` Unix socket in the user's `XDG_RUNTIME_DIR`. OpenVPN runs with `--management-client`; closing the socket on Disconnect, application close, or application crash tells OpenVPN to terminate. The GUI observes exit asynchronously and does not signal a root PID.

## Limitations

- This is for a single desktop session and one manager-owned connection at a time.
- It does not prevent unrelated OpenVPN or NetworkManager connections.
- A system polkit authentication agent is required for the graphical authorization prompt.
- Unsupported profile directives are reported in the list instead of being silently ignored.
- The first scan caps the number of profiles at 50,000 and profile size at 1 MiB to keep memory bounded.
