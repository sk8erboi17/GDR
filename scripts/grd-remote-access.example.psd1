@{
    Version = 1

    # Dedicated GRD OpenSSH service. Do not reuse GRD's 47989/47990 ports.
    Port = 47992

    # Must be a local path. The chroot root itself is never writable by a
    # remote user; writable content lives in its Exchange child directory.
    SftpRoot = 'C:\ProgramData\GRD\Sftp'

    # Each entry maps one existing, non-administrator local Windows account
    # to exactly one access profile. Use PublicKey for one client or PublicKeys
    # for up to 16 clients. Uncomment an entry only after creating the account.
    Users = @(
        # @{
        #     Name       = 'grd-file-reader'
        #     Permission = 'SftpRead'
        #     PublicKey  = 'ssh-ed25519 AAAA... device-name'
        # }
        # @{
        #     Name       = 'grd-file-writer'
        #     Permission = 'SftpWrite'
        #     PublicKey  = 'ssh-ed25519 AAAA... device-name'
        # }
        # @{
        #     Name       = 'grd-operator'
        #     Permission = 'PowerShell'
        #     PublicKeys = @(
        #         'ssh-ed25519 AAAA... first-device'
        #         'ssh-ed25519 AAAA... second-device'
        #     )
        # }
    )
}
