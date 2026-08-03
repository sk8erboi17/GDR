#Requires -Version 5.1

<#
.SYNOPSIS
Configures an isolated, key-only OpenSSH service for GRD file transfer and
optional PowerShell remoting on a trusted Windows LAN.

.DESCRIPTION
The script never reuses the GRD password and never edits the system sshd
configuration. It creates a dedicated SYSTEM startup task, firewall rule, host key,
authorized-key store and three managed local groups:

  grd-sftp-ro   SFTP download/read only, no shell
  grd-sftp-rw   SFTP read/write in Exchange, no shell
  grd-powershell PowerShell/SSH with the rights of a standard Windows account

The policy is authoritative: Apply removes stale members from these managed
groups and stale public-key files from the managed key directory. It never
deletes Windows user accounts. OpenSSH Server is not uninstalled by Remove.
#>

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [ValidateSet('Validate', 'SelfTest', 'Plan', 'Audit', 'Apply', 'Remove')]
    [string]$Mode = 'Validate',

    [string]$PolicyPath = '',

    [switch]$AllowPrivilegedPowerShellAccounts,

    [switch]$RemoveData
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop

if ([string]::IsNullOrWhiteSpace($PolicyPath)) {
    $PolicyPath = Join-Path $PSScriptRoot 'grd-remote-access.example.psd1'
}

$script:TaskName = 'GRDOpenSSH'
$script:TaskPath = '\'
$script:FirewallRuleName = 'GRD-RemoteAccess-In-TCP'
$script:OpenSshCapability = 'OpenSSH.Server~~~~0.0.1.0'
$script:ManagedGroups = @(
    'grd-sftp-ro',
    'grd-sftp-rw',
    'grd-powershell'
)
$script:GroupByPermission = @{
    SftpRead  = 'grd-sftp-ro'
    SftpWrite = 'grd-sftp-rw'
    PowerShell = 'grd-powershell'
}
$script:DeniedLocalGroupSids = @(
    'S-1-5-32-544', # Administrators
    'S-1-5-32-547', # Power Users
    'S-1-5-32-548', # Account Operators
    'S-1-5-32-549', # Server Operators
    'S-1-5-32-550', # Print Operators
    'S-1-5-32-551', # Backup Operators
    'S-1-5-32-555', # Remote Desktop Users
    'S-1-5-32-556', # Network Configuration Operators
    'S-1-5-32-569', # Cryptographic Operators
    'S-1-5-32-573', # Event Log Readers
    'S-1-5-32-578', # Hyper-V Administrators
    'S-1-5-32-580'  # Remote Management Users
)
$script:ProgramData = [Environment]::GetFolderPath('CommonApplicationData')
$script:AccessRoot = Join-Path $script:ProgramData 'GRD\OpenSSH'
$script:AuthorizedKeysRoot = Join-Path $script:AccessRoot 'authorized_keys'
$script:ConfigPath = Join-Path $script:AccessRoot 'sshd_config'
$script:HostKeyPath = Join-Path $script:AccessRoot 'ssh_host_ed25519_key'
$script:PidPath = Join-Path $script:AccessRoot 'sshd.pid'
$script:LogPath = Join-Path $script:AccessRoot 'sshd.log'
$script:SftpMarkerName = '.grd-managed-sftp-root'
$script:SftpMarkerValue = 'GRD managed SFTP root v1'

function Test-IsWindows {
    return [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator
        )) {
        throw 'Apply/Remove requires an elevated PowerShell session.'
    }
}

function Assert-PolicyValue {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "Invalid remote-access policy: $Message"
    }
}

function Test-Ed25519PublicKey {
    param([string]$PublicKey)

    if ([string]::IsNullOrWhiteSpace($PublicKey) -or
        $PublicKey.Contains("`r") -or $PublicKey.Contains("`n")) {
        return $false
    }
    if ($PublicKey -notmatch (
            '^ssh-ed25519 ([A-Za-z0-9+/]+={0,3})(?: [^\x00-\x1f]+)?$'
        )) {
        return $false
    }
    try {
        $blob = [Convert]::FromBase64String($Matches[1])
    } catch {
        return $false
    }
    if ($blob.Length -ne 51) {
        return $false
    }
    $algorithmLength =
        ([int]$blob[0] -shl 24) -bor ([int]$blob[1] -shl 16) -bor
        ([int]$blob[2] -shl 8) -bor [int]$blob[3]
    $keyLength =
        ([int]$blob[15] -shl 24) -bor ([int]$blob[16] -shl 16) -bor
        ([int]$blob[17] -shl 8) -bor [int]$blob[18]
    return $algorithmLength -eq 11 -and $keyLength -eq 32 -and
           [Text.Encoding]::ASCII.GetString($blob, 4, 11) -eq 'ssh-ed25519'
}

