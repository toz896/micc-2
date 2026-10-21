@echo off
REM ============================================================
REM Compila VoiceEQMic.dll (VST2, 64 bits) con MinGW-w64 (g++ x86_64)
REM ============================================================
if not exist build mkdir build

g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ ^
    -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX ^
    -o build\VoiceEQMic.dll src\plugin.cpp src\gui_win32.cpp ^
    -luser32 -lgdi32 -lcomdlg32
if errorlevel 1 (
  echo.
  echo *** ERROR de compilacion. Copia el mensaje de arriba y pegalo en el chat. ***
  exit /b 1
)
echo.
echo Listo: build\VoiceEQMic.dll  (x64)
echo Copialo a: C:\Program Files\EqualizerAPO\VSTPlugins\
