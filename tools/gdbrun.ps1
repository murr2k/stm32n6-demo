# Bring-up helper: start ST-LINK_gdbserver, run a GDB batch against an ELF, stop the server.
# Usage: tools\gdbrun.ps1 -Elf build\mcuboot\zephyr\zephyr.elf -Ex 'load $ELF','break foo','continue','bt'
# GDB commands go to a script file, so quoting inside -Ex strings is passed through untouched.
param(
    [Parameter(Mandatory)][string]$Elf,
    [string[]]$Ex = @(),
    [switch]$ExtLoad,
    [switch]$NoReset,
    [int]$TimeoutSec = 40,
    [int]$Port = 61234
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$gs = (Get-ChildItem "C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.stlink-gdb-server.win32_*\tools\bin\ST-LINK_gdbserver.exe" | Select-Object -First 1).FullName
$cp = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin"
$gdb = "$env:USERPROFILE\zephyr-sdk-1.0.1\gnu\arm-zephyr-eabi\bin\arm-zephyr-eabi-gdb.exe"
$cap = Join-Path $root 'captures'
New-Item -ItemType Directory -Force $cap | Out-Null

$sargs = @('--stm32cubeprogrammer-path', "`"$cp`"", '--port-number', "$Port", '--apid', '1', '--halt', '--swd')
if ($NoReset) { $sargs += '--attach' } else { $sargs += '--initialize-reset' }
if ($ExtLoad) { $sargs += @('--external-init', '--extload', "`"$cp\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr`"") }

$elfp = (Resolve-Path $Elf).Path -replace '\\', '/'
$script = @('set confirm off', 'set pagination off', 'set mem inaccessible-by-default off', 'set remotetimeout 10', "target remote :$Port")
$script += ($Ex | ForEach-Object { $_ -replace '\$ELF', $elfp })
Set-Content -Path "$cap\gdb.cmds" -Value $script -Encoding ascii

$srv = Start-Process -FilePath $gs -ArgumentList $sargs -RedirectStandardOutput "$cap\gdbserver.out" -RedirectStandardError "$cap\gdbserver.err" -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds 3
    $g = Start-Process -FilePath $gdb -ArgumentList @('-q', '-batch', '-x', "`"$cap\gdb.cmds`"", "`"$elfp`"") -RedirectStandardOutput "$cap\gdb.out" -RedirectStandardError "$cap\gdb.err" -PassThru -WindowStyle Hidden
    if (-not $g.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $g.Id -Force
        Write-Output "[gdbrun] GDB timed out after $TimeoutSec s and was stopped"
    }
    Get-Content "$cap\gdb.out"
    Get-Content "$cap\gdb.err" | Where-Object { $_ -notmatch '^Loading section|^$' }
} finally {
    if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
}