function Read-RemoteAccessPolicy {
    param([string]$LiteralPath)

    Assert-PolicyValue (Test-Path -LiteralPath $LiteralPath -PathType Leaf) (
        "file not found: $LiteralPath"
    )
    $data = Import-PowerShellDataFile -LiteralPath $LiteralPath
    Assert-PolicyValue ($data -is [hashtable]) 'root value must be a hashtable'
    Assert-PolicyValue ($data.ContainsKey('Version')) 'Version is required'
    Assert-PolicyValue ([int]$data.Version -eq 1) 'Version must be 1'
    Assert-PolicyValue ($data.ContainsKey('Port')) 'Port is required'
    $port = [int]$data.Port
    Assert-PolicyValue ($port -ge 1024 -and $port -le 65535) (
        'Port must be between 1024 and 65535'
    )
    Assert-PolicyValue ($port -ne 47989 -and $port -ne 47990) (
        'Port must not overlap GRD discovery/control/media ports'
    )
    Assert-PolicyValue ($data.ContainsKey('SftpRoot')) 'SftpRoot is required'
    $sftpRoot = [string]$data.SftpRoot
    Assert-PolicyValue ($sftpRoot -match '^[A-Za-z]:\\') (
        'SftpRoot must be an absolute local Windows path'
    )
    Assert-PolicyValue (-not $sftpRoot.StartsWith('\\')) (
        'SftpRoot must not be a UNC/network path'
    )
    Assert-PolicyValue ($sftpRoot -match (
            '^[A-Za-z]:\\[A-Za-z0-9_.-]+(?:\\[A-Za-z0-9_.-]+)*\\?$'
        )) (
        'SftpRoot may contain only safe local path characters'
    )
    Assert-PolicyValue ($sftpRoot -notmatch '\.\.') (
        'SftpRoot must not contain parent traversal'
    )
    Assert-PolicyValue ($sftpRoot -notmatch '\s') (
        'SftpRoot must not contain whitespace'
    )
    $sftpRoot = [IO.Path]::GetFullPath($sftpRoot).TrimEnd('\')
    Assert-PolicyValue ($sftpRoot.Length -gt 3) (
        'SftpRoot must not be a drive root'
    )
    Assert-PolicyValue ($sftpRoot -match '\\GRD(?:\\|$)') (
        'SftpRoot must contain a dedicated GRD path component'
    )

    $users = @()
    $seenUsers = @{}
    if ($data.ContainsKey('Users')) {
        foreach ($entry in @($data.Users)) {
            if ($null -eq $entry) {
                continue
            }
            Assert-PolicyValue ($entry -is [hashtable]) (
                'every Users entry must be a hashtable'
            )
            foreach ($required in @('Name', 'Permission')) {
                Assert-PolicyValue ($entry.ContainsKey($required)) (
                    "Users entry is missing $required"
                )
            }
            $name = ([string]$entry.Name).Trim()
            $permission = ([string]$entry.Permission).Trim()
            Assert-PolicyValue ($name -match '^[A-Za-z0-9][A-Za-z0-9_.-]{0,19}$') (
                "invalid local account name: $name"
            )
            Assert-PolicyValue (
                $script:GroupByPermission.ContainsKey($permission)
            ) "invalid permission for ${name}: $permission"
            Assert-PolicyValue (-not $seenUsers.ContainsKey($name)) (
                "duplicate account: $name"
            )
            $hasPublicKey = $entry.ContainsKey('PublicKey')
            $hasPublicKeys = $entry.ContainsKey('PublicKeys')
            Assert-PolicyValue ($hasPublicKey -xor $hasPublicKeys) (
                "${name}: specify exactly one of PublicKey or PublicKeys"
            )
            $rawPublicKeys = @(
                if ($hasPublicKey) {
                    $entry.PublicKey
                } else {
                    $entry.PublicKeys
                }
            )
            Assert-PolicyValue (
                $rawPublicKeys.Count -ge 1 -and $rawPublicKeys.Count -le 16
            ) "${name}: between 1 and 16 public keys are required"
            $publicKeys = @()
            $seenPublicKeys = @{}
            foreach ($rawPublicKey in $rawPublicKeys) {
                Assert-PolicyValue ($rawPublicKey -is [string]) (
                    "${name}: every public key must be a string"
                )
                $publicKey = $rawPublicKey.Trim()
                Assert-PolicyValue (Test-Ed25519PublicKey $publicKey) (
                    "${name}: only one-line ssh-ed25519 public keys are accepted"
                )
                $keyIdentity = ($publicKey -split ' ')[1]
                Assert-PolicyValue (-not $seenPublicKeys.ContainsKey(
                        $keyIdentity
                    )) "${name}: duplicate public key"
                $seenPublicKeys[$keyIdentity] = $true
                $publicKeys += $publicKey
            }
            $seenUsers[$name] = $true
            $users += [pscustomobject]@{
                Name = $name
                Permission = $permission
                Group = $script:GroupByPermission[$permission]
                PublicKeys = @($publicKeys)
            }
        }
    }

    return [pscustomobject]@{
        Version = 1
        Port = $port
        SftpRoot = $sftpRoot
        Users = $users
    }
}

function Show-PolicySummary {
    param($Policy)

    Write-Output "GRD remote access policy v$($Policy.Version)"
    Write-Output "  endpoint : TCP $($Policy.Port), Private/LocalSubnet only"
    Write-Output "  SFTP root: $($Policy.SftpRoot)"
    Write-Output '  auth     : per-user ED25519 keys; passwords disabled'
    if ($Policy.Users.Count -eq 0) {
        Write-Output '  users    : none (deny all)'
        return
    }
    foreach ($user in $Policy.Users) {
        Write-Output (
            '  user     : {0,-20} {1}' -f $user.Name, $user.Permission
        )
    }
    if ($AllowPrivilegedPowerShellAccounts.IsPresent -and
        @($Policy.Users | Where-Object Permission -eq 'PowerShell').Count -gt 0) {
        Write-Warning (
            'HIGH-RISK OVERRIDE: privileged Windows accounts are allowed ' +
            'for PowerShell entries in this invocation.'
        )
    }
}

function Test-PrivilegedPowerShellOptIn {
    param(
        [string]$Permission,
        [bool]$ExplicitlyAllowed
    )

    return $ExplicitlyAllowed -and $Permission -eq 'PowerShell'
}

function Invoke-SelfTest {
    $temporaryRoot = Join-Path (
        [IO.Path]::GetTempPath()
    ) ("grd-remote-access-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        $validKey = (
            'ssh-ed25519 ' +
            'AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA ' +
            'grd-selftest'
        )
        $validKey2 = (
            'ssh-ed25519 ' +
            'AAAAC3NzaC1lZDI1NTE5AAAAIAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB ' +
            'grd-selftest-2'
        )
        $valid = Join-Path $temporaryRoot 'valid.psd1'
        @"
@{
    Version = 1
    Port = 47992
    SftpRoot = 'C:\ProgramData\GRD\Sftp'
    Users = @(
        @{ Name='reader'; Permission='SftpRead'; PublicKey='$validKey' }
        @{ Name='writer'; Permission='SftpWrite'; PublicKey='$validKey' }
        @{ Name='operator'; Permission='PowerShell'; PublicKeys=@('$validKey','$validKey2') }
    )
}
"@ | Set-Content -LiteralPath $valid -Encoding UTF8
        $policy = Read-RemoteAccessPolicy $valid
        if ($policy.Users.Count -ne 3 -or $policy.Port -ne 47992 -or
            $policy.Users[0].PublicKeys.Count -ne 1 -or
            $policy.Users[2].PublicKeys.Count -ne 2) {
            throw 'Valid policy did not normalize as expected.'
        }

        $invalidCases = @{
            reserved_port = (Get-Content -LiteralPath $valid -Raw).Replace(
                'Port = 47992', 'Port = 47990'
            )
            network_root = (Get-Content -LiteralPath $valid -Raw).Replace(
                "'C:\ProgramData\GRD\Sftp'", "'\\server\share'"
            )
            unmanaged_root = (Get-Content -LiteralPath $valid -Raw).Replace(
                "'C:\ProgramData\GRD\Sftp'", "'C:\ProgramData\Transfers'"
            )
            unsafe_root_character = (
                Get-Content -LiteralPath $valid -Raw
            ).Replace(
                "'C:\ProgramData\GRD\Sftp'", "'C:\ProgramData\GRD\Sftp#bad'"
            )
            weak_key = (Get-Content -LiteralPath $valid -Raw).Replace(
                $validKey, 'ssh-rsa AAAA grd-selftest'
            )
            duplicate_user = (Get-Content -LiteralPath $valid -Raw).Replace(
                "Name='writer'", "Name='reader'"
            )
            invalid_permission = (Get-Content -LiteralPath $valid -Raw).Replace(
                "Permission='SftpRead'", "Permission='Administrator'"
            )
            ambiguous_key_fields = (
                Get-Content -LiteralPath $valid -Raw
            ).Replace(
                "PublicKeys=@('$validKey','$validKey2')",
                "PublicKey='$validKey'; PublicKeys=@('$validKey2')"
            )
            empty_key_list = (Get-Content -LiteralPath $valid -Raw).Replace(
                "PublicKeys=@('$validKey','$validKey2')", 'PublicKeys=@()'
            )
            duplicate_key = (Get-Content -LiteralPath $valid -Raw).Replace(
                $validKey2, $validKey
            )
        }
        foreach ($case in $invalidCases.GetEnumerator()) {
            $casePath = Join-Path $temporaryRoot ($case.Key + '.psd1')
            $case.Value | Set-Content -LiteralPath $casePath -Encoding UTF8
            $rejected = $false
            try {
                [void](Read-RemoteAccessPolicy $casePath)
            } catch {
                $rejected = $true
            }
            if (-not $rejected) {
                throw "Invalid self-test policy was accepted: $($case.Key)"
            }
        }
        $sftpOnlyPolicy = [pscustomobject]@{
            Version = $policy.Version
            Port = $policy.Port
            SftpRoot = $policy.SftpRoot
            Users = @($policy.Users | Where-Object Permission -ne 'PowerShell')
        }
        $configuration = New-SshdConfiguration $sftpOnlyPolicy
        foreach ($requiredLine in @(
                'PasswordAuthentication no',
                'AuthenticationMethods publickey',
                'ForceCommand internal-sftp -d /Download',
                'ForceCommand internal-sftp -d /Exchange',
                'AllowTcpForwarding no'
            )) {
            if (-not $configuration.Contains($requiredLine)) {
                throw "Generated config is missing: $requiredLine"
            }
        }
        $unsafeRejected = $false
        try {
            [void](Assert-SafeManagedPath 'C:\')
        } catch {
            $unsafeRejected = $true
        }
        if (-not $unsafeRejected) {
            throw 'Unsafe removal root was accepted.'
        }
        if (-not (Test-PrivilegedPowerShellOptIn 'PowerShell' $true) -or
            (Test-PrivilegedPowerShellOptIn 'PowerShell' $false) -or
            (Test-PrivilegedPowerShellOptIn 'SftpWrite' $true)) {
            throw 'Privileged-account opt-in was not limited to PowerShell.'
        }

        $emptyRoot = Join-Path $temporaryRoot 'GRD\empty'
        New-Item -ItemType Directory -Path $emptyRoot | Out-Null
        Assert-SftpRootCanBeManaged $emptyRoot

        $unmanagedRoot = Join-Path $temporaryRoot 'GRD\unmanaged'
        New-Item -ItemType Directory -Path $unmanagedRoot | Out-Null
        Set-Content -LiteralPath (Join-Path $unmanagedRoot 'existing.txt') `
            -Value 'existing data'
        $unmanagedRejected = $false
        try {
            Assert-SftpRootCanBeManaged $unmanagedRoot
        } catch {
            $unmanagedRejected = $true
        }
        if (-not $unmanagedRejected) {
            throw 'Non-empty unmarked SFTP root was accepted.'
        }

        $markedRoot = Join-Path $temporaryRoot 'GRD\marked'
        New-Item -ItemType Directory -Path $markedRoot | Out-Null
        Set-Content -LiteralPath (Get-SftpMarkerPath $markedRoot) `
            -Value $script:SftpMarkerValue -NoNewline
        Assert-SftpRootCanBeManaged $markedRoot
        if ((Assert-SafeSftpDataRemoval $markedRoot) -ne
            [IO.Path]::GetFullPath($markedRoot).TrimEnd('\')) {
            throw 'Marked SFTP removal path was not normalized as expected.'
        }
        Write-Output 'GRD remote-access policy self-test: OK (25/25)'
    } finally {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    }
}

function Get-OpenSshExecutable {
    param([string]$Name)

    $systemPath = Join-Path $env:SystemRoot "System32\OpenSSH\$Name"
    if (Test-Path -LiteralPath $systemPath -PathType Leaf) {
        return $systemPath
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    throw "$Name was not found after installing OpenSSH Server."
}

function Install-OpenSshServer {
    $openSshRoot = Join-Path $env:SystemRoot 'System32\OpenSSH'
    if ((Test-Path -LiteralPath (Join-Path $openSshRoot 'sshd.exe') `
            -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $openSshRoot 'ssh-keygen.exe') `
            -PathType Leaf)) {
        return
    }

    $capability = Get-WindowsCapability -Online -Name $script:OpenSshCapability
    $installedByThisRun = $false
    if ($null -eq $capability -or $capability.State -ne 'Installed') {
        if ($PSCmdlet.ShouldProcess('Windows', 'Install OpenSSH Server capability')) {
            Add-WindowsCapability -Online -Name $script:OpenSshCapability |
                Out-Null
            $installedByThisRun = $true
        }
    }
    if ($installedByThisRun) {
        $defaultRule = Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' `
            -ErrorAction SilentlyContinue
        if ($null -ne $defaultRule -and $PSCmdlet.ShouldProcess(
                'OpenSSH-Server-In-TCP',
                'Disable installer-created port 22 firewall rule'
            )) {
            Disable-NetFirewallRule -Name 'OpenSSH-Server-In-TCP'
        }
    }
}

function Get-StandardLocalUser {
    param(
        [string]$Name,
        [string]$Permission = ''
    )

    $user = Get-LocalUser -Name $Name -ErrorAction SilentlyContinue
    if ($null -eq $user) {
        throw "Policy account does not exist locally: $Name"
    }
    if (-not $user.Enabled) {
        throw "Policy account is disabled: $Name"
    }
    $privilegedGroups = @()
    foreach ($groupSid in $script:DeniedLocalGroupSids) {
        $group = Get-LocalGroup -SID $groupSid -ErrorAction SilentlyContinue
        if ($null -eq $group) {
            continue
        }
        $member = Get-LocalGroupMember -Group $group |
            Where-Object { $_.SID.Value -eq $user.SID.Value }
        if ($null -ne $member) {
            $privilegedGroups += $group.Name
        }
    }
    if ($privilegedGroups.Count -gt 0) {
        if (-not (Test-PrivilegedPowerShellOptIn `
                $Permission `
                $AllowPrivilegedPowerShellAccounts.IsPresent)) {
            throw (
                "Refusing account '$Name' because it belongs to the " +
                'privileged/remote group(s): ' +
                ($privilegedGroups -join ', ') + '.'
            )
        }
        Write-Warning (
            "HIGH-RISK OVERRIDE for '$Name': PowerShell-over-SSH keeps " +
            'the rights of privileged group(s): ' +
            ($privilegedGroups -join ', ') + '.'
        )
    }
    return $user
}

function Ensure-Directory {
    param([string]$LiteralPath)

    if (-not (Test-Path -LiteralPath $LiteralPath -PathType Container)) {
        if ($PSCmdlet.ShouldProcess($LiteralPath, 'Create directory')) {
            New-Item -ItemType Directory -Path $LiteralPath -Force | Out-Null
        }
    }
}

function Invoke-Icacls {
    param(
        [string]$LiteralPath,
        [string[]]$AclArguments
    )

    $icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    & $icacls $LiteralPath @AclArguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "icacls failed for $LiteralPath"
    }
}

function Invoke-TakeAdministratorsOwnership {
    param([string]$LiteralPath)

    $takeown = Join-Path $env:SystemRoot 'System32\takeown.exe'
    & $takeown /F $LiteralPath /A | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "takeown failed for $LiteralPath"
    }
}

function Test-AdministratorsCanReplaceAcl {
    param([string]$LiteralPath)

    try {
        $acl = Get-Acl -LiteralPath $LiteralPath
        $sidType = [Security.Principal.SecurityIdentifier]
        $owner = $acl.GetOwner($sidType).Value
        $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        if ($owner -eq $currentSid -or $owner -eq 'S-1-5-32-544') {
            return $true
        }
        $canReplace = $false
        foreach ($rule in @($acl.GetAccessRules($true, $true, $sidType))) {
            $sid = $rule.IdentityReference.Value
            if ($sid -ne $currentSid -and $sid -ne 'S-1-5-32-544') {
                continue
            }
            $canChange = ($rule.FileSystemRights -band
                [Security.AccessControl.FileSystemRights]::ChangePermissions) `
                -ne 0
            if (-not $canChange) {
                continue
            }
            if ($rule.AccessControlType -eq
                [Security.AccessControl.AccessControlType]::Deny) {
                return $false
            }
            $canReplace = $true
        }
        return $canReplace
    } catch {
        return $false
    }
}

function Set-ManagedAcl {
    param(
        [string]$LiteralPath,
        [string[]]$Grants,
        [switch]$SetAdministratorsOwner,
        [switch]$SetSystemOwner
    )

    if (-not $PSCmdlet.ShouldProcess($LiteralPath, 'Replace managed ACL')) {
        return
    }
    if (-not (Test-AdministratorsCanReplaceAcl $LiteralPath)) {
        # A previous interrupted reconciliation may have left a SYSTEM-owned
        # object with an empty DACL. Take ownership only for this exact managed
        # target before resetting it from its already restricted parent.
        Invoke-TakeAdministratorsOwnership $LiteralPath
    }
    Invoke-Icacls $LiteralPath @('/reset')
    # Add the final explicit grants before removing inherited entries. Doing
    # this in the opposite order can momentarily create an empty DACL and lock
    # an elevated administrator out of a SYSTEM-owned host key.
    $arguments = @('/grant:r') + $Grants
    Invoke-Icacls $LiteralPath $arguments
    Invoke-Icacls $LiteralPath @('/inheritance:r')
    if ($SetAdministratorsOwner) {
        Invoke-Icacls $LiteralPath @('/setowner', '*S-1-5-32-544')
    } elseif ($SetSystemOwner) {
        Invoke-Icacls $LiteralPath @('/setowner', '*S-1-5-18')
    }
}

function Ensure-ManagedGroups {
    foreach ($groupName in $script:ManagedGroups) {
        if ($null -eq (Get-LocalGroup -Name $groupName -ErrorAction SilentlyContinue)) {
            if ($PSCmdlet.ShouldProcess($groupName, 'Create managed local group')) {
                New-LocalGroup -Name $groupName -Description (
                    'Managed by GRD remote-access policy.'
                ) | Out-Null
            }
        }
    }
}

function Sync-GroupMemberships {
    param($Policy)

    $desiredByGroup = @{}
    foreach ($groupName in $script:ManagedGroups) {
        $desiredByGroup[$groupName] = @{}
    }
    foreach ($entry in $Policy.Users) {
        $account = Get-StandardLocalUser `
            -Name $entry.Name `
            -Permission $entry.Permission
        $entry | Add-Member -NotePropertyName SID -NotePropertyValue (
            $account.SID.Value
        ) -Force
        $desiredByGroup[$entry.Group][$account.SID.Value] = $true
    }

    foreach ($groupName in $script:ManagedGroups) {
        $current = @(
            Get-LocalGroupMember -Group $groupName -ErrorAction SilentlyContinue
        )
        foreach ($member in $current) {
            if (-not $desiredByGroup[$groupName].ContainsKey($member.SID.Value)) {
                if ($PSCmdlet.ShouldProcess(
                        "$($member.Name) from $groupName",
                        'Revoke stale managed permission'
                    )) {
                    Remove-LocalGroupMember -Group $groupName -Member $member
                }
            }
        }
    }

    foreach ($entry in $Policy.Users) {
        foreach ($groupName in $script:ManagedGroups) {
            $member = Get-LocalGroupMember -Group $groupName |
                Where-Object { $_.SID.Value -eq $entry.SID }
            if ($groupName -eq $entry.Group -and $null -eq $member) {
                if ($PSCmdlet.ShouldProcess(
                        "$($entry.Name) -> $groupName",
                        'Grant managed permission'
                    )) {
                    Add-LocalGroupMember -Group $groupName -Member $entry.Name
                }
            } elseif ($groupName -ne $entry.Group -and $null -ne $member) {
                if ($PSCmdlet.ShouldProcess(
                        "$($entry.Name) from $groupName",
                        'Remove conflicting managed permission'
                    )) {
                    Remove-LocalGroupMember -Group $groupName -Member $entry.Name
                }
            }
        }
    }
}

function Get-SftpMarkerPath {
    param([string]$SftpRoot)

    return Join-Path $SftpRoot $script:SftpMarkerName
}

function Assert-SftpRootCanBeManaged {
    param([string]$SftpRoot)

    if (-not (Test-Path -LiteralPath $SftpRoot)) {
        return
    }
    if (-not (Test-Path -LiteralPath $SftpRoot -PathType Container)) {
        throw "SFTP root exists but is not a directory: $SftpRoot"
    }
    $rootItem = Get-Item -LiteralPath $SftpRoot -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing an SFTP root that is a reparse point: $SftpRoot"
    }

    $markerPath = Get-SftpMarkerPath $SftpRoot
    if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
        $marker = (Get-Content -LiteralPath $markerPath -Raw).Trim()
        if ($marker -ne $script:SftpMarkerValue) {
            throw "Invalid GRD management marker in SFTP root: $SftpRoot"
        }
        return
    }

    $existingContent = Get-ChildItem -LiteralPath $SftpRoot -Force |
        Select-Object -First 1
    if ($null -ne $existingContent) {
        throw (
            'Refusing to replace ACLs on an existing non-empty, unmanaged ' +
            "SFTP root: $SftpRoot"
        )
    }
}

function Set-SftpDirectories {
    param($Policy)

    $download = Join-Path $Policy.SftpRoot 'Download'
    $exchange = Join-Path $Policy.SftpRoot 'Exchange'
    foreach ($directory in @(
            $script:AccessRoot,
            $script:AuthorizedKeysRoot,
            $Policy.SftpRoot,
            $download,
            $exchange
        )) {
        Ensure-Directory $directory
    }
    $markerPath = Get-SftpMarkerPath $Policy.SftpRoot
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf) -and
        $PSCmdlet.ShouldProcess($markerPath, 'Mark dedicated SFTP root')) {
        Set-Content -LiteralPath $markerPath -Value $script:SftpMarkerValue `
            -Encoding Ascii -NoNewline
    }

    $systemFull = '*S-1-5-18:(OI)(CI)(F)'
    $adminsFull = '*S-1-5-32-544:(OI)(CI)(F)'
    $readGroup = Get-LocalGroup -Name $script:GroupByPermission.SftpRead
    $writeGroup = Get-LocalGroup -Name $script:GroupByPermission.SftpWrite
    $readTraverse = "*$($readGroup.SID.Value):(RX)"
    $writeTraverse = "*$($writeGroup.SID.Value):(RX)"
    $readTree = "*$($readGroup.SID.Value):(OI)(CI)(RX)"
    $writeTree = "*$($writeGroup.SID.Value):(OI)(CI)(RX)"
    $writeModify = "*$($writeGroup.SID.Value):(OI)(CI)(M)"

    Set-ManagedAcl $script:AccessRoot @($systemFull, $adminsFull) `
        -SetAdministratorsOwner
    Set-ManagedAcl $script:AuthorizedKeysRoot @($systemFull, $adminsFull) `
        -SetAdministratorsOwner
    Set-ManagedAcl $Policy.SftpRoot @(
        $systemFull, $adminsFull, $readTraverse, $writeTraverse
    ) -SetAdministratorsOwner
    Set-ManagedAcl $download @(
        $systemFull, $adminsFull, $readTree, $writeTree
    ) -SetAdministratorsOwner
    Set-ManagedAcl $exchange @(
        $systemFull, $adminsFull, $readTree, $writeModify
    ) -SetAdministratorsOwner
}

function Sync-AuthorizedKeys {
    param($Policy)

    $expectedFiles = @{}
    foreach ($entry in $Policy.Users) {
        $fileName = $entry.Name.ToLowerInvariant()
        $keyPath = Join-Path $script:AuthorizedKeysRoot $fileName
        $expectedFiles[$fileName] = $true
        if ($PSCmdlet.ShouldProcess($keyPath, 'Install managed public keys')) {
            Set-Content -LiteralPath $keyPath -Value $entry.PublicKeys `
                -Encoding Ascii
            Set-ManagedAcl $keyPath @(
                '*S-1-5-18:F',
                '*S-1-5-32-544:F',
                "*$($entry.SID):R"
            )
        }
    }
    foreach ($file in @(
            Get-ChildItem -LiteralPath $script:AuthorizedKeysRoot -File `
                -ErrorAction SilentlyContinue
        )) {
        if (-not $expectedFiles.ContainsKey($file.Name)) {
            if ($PSCmdlet.ShouldProcess(
                    $file.FullName,
                    'Remove stale managed public key'
                )) {
                Remove-Item -LiteralPath $file.FullName -Force
            }
        }
    }
}

function Test-HostKeyAclRestricted {
    param([string]$LiteralPath)

    try {
        $acl = Get-Acl -LiteralPath $LiteralPath
        $sidType = [Security.Principal.SecurityIdentifier]
        $owner = $acl.GetOwner($sidType).Value
        if ($owner -ne 'S-1-5-18') {
            return $false
        }
        $rules = @($acl.GetAccessRules($true, $true, $sidType))
        if ($rules.Count -ne 2) {
            return $false
        }
        $required = @{
            'S-1-5-18' = $false
            'S-1-5-32-544' = $false
        }
        foreach ($rule in $rules) {
            $sid = $rule.IdentityReference.Value
            if ($rule.IsInherited -or
                $rule.AccessControlType -ne
                    [Security.AccessControl.AccessControlType]::Allow -or
                -not $required.ContainsKey($sid) -or
                ($rule.FileSystemRights -band
                    [Security.AccessControl.FileSystemRights]::FullControl) -ne
                    [Security.AccessControl.FileSystemRights]::FullControl) {
                return $false
            }
            $required[$sid] = $true
        }
        return $required['S-1-5-18'] -and $required['S-1-5-32-544']
    } catch {
        return $false
    }
}

function Ensure-HostKey {
    if (-not (Test-Path -LiteralPath $script:HostKeyPath -PathType Leaf)) {
        $sshKeygen = Get-OpenSshExecutable 'ssh-keygen.exe'
        if ($PSCmdlet.ShouldProcess(
                $script:HostKeyPath,
                'Generate ED25519 host key'
            )) {
            $process = Start-Process -FilePath $sshKeygen -NoNewWindow -Wait `
                -PassThru -ArgumentList @(
                    '-q', '-t', 'ed25519', '-N', '""', '-f',
                    ('"' + $script:HostKeyPath + '"')
                )
            if ($process.ExitCode -ne 0) {
                throw 'ssh-keygen failed while creating the GRD host key.'
            }
        }
    }
    # Repair an interrupted or older setup, but do not rewrite an already
    # restricted key while the managed sshd process has it open. The in-box
    # service rejects a host key accessible to any unrelated principal.
    if (-not (Test-HostKeyAclRestricted $script:HostKeyPath)) {
        Set-ManagedAcl $script:HostKeyPath @(
            '*S-1-5-18:F',
            '*S-1-5-32-544:F'
        ) -SetSystemOwner
    }
}

function Get-PowerShellSubsystemPath {
    $shortDefault = 'C:\Progra~1\PowerShell\7\pwsh.exe'
    if (-not (Test-Path -LiteralPath $shortDefault -PathType Leaf)) {
        throw (
            'PowerShell access was requested, but PowerShell 7 was not ' +
            'found at C:\Program Files\PowerShell\7\pwsh.exe.'
        )
    }
    return $shortDefault.Replace('\', '/')
}

function New-SshdConfiguration {
    param($Policy)

    $toSshPath = {
        param([string]$Path)
        return $Path.Replace('\', '/')
    }
    $hostKey = & $toSshPath $script:HostKeyPath
    $pidFile = & $toSshPath $script:PidPath
    $authorizedKeys = & $toSshPath $script:AuthorizedKeysRoot
    $sftpRoot = & $toSshPath $Policy.SftpRoot
    $lines = @(
        '# Generated by scripts/grd-remote-access.ps1. Do not edit manually.',
        "Port $($Policy.Port)",
        'AddressFamily any',
        'ListenAddress 0.0.0.0',
        'ListenAddress ::',
        "HostKey $hostKey",
        "PidFile $pidFile",
        "AuthorizedKeysFile $authorizedKeys/%u",
        'PubkeyAuthentication yes',
        'PasswordAuthentication no',
        'KbdInteractiveAuthentication no',
        'PermitEmptyPasswords no',
        'AuthenticationMethods publickey',
        ('AllowGroups ' + ($script:ManagedGroups -join ' ')),
        'AllowAgentForwarding no',
        'AllowTcpForwarding no',
        'GatewayPorts no',
        'PermitTunnel no',
        'PermitUserEnvironment no',
        'X11Forwarding no',
        'MaxAuthTries 3',
        'MaxStartups 4:30:8',
        'LoginGraceTime 30',
        'MaxSessions 4',
        'ClientAliveInterval 30',
        'ClientAliveCountMax 2',
        'LogLevel VERBOSE',
        'Subsystem sftp internal-sftp'
    )
    if (@($Policy.Users | Where-Object Permission -eq 'PowerShell').Count -gt 0) {
        $pwsh = Get-PowerShellSubsystemPath
        $lines += "Subsystem powershell $pwsh -sshs -NoLogo -NoProfile"
    }
    $lines += @(
        '',
        "Match Group $($script:GroupByPermission.SftpRead)",
        "    ChrootDirectory $sftpRoot",
        '    ForceCommand internal-sftp -d /Download',
        '    PermitTTY no',
        '',
        'Match all',
        '',
        "Match Group $($script:GroupByPermission.SftpWrite)",
        "    ChrootDirectory $sftpRoot",
        '    ForceCommand internal-sftp -d /Exchange',
        '    PermitTTY no',
        '',
        'Match all',
        ''
    )
    return ($lines -join "`r`n")
}

function Set-SshdConfiguration {
    param($Policy)

    $configuration = New-SshdConfiguration $Policy
    if ($PSCmdlet.ShouldProcess($script:ConfigPath, 'Write isolated sshd config')) {
        Set-Content -LiteralPath $script:ConfigPath -Value $configuration `
            -Encoding Ascii -NoNewline
    }
    return $configuration
}

function Test-SshdConfiguration {
    $sshd = Get-OpenSshExecutable 'sshd.exe'
    & $sshd -t -f $script:ConfigPath
    if ($LASTEXITCODE -ne 0) {
        throw 'OpenSSH rejected the generated GRD sshd_config.'
    }
    return $sshd
}

function Test-TcpPortListening {
    param([int]$Port)

    return $null -ne (Get-NetTCPConnection -State Listen -LocalPort $Port `
        -ErrorAction SilentlyContinue | Select-Object -First 1)
}

function Test-ManagedSshdProcess {
    param(
        [int]$ProcessId,
        $ManagedTask
    )

    if ($ProcessId -le 0 -or $null -eq $ManagedTask) {
        return $false
    }
    try {
        $actions = @($ManagedTask.Actions)
        if ($actions.Count -ne 1) {
            return $false
        }
        $process = Get-CimInstance -ClassName Win32_Process `
            -Filter "ProcessId = $ProcessId" -ErrorAction Stop
        if ($null -eq $process -or $process.Name -ne 'sshd.exe') {
            return $false
        }
        $expectedExecutable = [IO.Path]::GetFullPath(
            [string]$actions[0].Execute
        )
        $actualExecutable = [IO.Path]::GetFullPath(
            [string]$process.ExecutablePath
        )
        $commandLine = [string]$process.CommandLine
        return [string]::Equals(
                $actualExecutable,
                $expectedExecutable,
                [StringComparison]::OrdinalIgnoreCase
            ) -and
            $commandLine.IndexOf(
                $script:ConfigPath,
                [StringComparison]::OrdinalIgnoreCase
            ) -ge 0 -and
            $commandLine.IndexOf(
                $script:LogPath,
                [StringComparison]::OrdinalIgnoreCase
            ) -ge 0
    } catch {
        return $false
    }
}

function Get-ManagedSshdProcessId {
    $managedTask = Get-ScheduledTask -TaskName $script:TaskName `
        -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
    if ($null -eq $managedTask) {
        return 0
    }
    if (Test-Path -LiteralPath $script:PidPath -PathType Leaf) {
        $pidText = Get-Content -LiteralPath $script:PidPath -Raw `
            -ErrorAction SilentlyContinue
        if ($null -ne $pidText) {
            $managedProcessId = 0
            $pidText = $pidText.Trim()
            if ([int]::TryParse($pidText, [ref]$managedProcessId) -and
                (Test-ManagedSshdProcess $managedProcessId $managedTask)) {
                return $managedProcessId
            }
        }
    }
    # Windows OpenSSH can create the PID file with a SYSTEM-only ACL. Fall
    # back to process metadata bound to the managed task when it is unreadable.
    $candidates = @(
        Get-CimInstance -ClassName Win32_Process `
            -Filter "Name = 'sshd.exe'" -ErrorAction SilentlyContinue
    )
    foreach ($candidate in $candidates) {
        $candidateProcessId = [int]$candidate.ProcessId
        if (Test-ManagedSshdProcess $candidateProcessId $managedTask) {
            return $candidateProcessId
        }
    }
    return 0
}

function Test-ManagedEndpointListening {
    param([int]$Port)

    $managedProcessId = Get-ManagedSshdProcessId
    if ($managedProcessId -eq 0) {
        return $false
    }
    return $null -ne (Get-NetTCPConnection -State Listen -LocalPort $Port `
        -ErrorAction SilentlyContinue | Where-Object {
            [int]$_.OwningProcess -eq $managedProcessId
        } | Select-Object -First 1)
}

function Assert-EndpointPortAvailable {
    param([int]$Port)

    $listeners = @(
        Get-NetTCPConnection -State Listen -LocalPort $Port `
            -ErrorAction SilentlyContinue
    )
    if ($listeners.Count -eq 0) {
        return
    }

    $managedProcessId = Get-ManagedSshdProcessId

    $foreignListeners = @(
        $listeners | Where-Object {
            $managedProcessId -eq 0 -or
            [int]$_.OwningProcess -ne $managedProcessId
        }
    )
    if ($foreignListeners.Count -gt 0) {
        $owners = @(
            $foreignListeners | Select-Object -ExpandProperty OwningProcess `
                -Unique
        ) -join ', '
        throw "TCP port $Port is already used by process ID(s): $owners."
    }
}

function Set-DedicatedTask {
    param(
        [string]$SshdPath,
        [int]$Port
    )

    $arguments = (
        '-D -f "{0}" -E "{1}"' -f $script:ConfigPath, $script:LogPath
    )
    $existing = Get-ScheduledTask -TaskName $script:TaskName `
        -TaskPath $script:TaskPath `
        -ErrorAction SilentlyContinue
    $previousManagedProcessId = Get-ManagedSshdProcessId
    if (-not $PSCmdlet.ShouldProcess(
            $script:TaskName,
            "Install and start isolated sshd on TCP $Port"
        )) {
        throw 'The managed startup-task update was not approved.'
    }

    if ($null -ne $existing) {
        Stop-ScheduledTask -TaskName $script:TaskName `
            -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
        $stopAttemptsRemaining = 50
        do {
            $existing = Get-ScheduledTask -TaskName $script:TaskName `
                -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
            if ($null -eq $existing -or $existing.State -ne 'Running') {
                break
            }
            Start-Sleep -Milliseconds 100
            $stopAttemptsRemaining--
        } while ($stopAttemptsRemaining -gt 0)
        if ($null -ne $existing -and $existing.State -eq 'Running') {
            throw 'The existing GRDOpenSSH task did not stop.'
        }
        if ($previousManagedProcessId -ne 0) {
            $processStopAttemptsRemaining = 50
            while ($null -ne (Get-Process -Id $previousManagedProcessId `
                    -ErrorAction SilentlyContinue) -and
                $processStopAttemptsRemaining -gt 0) {
                Start-Sleep -Milliseconds 100
                $processStopAttemptsRemaining--
            }
            if ($null -ne (Get-Process -Id $previousManagedProcessId `
                    -ErrorAction SilentlyContinue)) {
                throw 'The existing managed sshd process did not stop.'
            }
        }
    }

    $attemptsRemaining = 30
    while ((Test-TcpPortListening $Port) -and $attemptsRemaining -gt 0) {
        Start-Sleep -Milliseconds 100
        $attemptsRemaining--
    }
    if (Test-TcpPortListening $Port) {
        throw "The managed sshd listener on TCP $Port did not stop."
    }
    if (Test-Path -LiteralPath $script:PidPath -PathType Leaf) {
        Remove-Item -LiteralPath $script:PidPath -Force
    }

    if ($null -ne $existing) {
        Unregister-ScheduledTask -TaskName $script:TaskName `
            -TaskPath $script:TaskPath -Confirm:$false
    }
    $action = New-ScheduledTaskAction -Execute $SshdPath `
        -Argument $arguments
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' `
        -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -MultipleInstances IgnoreNew `
        -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $script:TaskName `
        -TaskPath $script:TaskPath `
        -Description (
            'GRD key-only LAN SFTP and optional PowerShell endpoint.'
        ) `
        -Action $action -Trigger $trigger -Principal $principal `
        -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $script:TaskName -TaskPath $script:TaskPath
    $attemptsRemaining = 100
    while (-not (Test-ManagedEndpointListening $Port) -and
        $attemptsRemaining -gt 0) {
        Start-Sleep -Milliseconds 100
        $attemptsRemaining--
    }
    if (-not (Test-ManagedEndpointListening $Port)) {
        $taskInfo = Get-ScheduledTaskInfo -TaskName $script:TaskName `
            -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
        $result = if ($null -eq $taskInfo) {
            'unknown'
        } else {
            [string]$taskInfo.LastTaskResult
        }
        $diagnostic = ''
        if (Test-Path -LiteralPath $script:LogPath -PathType Leaf) {
            $logTail = @(
                Get-Content -LiteralPath $script:LogPath -Tail 20 `
                    -ErrorAction SilentlyContinue
            )
            if ($logTail.Count -gt 0) {
                $diagnostic = ' Last sshd log entries: ' +
                    ($logTail -join ' | ')
            }
        }
        throw (
            "GRDOpenSSH did not listen on TCP $Port after startup; " +
            "scheduled-task result: $result. Check $($script:LogPath)." +
            $diagnostic
        )
    }
}

function Set-FirewallRule {
    param([int]$Port)

    $existing = Get-NetFirewallRule -Name $script:FirewallRuleName `
        -ErrorAction SilentlyContinue
    if (-not $PSCmdlet.ShouldProcess(
            "TCP $Port from LocalSubnet on Private profiles",
            'Install exact managed firewall rule'
        )) {
        throw 'The managed firewall-rule update was not approved.'
    }
    if ($null -ne $existing) {
        Remove-NetFirewallRule -Name $script:FirewallRuleName
    }
    New-NetFirewallRule -Name $script:FirewallRuleName `
        -DisplayName 'GRD remote access (SFTP/PowerShell)' `
        -Description (
            'Managed by GRD; key-only access from the local subnet.'
        ) `
        -Enabled True -Profile Private -Direction Inbound -Action Allow `
        -Protocol TCP -LocalPort $Port -RemoteAddress LocalSubnet |
        Out-Null
}

function Test-FirewallRuleMatchesPolicy {
    param(
        $Rule,
        [int]$Port
    )

    if ($null -eq $Rule -or $Rule.Enabled -ne 'True' -or
        $Rule.Profile -ne 'Private' -or $Rule.Direction -ne 'Inbound' -or
        $Rule.Action -ne 'Allow') {
        return $false
    }
    try {
        $portFilter = Get-NetFirewallPortFilter `
            -AssociatedNetFirewallRule $Rule
        $addressFilter = Get-NetFirewallAddressFilter `
            -AssociatedNetFirewallRule $Rule
        $localPorts = @($portFilter.LocalPort)
        $remoteAddresses = @($addressFilter.RemoteAddress)
        return $portFilter.Protocol -eq 'TCP' -and
            $localPorts.Count -eq 1 -and
            [string]$localPorts[0] -eq [string]$Port -and
            $remoteAddresses.Count -eq 1 -and
            [string]$remoteAddresses[0] -eq 'LocalSubnet'
    } catch {
        return $false
    }
}

function Invoke-Apply {
    param($Policy)

    Assert-Administrator
    Assert-EndpointPortAvailable $Policy.Port
    Assert-SftpRootCanBeManaged $Policy.SftpRoot
    Install-OpenSshServer
    Ensure-ManagedGroups
    Sync-GroupMemberships $Policy
    Set-SftpDirectories $Policy
    Sync-AuthorizedKeys $Policy
    Ensure-HostKey
    [void](Set-SshdConfiguration $Policy)
    $sshd = Test-SshdConfiguration
    Set-FirewallRule $Policy.Port
    Set-DedicatedTask $sshd $Policy.Port
    Write-Output 'GRD remote access applied successfully.'
    Write-Output "Host fingerprint:"
    Get-Content -LiteralPath ($script:HostKeyPath + '.pub')
}

function Write-AuditResult {
    param(
        [bool]$Passed,
        [string]$Message
    )
    if ($Passed) {
        Write-Host "[OK]   $Message"
    } else {
        Write-Host "[FAIL] $Message"
    }
    return $Passed
}

function Invoke-Audit {
    param($Policy)

    if (-not (Test-IsWindows)) {
        throw 'Audit is available only on Windows.'
    }
    $passed = $true
    $passed = (Write-AuditResult (
        Test-Path -LiteralPath $script:ConfigPath -PathType Leaf
    ) 'isolated sshd_config exists') -and $passed
    $task = Get-ScheduledTask -TaskName $script:TaskName `
        -TaskPath $script:TaskPath `
        -ErrorAction SilentlyContinue
    $passed = (Write-AuditResult (
        $null -ne $task -and $task.State -eq 'Running'
    ) 'dedicated GRDOpenSSH startup task is running') -and $passed
    $taskDefinitionOk = $false
    if ($null -ne $task) {
        try {
            $taskActions = @($task.Actions)
            $expectedTaskArguments = (
                '-D -f "{0}" -E "{1}"' -f `
                    $script:ConfigPath, $script:LogPath
            )
            $taskDefinitionOk = $taskActions.Count -eq 1 -and
                $taskActions[0].Execute -eq (Get-OpenSshExecutable 'sshd.exe') `
                -and $taskActions[0].Arguments -eq $expectedTaskArguments
        } catch {
            $taskDefinitionOk = $false
        }
    }
    $passed = (Write-AuditResult $taskDefinitionOk (
        'startup task runs sshd with only the managed config and log'
    )) -and $passed
    $firewall = Get-NetFirewallRule -Name $script:FirewallRuleName `
        -ErrorAction SilentlyContinue
    $passed = (Write-AuditResult (
        Test-FirewallRuleMatchesPolicy $firewall $Policy.Port
    ) 'firewall is Private/LocalSubnet on the exact TCP port') -and $passed
    $passed = (Write-AuditResult (
        Test-ManagedEndpointListening $Policy.Port
    ) "managed sshd listens on TCP $($Policy.Port)") -and $passed
    $passed = (Write-AuditResult (
        Test-Path -LiteralPath $Policy.SftpRoot -PathType Container
    ) 'SFTP chroot exists') -and $passed
    $markerPath = Get-SftpMarkerPath $Policy.SftpRoot
    $markerOk = $false
    if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
        $markerOk = (Get-Content -LiteralPath $markerPath -Raw).Trim() -eq
            $script:SftpMarkerValue
    }
    $passed = (Write-AuditResult $markerOk (
        'SFTP root has the valid GRD management marker'
    )) -and $passed
    $systemSshd = Get-Service -Name 'sshd' -ErrorAction SilentlyContinue
    if ($null -ne $systemSshd -and $systemSshd.Status -eq 'Running') {
        Write-Warning (
            '[WARN] system sshd is also running and is outside the GRD policy'
        )
    }
    if (Test-Path -LiteralPath $script:ConfigPath -PathType Leaf) {
        try {
            $expectedConfiguration = New-SshdConfiguration $Policy
            $actualConfiguration = [IO.File]::ReadAllText($script:ConfigPath)
            $passed = (Write-AuditResult (
                $actualConfiguration -ceq $expectedConfiguration
            ) 'sshd_config exactly matches the policy') -and $passed
            [void](Test-SshdConfiguration)
            $passed = (Write-AuditResult $true 'OpenSSH accepts the config') `
                -and $passed
        } catch {
            [void](Write-AuditResult $false $_.Exception.Message)
            $passed = $false
        }
    }
    $desiredGroupSids = @{}
    foreach ($groupName in $script:ManagedGroups) {
        $desiredGroupSids[$groupName] = @{}
    }
    foreach ($entry in $Policy.Users) {
        $account = $null
        $accountOk = $false
        try {
            $account = Get-StandardLocalUser `
                -Name $entry.Name `
                -Permission $entry.Permission
            $accountOk = $true
        } catch {
            Write-Host "[FAIL] $($_.Exception.Message)"
        }
        $passed = (Write-AuditResult $accountOk (
            "account $($entry.Name) satisfies the requested account policy"
        )) -and $passed
        if ($accountOk) {
            $desiredGroupSids[$entry.Group][$account.SID.Value] = $true
            $member = Get-LocalGroupMember -Group $entry.Group `
                -ErrorAction SilentlyContinue |
                Where-Object { $_.SID.Value -eq $account.SID.Value }
            $passed = (Write-AuditResult ($null -ne $member) (
                "$($entry.Name) belongs to $($entry.Group)"
            )) -and $passed
        }
        $keyPath = Join-Path (
            $script:AuthorizedKeysRoot
        ) $entry.Name.ToLowerInvariant()
        $keyOk = $false
        if (Test-Path -LiteralPath $keyPath -PathType Leaf) {
            $actualKeys = @([IO.File]::ReadAllLines($keyPath))
            $expectedKeys = @($entry.PublicKeys)
            $keyOk = $actualKeys.Count -eq $expectedKeys.Count
            if ($keyOk) {
                for ($keyIndex = 0; $keyIndex -lt $expectedKeys.Count;
                    ++$keyIndex) {
                    if ($actualKeys[$keyIndex] -cne $expectedKeys[$keyIndex]) {
                        $keyOk = $false
                        break
                    }
                }
            }
        }
        $passed = (Write-AuditResult $keyOk (
            "managed keys match policy for $($entry.Name)"
        )) -and $passed
    }
    foreach ($groupName in $script:ManagedGroups) {
        $membershipMatches = $false
        try {
            $members = @(Get-LocalGroupMember -Group $groupName)
            $membershipMatches = $members.Count -eq
                $desiredGroupSids[$groupName].Count
            if ($membershipMatches) {
                foreach ($member in $members) {
                    if (-not $desiredGroupSids[$groupName].ContainsKey(
                            $member.SID.Value
                        )) {
                        $membershipMatches = $false
                        break
                    }
                }
            }
        } catch {
            $membershipMatches = $false
        }
        $passed = (Write-AuditResult $membershipMatches (
            "$groupName membership exactly matches the policy"
        )) -and $passed
    }
    return $passed
}

function Assert-SafeManagedPath {
    param([string]$LiteralPath)

    $full = [IO.Path]::GetFullPath($LiteralPath).TrimEnd('\')
    if ($full.Length -le 3 -or $full -notmatch '\\GRD(?:\\|$)') {
        throw "Refusing unsafe removal path: $full"
    }
    return $full
}

function Assert-SafeSftpDataRemoval {
    param([string]$LiteralPath)

    $full = Assert-SafeManagedPath $LiteralPath
    if (-not (Test-Path -LiteralPath $full)) {
        return $full
    }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "Refusing non-directory SFTP data path: $full"
    }
    $rootItem = Get-Item -LiteralPath $full -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to remove an SFTP root reparse point: $full"
    }
    $markerPath = Get-SftpMarkerPath $full
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf) -or
        (Get-Content -LiteralPath $markerPath -Raw).Trim() -ne
            $script:SftpMarkerValue) {
        throw "Refusing unmarked SFTP data removal: $full"
    }
    $nestedReparsePoint = Get-ChildItem -LiteralPath $full -Force -Recurse |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        } | Select-Object -First 1
    if ($null -ne $nestedReparsePoint) {
        throw (
            'Refusing SFTP data removal because a nested reparse point ' +
            "exists: $($nestedReparsePoint.FullName)"
        )
    }
    return $full
}

function Invoke-Remove {
    param($Policy)

    Assert-Administrator
    $task = Get-ScheduledTask -TaskName $script:TaskName `
        -TaskPath $script:TaskPath `
        -ErrorAction SilentlyContinue
    if ($null -ne $task -and $PSCmdlet.ShouldProcess(
            $script:TaskName,
            'Stop and delete managed startup task'
        )) {
        $managedProcessId = Get-ManagedSshdProcessId
        Stop-ScheduledTask -TaskName $script:TaskName `
            -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
        $stopAttemptsRemaining = 50
        do {
            $task = Get-ScheduledTask -TaskName $script:TaskName `
                -TaskPath $script:TaskPath -ErrorAction SilentlyContinue
            if ($null -eq $task -or $task.State -ne 'Running') {
                break
            }
            Start-Sleep -Milliseconds 100
            $stopAttemptsRemaining--
        } while ($stopAttemptsRemaining -gt 0)
        if ($null -ne $task -and $task.State -eq 'Running') {
            throw 'The GRDOpenSSH task did not stop; removal was aborted.'
        }
        if ($managedProcessId -ne 0) {
            $processStopAttemptsRemaining = 50
            while ($null -ne (Get-Process -Id $managedProcessId `
                    -ErrorAction SilentlyContinue) -and
                $processStopAttemptsRemaining -gt 0) {
                Start-Sleep -Milliseconds 100
                $processStopAttemptsRemaining--
            }
            if ($null -ne (Get-Process -Id $managedProcessId `
                    -ErrorAction SilentlyContinue)) {
                throw 'The managed sshd process did not stop; removal aborted.'
            }
        }
        Unregister-ScheduledTask -TaskName $script:TaskName `
            -TaskPath $script:TaskPath -Confirm:$false
    }
    if ($null -ne (Get-NetFirewallRule -Name $script:FirewallRuleName `
            -ErrorAction SilentlyContinue) -and $PSCmdlet.ShouldProcess(
            $script:FirewallRuleName,
            'Remove managed firewall rule'
        )) {
        Remove-NetFirewallRule -Name $script:FirewallRuleName
    }
    foreach ($groupName in $script:ManagedGroups) {
        if ($null -ne (Get-LocalGroup -Name $groupName `
                -ErrorAction SilentlyContinue) -and $PSCmdlet.ShouldProcess(
                $groupName,
                'Remove managed local group'
            )) {
            Remove-LocalGroup -Name $groupName
        }
    }
    $managedRoot = Assert-SafeManagedPath $script:AccessRoot
    if (Test-Path -LiteralPath $managedRoot -and $PSCmdlet.ShouldProcess(
            $managedRoot,
            'Remove managed config, keys and logs'
        )) {
        Remove-Item -LiteralPath $managedRoot -Recurse -Force
    }
    if ($RemoveData) {
        $sftpRoot = Assert-SafeSftpDataRemoval $Policy.SftpRoot
        if (Test-Path -LiteralPath $sftpRoot -and $PSCmdlet.ShouldProcess(
                $sftpRoot,
                'PERMANENTLY remove SFTP data'
            )) {
            Remove-Item -LiteralPath $sftpRoot -Recurse -Force
        }
    } else {
        Write-Output "SFTP data preserved at $($Policy.SftpRoot)."
    }
    Write-Output 'GRD remote access removed. OpenSSH Server was preserved.'
}

try {
    if ($Mode -eq 'SelfTest') {
        Invoke-SelfTest
        exit 0
    }
    $policy = Read-RemoteAccessPolicy $PolicyPath
    Show-PolicySummary $policy
    switch ($Mode) {
        'Validate' {
            Write-Output 'Policy validation: OK (no system changes).'
        }
        'Plan' {
            if (-not (Test-IsWindows)) {
                throw 'Plan is available only on Windows.'
            }
            foreach ($entry in $policy.Users) {
                try {
                    [void](Get-StandardLocalUser `
                        -Name $entry.Name `
                        -Permission $entry.Permission
                    )
                    Write-Output "[READY] local account $($entry.Name)"
                } catch {
                    Write-Output "[BLOCKED] $($_.Exception.Message)"
                }
            }
            Write-Output 'Plan complete (no system changes).'
        }
        'Audit' {
            if (-not (Invoke-Audit $policy)) {
                exit 1
            }
        }
        'Apply' {
            if (-not (Test-IsWindows)) {
                throw 'Apply is available only on Windows.'
            }
            if ($WhatIfPreference) {
                Write-Output 'WhatIf requested; showing policy only.'
            } else {
                Invoke-Apply $policy
            }
        }
        'Remove' {
            if (-not (Test-IsWindows)) {
                throw 'Remove is available only on Windows.'
            }
            Invoke-Remove $policy
        }
    }
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
