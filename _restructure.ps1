# Restructure the Smash mod library into sibling LayeredFS title folders.
# COPY-based: leaves 00040000000EDF00_Backup untouched as a safety net.
$ErrorActionPreference = 'Stop'
$titles = 'E:\luma\titles'
$srcRoot = Join-Path $titles '00040000000EDF00_Backup'
$prefix  = '00040000000EDF00_'

$log = @()
function Log($m){ $script:log += $m; Write-Host $m }

Get-ChildItem $srcRoot -Directory -Force | ForEach-Object {
    $src   = $_.FullName
    $name  = $_.Name                      # pretty name, e.g. "Eclipse smash"
    $dest  = Join-Path $titles ($prefix + $name)
    $romfs = Join-Path $dest 'romfs'

    if (Test-Path $dest) { Log "SKIP (exists): $name"; return }

    Log "=== $name -> $($prefix + $name) ==="
    New-Item -ItemType Directory -Path $romfs -Force | Out-Null

    foreach ($c in Get-ChildItem $src -Force) {
        if ($c.PSIsContainer -and $c.Name -eq 'codes') {
            # Code patch folder: lift the USA .ips out to <dest>\code.ips
            $ips = Get-ChildItem $c.FullName -Filter *.ips -File -ErrorAction SilentlyContinue |
                   Sort-Object @{e={$_.Name -notlike '*USA*'}}, Name | Select-Object -First 1
            if ($ips) { Copy-Item $ips.FullName (Join-Path $dest 'code.ips') -Force; Log "  code.ips  <- codes\$($ips.Name)" }
            else      { Log "  WARN: codes\ had no .ips" }
        }
        elseif (-not $c.PSIsContainer -and $c.Extension -eq '.ips') {
            Copy-Item $c.FullName (Join-Path $dest 'code.ips') -Force; Log "  code.ips  <- $($c.Name)"
        }
        elseif (-not $c.PSIsContainer -and $c.Extension -eq '.txt') {
            Copy-Item $c.FullName (Join-Path $dest $c.Name) -Force      # readme stays at mod root
        }
        else {
            Copy-Item $c.FullName (Join-Path $romfs $c.Name) -Recurse -Force  # asset -> romfs\
            Log "  romfs\$($c.Name)"
        }
    }

    # Display-name marker (read by the app for the list + return-rename).
    Set-Content -Path (Join-Path $dest 'modname.txt') -Value $name -NoNewline -Encoding ASCII
}

Log ""
Log "DONE. New sibling folders:"
Get-ChildItem $titles -Directory -Filter '00040000000EDF00_*' -Force | ForEach-Object { Log ("  " + $_.Name) }
$log -join "`n" | Set-Content -Path 'C:\Users\Felip\Desktop\3dsmods\_restructure.log' -Encoding ASCII
