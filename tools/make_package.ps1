# Assemble the player package from a finished build: dist\LP2Netplay-<version>\ and dist\LP2Netplay-<version>.zip
# (LP2NetSetup.exe, HOW TO TEST.txt, TWO-PC TEST.txt, the script installer in manual\ as a fallback, and linux\
# for Linux and Steam Deck; no game files), plus dist\LP2Netplay-<version>-linux.tar.gz. The version comes from the
# built xlive.dll, i.e. project(VERSION) in CMakeLists.txt. CI runs this on every push.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$dll = Get-Item (Join-Path $build 'xlive.dll')
$exe = Get-Item (Join-Path $build 'LP2NetSetup.exe')
$version = $dll.VersionInfo.ProductVersion
if (-not $version -or $exe.VersionInfo.ProductVersion -ne $version) {
    throw "build\xlive.dll ($version) and build\LP2NetSetup.exe ($($exe.VersionInfo.ProductVersion)) need the same version: run tools\build.bat"
}

$name = "LP2Netplay-$version"
$out = Join-Path $root "dist\$name"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out, (Join-Path $out 'manual'), (Join-Path $out 'linux') | Out-Null
Copy-Item $exe.FullName $out
$docs = 'HOW TO TEST.txt', 'TWO-PC TEST.txt'
$docs | ForEach-Object { Copy-Item (Join-Path $PSScriptRoot "package\$_") $out }
Copy-Item $dll.FullName (Join-Path $out 'manual')
Copy-Item (Join-Path $PSScriptRoot 'package\*') (Join-Path $out 'manual') -Exclude $docs
Copy-Item $dll.FullName (Join-Path $out 'linux')
Copy-Item (Join-Path $PSScriptRoot 'linux\*') (Join-Path $out 'linux')

$commit = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { (git -C $root rev-parse HEAD 2>$null) }
@"
LP2 Netplay $version
commit  $commit
built   $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm')) UTC
sha256  $((Get-FileHash $exe.FullName).Hash)  LP2NetSetup.exe
sha256  $((Get-FileHash $dll.FullName).Hash)  manual\xlive.dll, linux\xlive.dll (also inside LP2NetSetup.exe)
"@ | Set-Content -Encoding ascii (Join-Path $out 'VERSION.txt')

$zip = "$out.zip"
$tgz = "$out-linux.tar.gz"
Remove-Item $zip, $tgz -ErrorAction SilentlyContinue
# Python writes both archives with Unix permissions (the Linux scripts stay executable); without it, a plain zip.
$python = Get-Command python, python3 -ErrorAction SilentlyContinue | Select-Object -First 1
if ($python) {
    & $python.Source -I (Join-Path $PSScriptRoot 'pack_dist.py') $out
    if ($LASTEXITCODE -ne 0) { throw "pack_dist.py failed ($LASTEXITCODE)" }
    "package: $tgz"
}
else {
    Write-Warning 'python not found: plain zip only, and Linux players must run the scripts with bash'
    Compress-Archive -Path "$out\*" -DestinationPath $zip
}
Get-ChildItem $out -Recurse -File | ForEach-Object { [pscustomobject]@{ File = $_.FullName.Substring($out.Length + 1); Length = $_.Length } }
"package: $zip"
if ($env:GITHUB_OUTPUT) { "version=$version" | Add-Content -Encoding utf8 $env:GITHUB_OUTPUT }
