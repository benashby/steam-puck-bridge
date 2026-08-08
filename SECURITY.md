# Security Policy

## Supported versions

This project ships from `main`. Only the latest tagged release and the current
`main` branch receive fixes.

| Version | Supported |
|---|---|
| `main` / latest tag | ✅ |
| older tags | ❌ |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Use GitHub's private reporting:
[Security → Report a vulnerability](https://github.com/benashby/steam-puck-bridge/security/advisories/new)

If that is unavailable to you, email **mail@benashby.com** with `steam-puck-bridge`
in the subject line.

Please include:

- the version / commit you are running (`steam-puck-bridge --version`),
- your distribution and kernel (`uname -a`),
- what the bug lets an attacker do,
- a reproducer if you have one.

Expect an acknowledgement within 7 days. This is a hobby project maintained by
one person, so please allow up to 90 days for a fix before public disclosure.
Credit is given in the release notes unless you ask otherwise.

## Threat model

Some context on what is and isn't in scope, because this daemon sits close to
the kernel input stack:

**In scope**

- Memory-safety bugs (out-of-bounds read/write, overflow) in the HID report
  parser. This is the highest-risk area: it parses attacker-influenceable bytes
  from a USB device into fixed-size buffers.
- Anything that lets a non-privileged local process cause the daemon to inject
  synthetic input events it should not (uinput is an input-injection primitive —
  a compromised bridge can type into whatever has focus).
- Path handling in the `/sys/class/hidraw` scan (symlink following, TOCTOU).
- The `pgrep`-style Steam-detection logic being tricked into permanently
  disabling the bridge, or the reverse.

**Out of scope**

- The daemon runs unprivileged by design. Anything that requires you to already
  be root, or to have run it as root against advice, is not a vulnerability
  in this project.
- The permissive `/dev/hidraw*` permissions shipped by Valve's `steam-devices`
  udev rules on some distros. That is upstream's decision; the
  [`udev/60-steam-puck-bridge.rules`](udev/60-steam-puck-bridge.rules) file in
  this repo uses `uaccess` instead, which is tighter.
- Physical-access attacks involving plugging in a malicious USB device that
  claims VID `28de`. See the memory-safety bullet above for the part of that we
  *do* care about.
