# Optional SFTP and PowerShell access on a Windows host

This feature is deliberately separate from the GRD transport and password.
GRD's shared password proves that a peer may join a desktop session, but it
does not identify a person well enough to grant filesystem or shell access.
SFTP and PowerShell therefore use a dedicated OpenSSH service, individual
Windows accounts and one ED25519 public key per account.

Nothing is enabled by building or launching GRD. An administrator must review
a local policy and explicitly apply it.

```text
                       trusted LAN only

 GRD client role                         Windows account + ED25519 key
 controller / observer                             |
          |                                        v
          |                               TCP 47992, Private profile,
          |                               RemoteAddress=LocalSubnet
          |                                        |
          |                         +--------------+---------------+
          |                         |                              |
          v                         v                              v
 desktop stream/control      internal-sftp                 PowerShell 7 SSH
 (GRD 47989/47990)       (chroot + NTFS ACLs)          (standard-user rights)
```

The script creates a `SYSTEM` startup task named `GRDOpenSSH`, which runs a
second `sshd` process in foreground mode with the isolated GRD configuration.
It does not edit, replace or restart the normal Windows `sshd` service.

## Permission profiles

Each account receives exactly one profile. Removing an account from the policy
and applying it again revokes its managed group membership and key.

| Profile | Files | Shell | Intended use |
| --- | --- | --- | --- |
| `SftpRead` | Read `Download` and `Exchange` | None; `internal-sftp` is forced | Download-only users |
| `SftpWrite` | Read `Download`; modify `Exchange` | None; `internal-sftp` is forced | Bidirectional file exchange |
| `PowerShell` | Whatever the standard Windows account can access | PowerShell subsystem and normal SSH shell | Explicit host operators |

All profiles use public-key authentication. Password authentication, empty
passwords, keyboard-interactive authentication, agent forwarding, TCP
forwarding, tunnels, X11 forwarding and user environment files are disabled.
The SFTP chroot root is owned by Administrators/SYSTEM and is not writable by
remote users. The writable directory is a child named `Exchange`.

GRD controller and observer roles inherit **none** of these profiles. A desktop
controller does not automatically become an SFTP or PowerShell user.

PowerShell-over-SSH preserves the Windows privileges of the account. By
default, the setup script consequently accepts local standard accounts only
and rejects direct members of Administrators, Remote Desktop Users, Remote
Management Users, Backup Operators and other privileged built-in groups. It
never creates or deletes Windows accounts.

An administrator who deliberately needs an existing privileged identity may
pass `-AllowPrivilegedPowerShellAccounts` to `Plan`, `Apply`, and `Audit`. This
high-risk override applies only to entries whose profile is `PowerShell`; it
never permits a privileged account in either SFTP profile. The switch must be
supplied again for every reconciliation or audit so the exception cannot
become an invisible default. Such a session can access everything allowed to
that Windows identity and is not constrained to a cmdlet allowlist.

## 1. Create dedicated standard accounts

Run these commands in an elevated PowerShell prompt and choose strong random
local passwords. Those passwords are not placed in the policy and cannot be
used by the dedicated SSH service, which is key-only.

```powershell
$credential = Get-Credential -UserName 'grd-file-writer'
New-LocalUser -Name $credential.UserName -Password $credential.Password
```

Use a different account for PowerShell. Do not add these accounts to
Administrators, Remote Desktop Users or another privileged group.

## 2. Generate one key per account on the client

macOS, Linux and current Windows versions include `ssh-keygen`:

```text
ssh-keygen -t ed25519 -a 64 -f ~/.ssh/grd-file-writer
```

Protect each private key with a passphrase. Copy only the single-line `.pub`
value into the host policy. Never copy or commit a private key.

## 3. Create and validate a local policy

Copy the example to the ignored local-policy name:

```powershell
Copy-Item .\scripts\grd-remote-access.example.psd1 `
          .\scripts\grd-remote-access.local.psd1
