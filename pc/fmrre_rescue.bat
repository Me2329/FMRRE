@echo off
rem FMRRE PC rescue for Windows: double-click for the window, or run "fmrre_rescue.bat fix" in a terminal.
rem Needs Python 3 (python.org; tick "Add python.exe to PATH") and Android platform-tools (adb.exe, fastboot.exe)
rem in PATH or in a "platform-tools" folder next to this file.
where py >nul 2>nul && (py -3 "%~dp0fmrre_rescue.py" %* & goto :end)
where python >nul 2>nul && (python "%~dp0fmrre_rescue.py" %* & goto :end)
echo Python 3 is not installed: get it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
pause
:end
