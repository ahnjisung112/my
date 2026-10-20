@echo off
cd /d "%~dp0"
pip install pyinstaller
pyinstaller --noconsole --onefile --name 밝기조절 brightness.py
echo.
echo 완료: dist\밝기조절.exe
pause
