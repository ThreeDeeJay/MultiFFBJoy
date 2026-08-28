# MultiFFBJoy

DirectInput FFB bridge for BeamNG.drive and other UDP-capable clients.

## Source layout

- `main.cpp` — application startup/shutdown and global state
- `common.h` — shared types, constants and API declarations
- `gui.cpp` — Win32 GUI and thread-safe log/status dispatch
- `udp.cpp` — UDP server and command parser
- `device.cpp` — DirectInput enumeration, selection and acquisition
- `ffb.cpp` — DirectInput effects, watchdog and recovery
- `preset.cpp` — `.fff` parsing, polygon zone selection and PRND reference
- `configuration.cpp/.h` — tab-indented profile configuration and inheritance

## Important behavior preserved

The experimentally validated SideWinder Force Feedback 2 mapping is retained:

- DirectInput spring `condition[0]` receives logical Y.
- DirectInput spring `condition[1]` receives logical X.
- Reverse and Neutral use spring equilibria inside their zones.
- Park and Drive use spring equilibria at the top/bottom travel limits while
  keeping X centered, reproducing the observed straight PRND behavior.

Spring forcefields are persistent state. If the DirectInput device is lost,
re-acquisition restores the last active spring automatically.

## Configuration.txt

The resolver supports general-to-specific inheritance. `Transmission` is a
sibling of `Vehicle`, so transmission defaults can be combined with vehicle
and configuration overrides.

Example:

```text
Profiles
\tBeamNG.drive
\t\tVehicle
\t\t\tCar
\t\t\t\tmiramar=PRND21
\t\t\t\t\tluxe_A=PRNDL
\t\t\tAircraft=Flightstick
\t\tTransmission
\t\t\tAutomatic=PRND
\t\t\tManual=5RDR
```

Node names may be quoted. Quoted names are matched case-insensitively, while
quotes are treated as syntax rather than part of the key. This permits either
language-specific names or BeamNG internal codenames.

`Configuration.txt` and `forcefields\*.fff` are resolved relative to the helper executable directory, not the game directory or process working directory. The helper logs the exact configuration path at startup. A single helper instance owns UDP port 65458.

## UDP commands

- `PING`
- `PROFILE <preset|preset.fff>` — loads and starts position-aware tracking
- `STOP`
- `REACQUIRE`
- `CENTER`
- `SPRING <0..1>`
- `TEST_FFB <x> <y>`
- `VEHICLE|<game>|<vehicleType>|<vehicle>|<configuration>|<transmission>|<gearLayout>` — reloads `Configuration.txt`, resolves the most-specific matching profile, and activates the resulting preset.

## Safety / concurrency changes

- All DirectInput device/effect operations are serialized by one recursive FFB
  mutex, preventing the watchdog, UDP thread and preset monitor from touching
  COM effect pointers concurrently.
- Re-acquisition is single-flight via `g_reacquiring`.
- UDP `WSAStartup`/`WSACleanup` is balanced.
- GUI updates from worker threads are posted to the GUI thread.
- UTF-8 to UTF-16 conversion no longer writes past the destination buffer.
- `.fff` zone selection uses the actual polygon instead of only its bounding box.
- `PROFILE` starts the preset monitor rather than applying only one sample.
- Forcefield springs mark themselves persistent, preventing the UDP safety timer
  from immediately stopping a valid position-aware profile.
