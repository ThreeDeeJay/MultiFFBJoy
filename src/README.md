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

## PRND reference test

The GUI now includes a hard-coded PRND reference test independent of `.fff` parsing:

- `PRND Test` loads an in-memory straight PRND preset and starts the existing position-aware monitor.
- `PRND Park`, `PRND Reverse`, `PRND Neutral`, and `PRND Drive` apply one reference spring directly.

The hard-coded reference uses the supplied profile values:

- Park: center `(0,-8500)`, power `(-10000,10000)`
- Reverse: center `(0,-3500)`, power `(10000,10000)`
- Neutral: center `(0,3500)`, power `(10000,10000)`
- Drive: center `(0,8500)`, power `(-10000,10000)`

The reference zones are full-width straight rectangles in FFShifter coordinates, ordered top-to-bottom as Park, Reverse, Neutral, Drive.

### FFB2 condition-axis mapping

Observed behavior from the FFB2 showed that the old implementation put the logical vertical offset into `DICONDITION[1]`, while that condition slot produced the horizontal physical pull. The new implementation therefore translates logical FFShifter X/Y to FFB2 condition slots as:

- `condition[0]` = logical Y
- `condition[1]` = logical X

The log prints the actual condition offsets and coefficients whenever a reference zone is applied, making this easy to verify against the physical stick behavior.
