@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
rc /nologo resource.rc
cl /nologo /O1 /MT /EHsc /GL /DNDEBUG /utf-8 main.cpp resource.res /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib /OUT:cockroach.exe
