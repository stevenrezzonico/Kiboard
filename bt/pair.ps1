# Builds (once) and runs pair.cs: pairs this PC with the Kindle's Bluetooth keyboard, or removes the pairing.
# Usage: pair.ps1 [-Name Kindle] [-Unpair]
param([string]$Name = 'Kindle', [switch]$Unpair)
$exe = Join-Path $PSScriptRoot '..\out\pair.exe'
$src = Join-Path $PSScriptRoot 'pair.cs'
if (-not (Test-Path $exe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
	$md = "$env:WINDIR\System32\WinMetadata"
	$fw = [System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
	& "$fw\csc.exe" /nologo /out:$exe "/r:$md\Windows.Devices.winmd" "/r:$md\Windows.Foundation.winmd" "/r:$md\Windows.Storage.winmd" `
		"/r:$fw\System.Runtime.dll" "/r:$fw\System.Runtime.InteropServices.WindowsRuntime.dll" $src
}
if ($Unpair) { & $exe $Name unpair } else { & $exe $Name }
