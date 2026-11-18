# Builds (once) and runs the advertisement sniffer adv.cs. Usage: adv.ps1 [-Seconds 10] [-Name Kindle]
param([int]$Seconds = 10, [string]$Name = 'Kindle')
$exe = Join-Path $PSScriptRoot '..\out\adv.exe'
if (-not (Test-Path $exe)) {
	$md = "$env:WINDIR\System32\WinMetadata"
	$fw = [System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
	& "$fw\csc.exe" /nologo /out:$exe "/r:$md\Windows.Devices.winmd" "/r:$md\Windows.Foundation.winmd" "/r:$md\Windows.Storage.winmd" `
		"/r:$fw\System.Runtime.WindowsRuntime.dll" "/r:$fw\System.Runtime.dll" `
		"/r:$fw\System.Runtime.InteropServices.WindowsRuntime.dll" (Join-Path $PSScriptRoot 'adv.cs')
}
& $exe $Seconds $Name
