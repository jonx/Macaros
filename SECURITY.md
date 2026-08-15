# Security

This is an experimental hobby OS port, not production software. Keep that in
mind before exposing it to anything you care about:

- `bsdsocket.library` forwards guest network calls to real host sockets; an
  AROS program has the network access of the host process.
- The host-volume handler gives the guest read/write access to the Mac
  folders you mount into it.
- `run68k` executes 68k binaries in-process with no sandbox; only run
  binaries you trust.

To report a vulnerability (for example a guest escape that reaches host
memory or files outside the configured mounts), email **code@jkn.me** rather
than opening a public issue. Expect a best-effort hobby-project response
time.
