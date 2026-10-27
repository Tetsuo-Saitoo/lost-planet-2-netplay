# Assemble the copy-to-another-PC test package: dist\LP2Netplay-test\ and dist\LP2Netplay-test.zip
# (our xlive.dll + install scripts + instructions; no game files).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'dist\LP2Netplay-test'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $root 'build\xlive.dll') $out
Copy-Item (Join-Path $PSScriptRoot 'package\*') $out
$dll = Get-Item (Join-Path $out 'xlive.dll')
@"
LP2 Netplay test build
built   $($dll.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
sha256  $((Get-FileHash $dll.FullName).Hash)
"@ | Set-Content -Encoding ascii (Join-Path $out 'VERSION.txt')
$zip = "$out.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$out\*" -DestinationPath $zip
Get-ChildItem $out | Select-Object Name, Length
"package: $zip"
