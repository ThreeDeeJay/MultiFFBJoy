# MultiFFBJoy UDP Protocol

This document describes the initial UDP protocol used to communicate with
MultiFFBJoy.

## Connection

MultiFFBJoy listens on:

    127.0.0.1:47777

Transport:

    UDP

Commands are plain UTF-8 text.

Each command is sent as one UDP datagram.

## Commands

### PING

Checks whether MultiFFBJoy is receiving commands.

    PING

The prototype does not require a response.

### STOP

Immediately stops all active FFB effects.

    STOP

### CENTER

Activates a basic centered spring.

    CENTER

This is equivalent to:

    SPRING 0 0 0.5

### SPRING

Updates the position used by the spring effect.

    SPRING <x> <y> <strength>

Parameters:

- `x`: normalized X position from -1.0 to +1.0
- `y`: normalized Y position from -1.0 to +1.0
- `strength`: force strength from 0.0 to 1.0

Example:

    SPRING 0.5 -0.25 0.75

The logical coordinate system is:

             +Y
              ^
              |
              |
        -X <--+--> +X
              |
              |
              v
             -Y

The FFB backend converts this logical coordinate system to the physical
DirectInput coordinate system.

## Safety Timeout

The sender should continuously send commands while FFB is required.

If no valid command is received for the configured timeout period,
MultiFFBJoy stops all FFB.

The prototype timeout is:

    250 ms

This prevents a client crash or disconnection from leaving a force effect
running indefinitely.

## Example Client Sequence

A client wanting to enable a centered spring could send:

    CENTER

Then periodically send:

    SPRING 0.05 -0.10 0.50
    SPRING 0.10 -0.12 0.50
    SPRING 0.15 -0.08 0.50

When FFB is no longer required:

    STOP

## Future Protocol

The protocol is intentionally small during the prototype stage.

Future commands may include:

- CONSTANT
- DAMPER
- FRICTION
- PERIODIC
- EFFECT_CREATE
- EFFECT_UPDATE
- EFFECT_DESTROY
- DEVICE_SELECT
- DEVICE_INFO
- GAIN
- PROFILE

The protocol should remain independent of any particular game.