```

Add existing account names and one permission per entry. `PublicKey` remains
available for one client; use `PublicKeys` for up to 16 client keys belonging
to the same Windows account:

```powershell
@{
    Version = 1
    Port = 47992
    SftpRoot = 'C:\ProgramData\GRD\Sftp'
    Users = @(
        @{
            Name = 'grd-file-writer'
            Permission = 'SftpWrite'
            PublicKey = 'ssh-ed25519 AAAA... macbook-grd'
        }
        @{
            Name = 'grd-operator'
            Permission = 'PowerShell'
            PublicKeys = @(
                'ssh-ed25519 AAAA... admin-macbook'
                'ssh-ed25519 AAAA... backup-workstation'
            )
        }
    )
}
```

Validation and planning do not require elevation and make no changes:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\grd-remote-access.ps1 `
  -Mode Validate `
  -PolicyPath .\scripts\grd-remote-access.local.psd1

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\grd-remote-access.ps1 `
  -Mode Plan `
  -PolicyPath .\scripts\grd-remote-access.local.psd1
```

## 4. Apply and audit on the host

Review the plan, open PowerShell **as Administrator**, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\grd-remote-access.ps1 `
  -Mode Apply `
  -PolicyPath .\scripts\grd-remote-access.local.psd1

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\grd-remote-access.ps1 `
  -Mode Audit `
  -PolicyPath .\scripts\grd-remote-access.local.psd1
```

For an explicitly approved privileged `PowerShell` entry, append
`-AllowPrivilegedPowerShellAccounts` to both commands.

`Apply` installs the Windows OpenSSH Server capability if necessary, creates
the isolated startup task/configuration and managed groups, writes the public keys,
sets NTFS ACLs, validates `sshd_config`, and opens only the selected TCP port
for `LocalSubnet` on Private network profiles.

For safety, the SFTP root must contain a `GRD` path component. An existing
non-empty directory is accepted only if it already contains the management
marker created by this script; this prevents an accidental ACL replacement on
an unrelated directory. `Apply` also refuses a port owned by another process.

The generated host key and verbose SSH audit log are stored under:

```text
C:\ProgramData\GRD\OpenSSH\
```

Verify the host fingerprint locally before accepting it on a client:

```powershell
ssh-keygen -lf C:\ProgramData\GRD\OpenSSH\ssh_host_ed25519_key.pub
```

## 5. Connect

SFTP is available with the standard client included in macOS, Linux and
current Windows versions:

```text
sftp -P 47992 -i ~/.ssh/grd-file-writer grd-file-writer@HOST_LAN_IP
```

PowerShell remoting requires PowerShell 7 on both machines:

```powershell
Enter-PSSession `
  -HostName HOST_LAN_IP `
  -Port 47992 `
  -UserName grd-operator `
  -KeyFilePath ~/.ssh/grd-operator `
  -Subsystem powershell
```

## Revocation and removal

To revoke one user, remove it from the policy and run `Apply` again. To disable
the whole optional service while preserving transferred files:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\grd-remote-access.ps1 `
  -Mode Remove `
  -PolicyPath .\scripts\grd-remote-access.local.psd1
```

`Remove` preserves the SFTP data and Windows accounts. `-RemoveData` is an
explicit destructive option that also deletes the configured SFTP root. Data
removal requires the valid management marker and refuses reparse points.
OpenSSH Server itself is preserved because another application might use the
Windows capability.

## Security limits

- Keep this endpoint on a trusted LAN; do not port-forward it from a router.
- A `PowerShell` account has all capabilities of that Windows account, not an
  allowlist of cmdlets.
- PowerShell's SSH transport does not currently support endpoint
  configuration or Just Enough Administration (JEA). A future privileged,
  command-limited mode should use a separately secured JEA/WinRM endpoint,
  not pretend that an SSH PowerShell session is constrained.
- `ChrootDirectory` must remain a local path, not a UNC/network share.
- Public keys identify users; protect private keys as carefully as passwords
  and use distinct keys so one user can be revoked independently.

References:

- [Get started with OpenSSH Server for Windows](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse)
- [OpenSSH Server configuration for Windows](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh-server-configuration)
- [Troubleshoot SFTP chroot on Windows](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/troubleshoot-sftp-issues-using-openssh)
- [PowerShell remoting over SSH](https://learn.microsoft.com/en-us/powershell/scripting/security/remoting/ssh-remoting-in-powershell)
