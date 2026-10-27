@echo off
rem Headless Ghidra import + auto-analysis + post scripts, into re-work\ghidra\LP2Net.
rem usage: ghidra_analyze.bat <binary> <log> <out dir> <post script> [max heap, default 6G]
rem Java is called directly: from the Claude app the nested Ghidra launchers drop the settings-dir options and
rem Ghidra's OSGi cache under AppData fails. All Ghidra state lives under re-work\ghidra_user instead.
setlocal
set "G=D:\Tools\ghidra_12.2_PUBLIC"
rem Ghidra rejects ".." in a project path, so resolve the repo root first
for %%I in ("%~dp0..") do set "R=%%~fI\re-work"
set "U=%R%\ghidra_user"
set "HEAP=%~5"
if "%HEAP%"=="" set "HEAP=6G"
for %%d in (home settings cache temp) do if not exist "%U%\%%d" mkdir "%U%\%%d"
if not exist "%R%\ghidra" mkdir "%R%\ghidra"
"D:\Tools\jdk-25.0.4.1+1\bin\java.exe" -XX:ParallelGCThreads=2 -XX:CICompilerCount=2 -Xmx%HEAP% ^
 -Djava.system.class.loader=ghidra.GhidraClassLoader -Dfile.encoding=UTF8 -Duser.country=US -Duser.language=en ^
 -Dsun.java2d.d3d=false -Djava.awt.headless=true ^
 "-Duser.home=%U%\home" "-Dapplication.settingsdir=%U%\settings" "-Dapplication.cachedir=%U%\cache" ^
 "-Dapplication.tempdir=%U%\temp" "-Djava.io.tmpdir=%U%\temp" ^
 -cp "%G%\Ghidra\Framework\Utility\lib\Utility.jar" ghidra.Ghidra ghidra.app.util.headless.AnalyzeHeadless ^
 "%R%\ghidra" LP2Net -import "%~1" -overwrite -max-cpu 4 -analysisTimeoutPerFile 14400 ^
 -scriptPath "%~dp0ghidra_scripts" -postScript %4 "%~3" > "%~2" 2>&1
echo DONE exit %ERRORLEVEL%>> "%~2"
