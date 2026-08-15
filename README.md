# MultiFFBJoy

MultiFFBJoy is a generic Windows Force Feedback joystick helper.

It is intentionally independent of any particular game. A game or other
application sends commands to MultiFFBJoy, and MultiFFBJoy outputs the
corresponding Force Feedback effects through DirectInput.

## Prototype

The first prototype provides:

- Win32 GUI
- DirectInput device enumeration
- Automatic selection of the first suitable FFB joystick
- Device and FFB status display
- Received command log
- Localhost UDP command interface
- Basic 2-axis spring/autocentering
- FFB safety timeout
- Clean shutdown of FFB

## Command Interface

MultiFFBJoy listens for UDP commands on:

127.0.0.1:47777

### PING

Tests communication.

    PING

### STOP

Stops all Force Feedback.

    STOP

### CENTER

Enables a centered spring at default strength.

    CENTER

### SPRING

Sets the spring position and strength.

    SPRING <x> <y> <strength>

Example:

    SPRING 0.5 -0.25 0.75

Coordinates are normalized from -1.0 to +1.0.

The logical coordinate system is:

             +Y
              ^
              |
        -X <--+--> +X
              |
              v
             -Y

Strength ranges from 0.0 to 1.0.

## Device Selection

The helper does not contain any hard-coded hardware IDs.

At startup it enumerates DirectInput game controllers and looks for the
first device that:

1. Supports Force Feedback.
2. Has at least two axes.

The selected device is displayed in the GUI.

## Safety

If commands stop arriving, MultiFFBJoy automatically stops Force Feedback
after a short timeout.

FFB is also stopped when the application exits or receives a STOP command.

## Project Structure

    MultiFFBJoy/
    |
    +-- README.md
    +-- MultiFFBJoy.sln
    +-- MultiFFBJoy.vcxproj
    |
    +-- src/
    |   +-- main.cpp
    |
    +-- docs/
        +-- protocol.md

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022
- Desktop development with C++
- Windows SDK
- DirectInput 8 development libraries

## Future Plans

The helper will eventually support:

- Spring
- Constant force
- Damper
- Friction
- Multiple simultaneous effects
- Configurable FFB profiles
- H-pattern shifter force fields
- Gear detents
- Aircraft flight-stick centering
- Other game/application clients

The FFB helper itself will remain game-agnostic.