# Security

GRD 0.2 is intended exclusively for trusted local networks.

To report a vulnerability, do not publish credentials, configuration files,
or network captures containing personal data in a public issue. Preserve a
minimal reproduction and include the operating system, version, and steps.

The local configuration stores an Argon2id salt and verifier with `0600`
permissions on macOS and Linux. It never stores the plaintext password.
Sessions use ephemeral X25519 keys and XChaCha20-Poly1305 with nonces derived
from independent monotonic counters for each direction.

The protocol and handshake have not yet received an independent security
audit and must not be exposed to the Internet.

The terminal/SFTP integration does not reuse the GRD password. It advertises
an already configured OpenSSH service on the LAN only after verifying its
local banner; authentication, private keys, and host-key verification remain
inside the operating system's `ssh` and `sftp` clients. Enabling this feature
does not modify firewall rules, allowed users, or `sshd` configuration. The
administrator must restrict SSH service exposure separately.

Optional SFTP/PowerShell access on Windows also does not use the GRD password.
It uses an isolated OpenSSH service, standard Windows accounts, and individual
ED25519 keys. Controller and observer roles do not automatically receive file
or shell access. PowerShell over SSH preserves every privilege of the account
and does not support JEA, so the profile is disabled by default and the script
rejects administrator accounts. The explicit
`-AllowPrivilegedPowerShellAccounts` exception is limited to the PowerShell
profile, must be repeated for every apply/audit operation, and grants all
effective rights of the account. Use it only when deliberately accepting that
risk. This port must also remain confined to the LAN.
