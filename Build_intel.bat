@echo off
set NAME=MandelbrotCpuAvx2
if exist %NAME%_intel.exe del %NAME%_intel.exe
icl /Zi /fast /EHsc- /W4 /wd4324 /arch:AVX2 %NAME%.cpp /link kernel32.lib user32.lib gdi32.lib /incremental:no /opt:ref /pdb:MandelbrotCpuAvx2_intel.pdb /out:MandelbrotCpuAvx2_intel.exe
if exist %NAME%.obj del %NAME%.obj
if "%1" == "run" if exist %NAME%_intel.exe (.\%NAME%_intel.exe)
