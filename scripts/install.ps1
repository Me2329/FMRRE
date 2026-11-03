<#
  Put TWRP v2 on a phone from Windows (PowerShell 5.1 or 7).

    powershell -ExecutionPolicy Bypass -File scripts\install.ps1 [-Image twrpv2-recovery.img] [options]

  Fastboot phones (Pixel, Xiaomi, OnePlus, Motorola, Nothing, Sony, ...):
    default        fastboot boot  - runs TWRP v2 once, writes NOTHING
    -Permanent     flash it (recovery, vendor_boot or boot - detected)
  Samsung:
    -Samsung       build an Odin package (AP slot) with recovery.img + a disabled vbmeta

  Options: -Partition NAME, -BothSlots, -Vbmeta stock_vbmeta.img, -DisableVbmeta, -NoVbmeta, -Yes, -DryRun
  Fastboot phones need Android platform-tools (fastboot.exe, adb.exe) in PATH and the Google USB driver.
#>
param(
    [string]$Image = "",
    [switch]$Permanent,
    [switch]$Samsung,
    [string]$Partition = "",
    [switch]$BothSlots,
    [string]$Vbmeta = "",
    [switch]$DisableVbmeta,
    [switch]$NoVbmeta,
    [switch]$Yes,
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
# .NET file APIs resolve relative paths against the process directory, not PowerShell's location.
[Environment]::CurrentDirectory = (Get-Location).Path

function Say($m)  { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "xx  $m" -ForegroundColor Red; exit 1 }

# Run a native tool and capture stdout+stderr as text. Windows PowerShell 5.1 turns redirected
# stderr into errors, which $ErrorActionPreference = Stop would make fatal - so relax it here.
function Native([string]$exe, [string[]]$a) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & $exe @a 2>&1 | ForEach-Object { "$_" } } catch { $out = @() } finally { $ErrorActionPreference = $old }
    return (@($out) -join "`n")
}
# Run a step that changes the phone (output goes straight to the console).
function Run([string]$exe, [string[]]$a) {
    Write-Host "   > $exe $($a -join ' ')"
    if ($DryRun) { return }
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @a } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { Die "$exe failed (exit $LASTEXITCODE)" }
}
function Ask($q) {
    if ($Yes) { return }
    if ((Read-Host "$q [y/N]") -notmatch '^[yY]$') { Die "Cancelled." }
}

# Unsigned vbmeta, no descriptors, flags = 3 (same bytes as tools/odin_pack.py vbmeta-disabled).
function New-DisabledVbmeta([string]$path) {
    $b = New-Object byte[] 4096
    [Text.Encoding]::ASCII.GetBytes("AVB0").CopyTo($b, 0)
    $b[7] = 1          # required libavb major version = 1 (big-endian u32 at offset 4)
    $b[123] = 3        # flags (big-endian u32 at offset 120): hashtree + verification disabled
    [Text.Encoding]::ASCII.GetBytes("avbtool 1.3.0 (twrpv2)").CopyTo($b, 128)
    [IO.File]::WriteAllBytes($path, $b)
}
function Set-VbmetaDisabled([string]$src, [string]$dst) {
    $b = [IO.File]::ReadAllBytes($src)
    if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne "AVB0") { Die "$src is not a vbmeta image" }
    $b[123] = [byte]($b[123] -bor 3)
    [IO.File]::WriteAllBytes($dst, $b)
}
function Get-Magic([string]$path) {
    $f = [IO.File]::OpenRead($path)
    try { $h = New-Object byte[] 8; [void]$f.Read($h, 0, 8) } finally { $f.Close() }
    return [Text.Encoding]::ASCII.GetString($h)
}

# ---- image ---------------------------------------------------------------------
if (-not $Image) {
    foreach ($c in "twrpv2-recovery.img", "twrpv2-boot.img", "twrpv2-vendor_boot.img") { if (Test-Path $c) { $Image = $c; break } }
}
if (-not $Image -or -not (Test-Path $Image)) {
    Die "No TWRP v2 image. Build twrpv2-recovery.img from YOUR phone's stock recovery/boot image first (see README), then: scripts\install.ps1 -Image twrpv2-recovery.img"
}
$Image = (Resolve-Path $Image).Path
$magic = Get-Magic $Image
if ($magic -eq "ANDROID!") { $kind = "boot" } elseif ($magic -eq "VNDRBOOT") { $kind = "vendor_boot" } else { Die "$Image is not an Android boot/recovery image" }
Say "Image: $(Split-Path $Image -Leaf) ($kind format, $([math]::Round((Get-Item $Image).Length / 1MB, 1)) MB)"

