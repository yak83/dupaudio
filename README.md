# DupAudio
Windows 11[^1] app to duplicate audio output across multiple output devices. Intended for training scenarios where a trainer needs to listen in on a call without native application support for multiple outputs.

Author: Mike Fowler
Date: 23/11/2026
Version: 1.0

Details:
- A source output must be selected. This can be the primary audio output, or optionally
  a virtual output e.g. VB-Audio (https://vb-audio.com/Cable/). The audio stream is
  captured using WASAPI loopback
- Captured audio data is fed into a unique ring buffer per duplicated output device
- A thread per device services the local ring buffer, outputting the audio stream
  based on a hardware clock to prevent drift. Sample corrections are applied to
  maintain the target latency over longer sessions
- Devices reconnect based on selected name or ID, so an unexpected disconnect should
  be rapidly resolved with simply reconnecting the device to the same USB port
- If a duplicate output source becomes the default Windows output device the
  duplication is stopped to prevent a "feedback" scenario
- A basic UI is provided using a right-click NotifyIcon tray menu
- Configuration can be preset through a dupaudio.ini file. Additional settings are
  available within the file, e.g. setting target latency
- No installation required, just copy and run!

Usage:
- dupaudio.exe                   Runs app in the background. Right-click the tray NotifyIcon for basic settings
- dupaudio.exe --console         As above, but with debug console output
- dupaudio.exe --list            Lists the currently-available output devices to the console and quits
- dupaudio.exe --stop            Stops a running instance
- dupaudio.exe --config <file>   Uses the config file <file> rather than dupaudio.ini

Build Info:
- Windows-only; targetted for and tested on Windows 11 (Version 10.0.26300 Build 26300)
- C++ 17
- MS Visual C++ 2026 tools
- Requires standard libraries: ole32.lib avrt.lib shell32.lib user32.lib advapi32.lib gdi32.lib
- No other dependencies, no external packages/libraries
- basic build steps: (compile resources, compile code, clean up temporary files)
  1. `rc /nologo dupaudio.rc`
  2. `cl /nologo /O2 /EHsc /std:c++17 /W4 /MT /DUNICODE /D_UNICODE dupaudio.cpp dupaudio.res /Fe:dupaudio.exe /link /SUBSYSTEM:WINDOWS ole32.lib avrt.lib shell32.lib user32.lib advapi32.lib gdi32.lib`
  3. `del /q dupaudio.obj dupaudio.res 2>nul`


[^1]: Windows-only; targeted at and tested on Windows 11 (Version 10.0.26300 Build 26300). Not built for portability - good luck on other platforms.
