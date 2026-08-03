#!/usr/bin/env bash
set -euo pipefail

exe_path="${1:-build/release/grd.exe}"
package_dir="${2:-dist/windows}"

if [[ ! -f "$exe_path" ]]; then
    echo "Missing Windows executable: $exe_path" >&2
    exit 1
fi

mkdir -p "$package_dir"
cp "$exe_path" "$package_dir/grd.exe"
cp README.md SECURITY.md REMOTE_ACCESS.md "$package_dir/"
mkdir -p "$package_dir/scripts"
cp scripts/grd-remote-access.ps1 \
   scripts/grd-remote-access.example.psd1 \
   "$package_dir/scripts/"

mapfile -t runtime_dlls < <(
    ldd "$exe_path" |
        awk '{
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^\/ucrt64\/bin\/.*\.dll$/) {
                    print $i
                }
            }
        }' |
        sort -u
)

if [[ ${#runtime_dlls[@]} -eq 0 ]]; then
    echo "No UCRT64 runtime DLLs were discovered for $exe_path" >&2
    exit 1
fi

for runtime_dll in "${runtime_dlls[@]}"; do
    cp "$runtime_dll" "$package_dir/"
done

echo "Packaged grd.exe, remote-access tools and ${#runtime_dlls[@]} runtime DLLs in $package_dir"
