@echo off
cd /d "%~dp0"
start "" chrome http://localhost:8000
py serve.py
pause
