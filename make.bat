@echo off
set NAME=MandelbrotCpuAvx2
if exist %NAME%.exe del %NAME%.exe
cl /Zi /O2 /arch:AVX2 %NAME%.c /link kernel32.lib user32.lib gdi32.lib /incremental:no /opt:ref
if exist %NAME%.obj del %NAME%.obj
if "%1" == "run" if exist %NAME%.exe (.\%NAME%.exe)
