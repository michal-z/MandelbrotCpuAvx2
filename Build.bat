@echo off
set NAME=MandelbrotCpuAvx2
if exist %NAME%.exe del %NAME%.exe
cl /Zi /O2 /EHsc- /fp:fast /arch:AVX2 %NAME%.cpp /link kernel32.lib user32.lib gdi32.lib /incremental:no /opt:ref
if exist %NAME%.obj del %NAME%.obj
if "%1" == "run" if exist %NAME%.exe (.\%NAME%.exe)
