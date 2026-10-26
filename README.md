# askpass: per-request sudo approval for Claude Code

> [!CAUTION]
> LLM generated code. Not a security tool. Use at your own risk.

Two small C programs that let Claude Code (for example, the VS Code extension
over Remote-SSH) run `sudo` on a headless Linux machine. **Every invocation
must be approved by you**, from one of your own terminals.

- **`askpass-agentic`** is a `SUDO_ASKPASS` helper. When sudo needs a password, it
  creates a one-shot request and broadcasts a notice to all of your terminals.
- **`askpass-confirm`** shows a pending request and prompts for your sudo
  password. It then hands the password to the waiting sudo, or denies it.

When Claude runs `sudo -A whoami`, every terminal you have open shows:

```
Claude for VSCode requesting sudo permission to execute:
> sudo -A whoami
Run askpass-confirm --request 012345678 to approve the request.
```

## How it works

1. Claude runs `sudo -A <command>`. Sudo has no TTY, so it runs the program
   named in `SUDO_ASKPASS`.
2. `askpass-agentic` reads the exact command line from its parent (sudo) through
   `/proc/<ppid>/cmdline`. It then creates `~/.claude-sudo/<id>.cmd` and a
   FIFO, `~/.claude-sudo/<id>.fifo`, where `<id>` is a random 9-digit number.
3. `askpass-agentic` writes the notice to every terminal device you own: `/dev/pts/*`
   (ssh sessions, tmux panes, VS Code terminals) and `/dev/tty[0-9]*`.
4. You run `askpass-confirm --request <id>` in any terminal. It displays the
   command again and reads your password from `/dev/tty` with echo disabled.
5. The password travels through the FIFO to `askpass-agentic`, which prints it to
   stdout. Sudo then verifies it as usual.

`askpass-agentic` **fails closed**, which makes sudo fail, if any of these happen:

- you deny the request or submit an empty password,
- no response arrives within 120 seconds,
- the process is interrupted,
- the kill switch is on,
- any error occurs.

The request files are removed in every case.

## Requirements

- Linux (the programs rely on `/proc` and Linux FIFO semantics)
- A C compiler (`gcc` or `clang`) and `make`
- `sudo` with askpass support (any modern version)
- Claude Code with access to the remote machine

## Compilation

```sh
make
```

This builds `askpass-agentic` and `askpass-confirm` with hardening flags: FORTIFY,
stack protector, PIE, and full RELRO. To override the compiler or flags:

```sh
make CC=clang
make CFLAGS="-O2 -g"
```

To change the approval timeout, edit `TIMEOUT_SEC` in `askpass-agentic.c` and rebuild.

## Installation

```sh
make install                  # installs to ~/.local/bin
make install PREFIX=/opt/foo  # or to /opt/foo/bin
```

Make sure `~/.local/bin` is on your `PATH`. Add this to `~/.bashrc` if needed:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

## Setup

### 1. Disable sudo credential caching

By default, sudo remembers a successful authentication for several minutes.
That would let later commands run without approval. Turn it off for your user:

```sh
sudo visudo -f /etc/sudoers.d/claude-askpass
```

```
Defaults:youruser timestamp_timeout=0
```

### 2. Configure Claude Code

Edit `~/.claude/settings.json` on the **remote** machine, where the VS Code
extension's server runs. Use an absolute path, because `~` is not expanded.

```json
{
  "env": {
    "SUDO_ASKPASS": "/home/youruser/.local/bin/askpass-agentic"
  },
  "permissions": {
    "ask":  ["Bash(sudo:*)"],
    "deny": ["Bash(askpass-confirm:*)", "Edit(~/.local/bin/**)"]
  }
}
```

The settings entries do the following:

- **`ask` on `sudo`**: Claude Code shows you every sudo command in VS Code
  before it runs. Never choose "always allow" for these prompts.
- **`deny` on `askpass-confirm`**: Claude cannot try to approve its own
  requests.
- **`deny` on edits to `~/.local/bin`**: Claude's file tools stay away from
  the installed binaries.

### 3. Tell Claude to use `sudo -A`

Add this to `~/.claude/CLAUDE.md`:

```
When a command needs root, run it with `sudo -A`. A human approves each
request out of band; wait for it, and if it fails, report the failure
rather than retrying repeatedly.
```

Plain `sudo` without `-A` simply fails, because Claude has no TTY.

### 4. Test

From a terminal, run:

