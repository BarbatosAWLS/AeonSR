param([string]$Dest = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = "$root\external\D3D12Core" }

$version = '1.619.0'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$version/microsoft.direct3d.d3d12.$version.nupkg"
$inside = 'build/native/bin/x64/D3D12Core.dll'
$sha = '1cabfee6593688772368b785f2755dcffa1e5b68280764ad1501d80d859c92db'
$target = "$Dest\D3D12Core.dll"

function Sha($path) {
    $h = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return (($h.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $stream.Dispose() }
}

if (-not $sha) {
    Write-Host 'No SHA-256 is pinned in this script yet. Download once, note the value it prints, and put it in.'
}
if ($sha -and (Test-Path $target) -and (Sha $target) -eq $sha) { Write-Host "  in place: $target"; exit 0 }

$work = Join-Path ([IO.Path]::GetTempPath()) "fetch_agility_$PID"
New-Item -ItemType Directory -Force $work | Out-Null
try {
    Write-Host "  $url"
    Invoke-WebRequest -Uri $url -OutFile "$work\agility.nupkg" -UseBasicParsing
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead("$work\agility.nupkg")
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $inside } | Select-Object -First 1
        if ($null -eq $entry) { Write-Host "  the package does not hold $inside"; exit 1 }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$work\D3D12Core.dll", $true)
    } finally { $zip.Dispose() }

    $sig = Get-AuthenticodeSignature "$work\D3D12Core.dll"
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft') {
        Write-Host "  D3D12Core.dll is not validly signed by Microsoft ($($sig.Status)); nothing was kept"; exit 1
    }
    $got = Sha "$work\D3D12Core.dll"
    if ($sha -and $got -ne $sha) {
        Write-Host "  CHECKSUM MISMATCH: got $got, want $sha; nothing was kept"; exit 1
    }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    Copy-Item "$work\D3D12Core.dll" $target -Force
    Write-Host "  kept $target  (signed by Microsoft, version $((Get-Item $target).VersionInfo.FileVersion), sha256 $got)"
    if (-not $sha) { Write-Host '  (no checksum pinned yet: put the sha256 above into this script)' }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
