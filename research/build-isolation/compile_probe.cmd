@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
"D:\Dev\Tools\LLVM\bin\clang-cl.exe" /nologo /std:c++20 /EHsc /MT /utf-8 lpac_probe.cpp /Fe:lpac_probe.exe /link advapi32.lib userenv.lib ws2_32.lib