# ---- Samsung: Odin package -------------------------------------------------------
if ($Samsung) {
    if ($kind -ne "boot") { Die "Samsung needs a recovery.img (from the AP file of your firmware)" }
    $dir = "out\samsung"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $out = Join-Path $dir "twrpv2-recovery-odin.tar.md5"
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py -and (Native $py.Source @("--version")) -notmatch "Python 3") { $py = $null }   # Microsoft Store stub
    if ($py) {
        $vb = @("--vbmeta-disabled")
        if ($Vbmeta) { $vb = @("--vbmeta", (Resolve-Path $Vbmeta).Path) }
        if ($NoVbmeta) { $vb = @("--no-vbmeta") }
        Run $py.Source (@("tools\odin_pack.py", "tar", "--recovery", $Image) + $vb + @("-o", $out))
    } else {
        # No Python: Windows 10 1803+ ships bsdtar as tar.exe.
        $stage = Join-Path $dir "stage"
        Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
        New-Item -ItemType Directory $stage | Out-Null
        Copy-Item $Image (Join-Path $stage "recovery.img")
        $files = @("recovery.img")
        if (-not $NoVbmeta) {
            if ($Vbmeta) { Set-VbmetaDisabled (Resolve-Path $Vbmeta).Path (Join-Path $stage "vbmeta.img") }
            else { New-DisabledVbmeta (Join-Path $stage "vbmeta.img") }
            $files += "vbmeta.img"
        }
        $tar = Join-Path $dir "twrpv2-recovery-odin.tar"
        Run "tar.exe" (@("--format=ustar", "-cf", $tar, "-C", $stage) + $files)
        if (-not $DryRun) {
            $md5 = (Get-FileHash $tar -Algorithm MD5).Hash.ToLower()
            Move-Item -Force $tar $out
            [IO.File]::AppendAllText((Resolve-Path $out).Path, "$md5  twrpv2-recovery-odin.tar`n")
            Say "$out (md5 $md5)"
        }
        Remove-Item -Recurse -Force $stage
    }
    Write-Host @"

   Odin:
     1. Phone: power off, hold Volume Up + Volume Down, plug in the USB cable, press Volume Up (Download mode).
        The bootloader must be unlocked (OEM unlocking ON) and the KG state must not be 'Prenormal'.
     2. Odin: AP = $out   Options: UNTICK 'Auto Reboot', then Start.
     3. When it says PASS: hold Volume Down + Power until the screen goes black, then IMMEDIATELY
        Volume Up + Power (+ Bixby on older models) until TWRP v2 appears.
        If Android boots first, it puts the stock recovery back.
"@
    exit 0
}

# ---- Fastboot -------------------------------------------------------------------
$fb = Get-Command fastboot -ErrorAction SilentlyContinue
if (-not $fb) { Die "fastboot.exe not found. Install Android platform-tools and add the folder to PATH." }
$fb = $fb.Source
function GetVar([string]$n) {
    $o = Native $fb @("getvar", $n)
    if ($o -match "(?m)^$([regex]::Escape($n)):\s*(\S+)") { return $Matches[1] }
    return ""
}
function FbDevices { return (Native $fb @("devices")).Trim() }

if (-not $DryRun -and -not (FbDevices)) {
    $adb = Get-Command adb -ErrorAction SilentlyContinue
    if ($adb -and ((Native $adb.Source @("get-state")) -match "^(device|recovery)")) {
        $man = (Native $adb.Source @("shell", "getprop", "ro.product.manufacturer")).Trim().ToLower()
        if ($man -eq "samsung") { Die "This is a Samsung (no fastboot): run  scripts\install.ps1 -Samsung  and flash the package with Odin." }
        Say "Phone on adb ($man) - rebooting to the bootloader"
        Run $adb.Source @("reboot", "bootloader")
        for ($i = 0; $i -lt 90 -and -not (FbDevices); $i++) { Start-Sleep 1 }
    }
    if (-not (FbDevices)) {
        Die "No phone in fastboot mode. Power off, hold Volume Down + Power, plug it in (Samsung? use -Samsung)."
    }
}
$product = GetVar "product"; $unlocked = GetVar "unlocked"; $slot = GetVar "current-slot"
Say "Phone: $product  unlocked: $unlocked  slot: $(if ($slot) { $slot } else { 'none' })"
if ($unlocked -eq "no") { Die "The bootloader is LOCKED. Unlocking erases the phone: fastboot flashing unlock (some brands: fastboot oem unlock)" }
if ((GetVar "is-userspace") -eq "yes") {
    Say "In fastbootd - switching to the bootloader"
    Run $fb @("reboot", "bootloader")
    for ($i = 0; $i -lt 60 -and -not (FbDevices); $i++) { Start-Sleep 1 }
}
if (-not $Partition) {
    if ($kind -eq "vendor_boot") { $Partition = "vendor_boot" }
    elseif ((GetVar "partition-size:recovery") -or (GetVar "partition-size:recovery_a")) { $Partition = "recovery" }
    else { $Partition = "boot" }
}
Say "Target partition: $Partition"

if (-not $Permanent) {
    if ($kind -eq "vendor_boot") { Die "vendor_boot images can't be test-booted: use -Permanent (or build from recovery.img / boot.img)" }
    Say "Test boot: TWRP v2 runs once from RAM, nothing on the phone is changed."
    Run $fb @("boot", $Image)
    Write-Host "`n   If it works:  scripts\install.ps1 -Image $(Split-Path $Image -Leaf) -Permanent"
    exit 0
}
if ($Partition -ne "recovery") {
    Warn "This phone has no recovery partition: TWRP v2 goes into '$Partition'."
    Warn "Only safe if the image was built from THIS phone's own $Partition.img. Keep the stock one: fastboot flash $Partition $Partition.img restores it."
}
$where = $Partition
if ($slot) { if ($BothSlots) { $where = "$Partition (both slots)" } else { $where = "$Partition (slot $slot)" } }
Ask "Flash $(Split-Path $Image -Leaf) to $where?"
if ($BothSlots -and $slot) { Run $fb @("flash", "--slot=all", $Partition, $Image) } else { Run $fb @("flash", $Partition, $Image) }
if ($Vbmeta) {
    Run $fb @("--disable-verity", "--disable-verification", "flash", "vbmeta", (Resolve-Path $Vbmeta).Path)
} elseif ($DisableVbmeta) {
    New-Item -ItemType Directory -Force out | Out-Null
    New-DisabledVbmeta "out\vbmeta_disabled.img"
    Run $fb @("--disable-verity", "--disable-verification", "flash", "vbmeta", "out\vbmeta_disabled.img")
}
Say "Rebooting into TWRP v2"
$r = Native $fb @("reboot", "recovery")
if ($r -match "FAILED|unknown") { Warn "This bootloader can't 'reboot recovery': hold Volume Up + Power while it restarts."; Run $fb @("reboot") }