```sh
SUDO_ASKPASS="$HOME/.local/bin/askpass-agentic" setsid sudo -A whoami
```

`setsid` detaches the command from your TTY, just as Claude's commands are.
The notice should appear in your terminals. Approve it from another terminal
with `askpass-confirm --request <id>`, and the first command should print
`root`.

## Usage

When a notice appears, you can approve or deny it from any of your terminals.

| Command | Effect |
|---|---|
| `askpass-confirm --request ID` | Show the command and prompt for your password to approve it. An empty password denies it. |
| `askpass-confirm --request ID --deny` | Deny the request without a prompt. |
| `askpass-confirm --list` | List pending requests, for example if you missed the notice. |
| `askpass-confirm --disable` | Kill switch: every future request is denied immediately. |
| `askpass-confirm --enable` | Remove the kill switch. |

A typical approval looks like this:

```
$ askpass-confirm --request 012345678
Request 012345678 wants to run:
> sudo -A apt-get install -y htop
Your sudo password (empty to deny):
Approved (sudo will verify the password).
```

If you type the wrong password, sudo retries and a new request (with a new
ID) is broadcast.

`askpass-confirm` requires a controlling terminal (`/dev/tty`). It refuses to
run from processes without one, such as commands executed by Claude.

## Revoking access

The table lists the options from fastest to most thorough.

| Scope | How |
|---|---|
| One request | `askpass-confirm --request ID --deny`, or ignore it until it times out |
| Temporarily, for everything | `askpass-confirm --disable` |
| At the Claude Code level | Move `Bash(sudo:*)` from `ask` to `deny` in `settings.json` |
| Permanently | Remove `SUDO_ASKPASS` from `settings.json`, run `make uninstall`, delete `/etc/sudoers.d/claude-askpass`, and `rm -rf ~/.claude-sudo` |

## Security notes

- **Display hardening:** the command shown to you is shell-quoted, and every
  non-printable or non-ASCII byte is rendered as `\xNN`. A crafted argument
  therefore cannot inject terminal escape sequences to disguise what you are
  approving.
- **Password handling:** your password never appears in any process's
  argument list. It travels only through a mode-0600 FIFO in a mode-0700
  directory, and buffers are wiped after use.
- **State directory checks:** `~/.claude-sudo` is located through the passwd
  database rather than `$HOME`. The programs refuse to use it unless it is a
  real directory, owned by you, with no group or other permissions.
- **Notification scope:** notices go only to terminals you own. Writing to
  other users' terminals, as `wall -g` does, would require setgid-tty
  privileges.
- **Threat model:** this tool protects against *unapproved or accidental*
  privilege escalation. It does **not** defend against a deliberately
  malicious process running as your own user.
  - Such a process could replace the binaries in `~/.local/bin`.
  - It could also fake a request and capture the password you type.
  - Installing the binaries root-owned to a system location (for example,
    `sudo make install PREFIX=/opt/askpass`, with `install -o root -g root`
    in the Makefile) removes the first risk but not the second.
  - For stronger guarantees, consider sudo authentication through a forwarded
    SSH agent with a FIDO2 hardware key (`pam_rssh`).

## Troubleshooting

**No notice appears.** Check that you have a terminal open on the machine
under the same user. Run `askpass-confirm --list` to see pending requests
regardless of notices. Requests expire after 120 seconds.

**"state directory ~/.claude-sudo is missing or unsafe".** Fix the
permissions with `chmod 700 ~/.claude-sudo`. Also make sure the path is not a
symlink and is owned by you.

**"no pending request … (expired?)".** The request timed out or sudo gave up.
Have Claude rerun the command.

**Sudo still prompts on a TTY or fails with "a terminal is required".** The
command was run without `-A`, or `SUDO_ASKPASS` is not set in Claude Code's
environment. Restart the VS Code extension after editing `settings.json`.

**Approvals carry over to the next command.** The `timestamp_timeout=0`
sudoers entry is missing or names the wrong user. Check it with `sudo -l`.

## Files

| File | Purpose |
|---|---|
| `askpass-agentic.c` | The `SUDO_ASKPASS` helper: creates requests, broadcasts notices, and waits for a response |
| `askpass-confirm.c` | The approval tool |
| `askpass_common.h` | Shared helpers (state directory checks, ID validation) |
| `Makefile` | Build, install, uninstall, and clean targets |
| `~/.claude-sudo/` | Runtime state: pending requests and the `disabled` kill-switch file |
