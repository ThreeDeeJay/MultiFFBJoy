# MultiFFBJoy refactored

Files:
- main.cpp          - startup/shutdown and orchestration
- common.h          - shared declarations/types/constants
- gui.cpp           - Win32 window, status, logging and test buttons
- udp.cpp           - UDP server and command parser
- device.cpp        - DirectInput enumeration, selection, acquisition and release
- ffb.cpp           - spring/constant effects, re-acquisition and watchdog

Build:
Compile all six .cpp/.h files into the same executable. The existing
DirectInput 8 / DXGUID / Winsock libraries are linked by main.cpp.

Important fix:
The previous SelectFirstSuitableDevice() contained an accidental call to
SelectFirstSuitableDevice() from inside itself. That caused recursive
re-selection, repeated effect creation/release and the apparent hang plus
short bursts of centering force. The refactored version has no such call.

Also:
- The watchdog is started exactly once by main.cpp.
- Re-acquisition cannot overlap because of g_reacquiring.
- Re-acquisition does not call Acquire() twice.
- Spring SetParameters() no longer includes DIEP_START; it downloads the
  parameters and then calls Start() once.
- ReleaseFFBDevice() preserves springPersistent so a persistent spring can
  be restored after a device loss.
- Normal STOP clears springPersistent.
- UDP shutdown and watchdog shutdown are separate.
