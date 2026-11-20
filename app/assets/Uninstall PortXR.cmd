@echo off
rem Takes VR out of every game PortXR added it to (each folder back as it was) and deletes PortXR's settings.
rem Your game scripts stay. Close your games first. Afterwards, delete this folder.
choice /m "Remove VR from every game and delete PortXR's settings"
if errorlevel 2 exit /b
start "" /wait "%~dp0PortXR.exe" --uninstall
echo.
pause
