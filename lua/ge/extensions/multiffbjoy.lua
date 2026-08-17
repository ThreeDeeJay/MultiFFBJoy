-- ============================================================================
-- MultiFFBJoy - BeamNG GE extension
--
-- Communicates with the MultiFFBJoy helper application over UDP.
--
-- The helper owns the DirectInput FFB device. BeamNG only tells the helper
-- what FFB state should currently be active.
--
-- Current prototype behavior:
--   * Automatically initializes when the extension loads.
--   * Synchronizes the current player vehicle immediately.
--   * Re-acquires the DirectInput FFB device whenever the player vehicle
--     changes/spawns.
--   * Applies full-strength centering after each re-acquisition.
--   * Stops FFB when there is no active player vehicle.
--
-- UDP protocol:
--   PING
--   CENTER
--   STOP
--   REACQUIRE
--   SPRING <0.0..1.0>
-- ============================================================================
local M = {}
local socket = nil
local udpSocket = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local currentVehicleId = nil
local currentState = "STOP"
local initialized = false
-- ---------------------------------------------------------------------------
-- Logging
-- ---------------------------------------------------------------------------
local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end
-- ---------------------------------------------------------------------------
-- UDP
-- ---------------------------------------------------------------------------
local function initializeUdp()
  if udpSocket then
    return true
  end
  local ok, socketModule = pcall(require, "socket")
  if not ok or not socketModule then
    log("ERROR: LuaSocket is unavailable.")
    return false
  end
  socket = socketModule
  local sock, err = socket.udp()
  if not sock then
    log("ERROR: could not create UDP socket: " .. tostring(err))
    return false
  end
  udpSocket = sock
  -- We only send commands to the helper. The helper owns the listening
  -- socket, so we do not need to bind this socket to a local port.
  udpSocket:settimeout(0)
  log(
    string.format(
      "UDP initialized: %s:%d",
      UDP_HOST,
      UDP_PORT
      )
    )
  return true
end
local function sendCommand(command)
  if not udpSocket then
    log(
      "UDP unavailable; cannot send command: " ..
      tostring(command)
      )
    return false
  end
  local bytes, err = udpSocket:sendto(
    command,
    UDP_HOST,
    UDP_PORT
    )
  if not bytes then
    log(
      "UDP send failed for '" ..
      tostring(command) ..
      "': " ..
      tostring(err)
      )
    return false
  end
  log("TX: " .. tostring(command))
  return true
end
-- ---------------------------------------------------------------------------
-- FFB commands
-- ---------------------------------------------------------------------------
local function stopFFB()
  if sendCommand("STOP") then
    currentState = "STOP"
    return true
  end
  return false
end
local function centerFFB()
  if sendCommand("CENTER") then
    currentState = "CENTER"
    return true
  end
  return false
end
local function setSpringStrength(strength)
  strength = tonumber(strength) or 0
  if strength < 0 then
    strength = 0
  elseif strength > 1 then
    strength = 1
  end
  if sendCommand(
    string.format("SPRING %.3f", strength)
    ) then
    currentState = "SPRING"
    return true
  end
  return false
end
-- ---------------------------------------------------------------------------
-- FFB device re-acquisition
--
-- This is intentionally NOT suppressed when currentState is already CENTER.
--
-- BeamNG can interact with / take ownership of the DirectInput FFB device
-- during vehicle creation and switching. Therefore every actual vehicle
-- transition must give the helper an opportunity to release and re-open the
-- FFB device and recreate its effects.
-- ---------------------------------------------------------------------------
local function reacquireFFB()
  log("Requesting FFB device re-acquisition.")
  if not sendCommand("REACQUIRE") then
    return false
  end
  -- The helper is now responsible for releasing/re-opening the device and
  -- recreating its effects. Do not pretend that CENTER is already active
  -- until we explicitly send the CENTER command below.
  currentState = "NONE"
  return true
end
-- ---------------------------------------------------------------------------
-- Vehicle identification
-- ---------------------------------------------------------------------------
local function getPlayerVehicleId()
  local vehicleId = be:getPlayerVehicleID(0)
  if vehicleId == nil then
    return nil
  end
  -- BeamNG can represent "no vehicle" as either nil or -1 depending on the
  -- lifecycle stage.
  if vehicleId == -1 then
    return nil
  end
  return vehicleId
end
-- ---------------------------------------------------------------------------
-- Synchronize FFB with the current player vehicle
-- ---------------------------------------------------------------------------
local function synchronizeVehicle()
  local vehicleId = getPlayerVehicleId()
  if vehicleId == currentVehicleId then
    return
  end
  local previousVehicleId = currentVehicleId
  currentVehicleId = vehicleId
  if vehicleId then
    log(
      "Player vehicle changed: " ..
      tostring(previousVehicleId) ..
      " -> " ..
      tostring(vehicleId)
      )
    -- The vehicle transition is exactly when BeamNG may have touched the
    -- DirectInput FFB device. Always reacquire it rather than merely sending
    -- another CENTER command.
    if reacquireFFB() then
      centerFFB()
    end
  else
    log("No active player vehicle.")
    stopFFB()
  end
end
-- ---------------------------------------------------------------------------
-- BeamNG vehicle lifecycle callbacks
-- ---------------------------------------------------------------------------
function M.onVehicleSpawned(vehicleId)
  log(
    "Vehicle spawned: " ..
    tostring(vehicleId)
    )
  -- Do not immediately send CENTER here. BeamNG's vehicle initialization may
  -- still be in progress.
  --
  -- Invalidating the cached ID causes onUpdate() to perform the synchronized
  -- REACQUIRE + CENTER once the vehicle is visible as the player vehicle.
  currentVehicleId = nil
end
function M.onVehicleSwitched(oldId, newId)
  log(
    "Vehicle switched: " ..
    tostring(oldId) ..
    " -> " ..
    tostring(newId)
    )
  -- Force the next update to synchronize the new player vehicle.
  currentVehicleId = nil
end
function M.onVehicleDestroyed(vehicleId)
  log(
    "Vehicle destroyed: " ..
    tostring(vehicleId)
    )
  if vehicleId == currentVehicleId then
    currentVehicleId = nil
    stopFFB()
  end
end
-- ---------------------------------------------------------------------------
-- Extension lifecycle
-- ---------------------------------------------------------------------------
function M.onExtensionLoaded()
  -- This callback is not relied upon for initialization because extension
  -- loading behavior can vary. init() below performs the actual setup.
end
function M.onExtensionUnloaded()
  log("Extension shutting down.")
  -- Explicitly stop the helper-side force when BeamNG unloads the extension.
  stopFFB()
  if udpSocket then
    udpSocket:close()
    udpSocket = nil
  end
  socket = nil
  initialized = false
  currentVehicleId = nil
  currentState = "STOP"
  log("Extension unloaded.")
end
-- ---------------------------------------------------------------------------
-- Initialization
-- ---------------------------------------------------------------------------
local function initialize()
  if initialized then
    return true
  end
  log("Extension initialized.")
  if not initializeUdp() then
    log("UDP initialization failed.")
    return false
  end
  initialized = true
  -- Do not wait for the next vehicle spawn.
  --
  -- If BeamNG is already sitting in a vehicle when the extension is loaded,
  -- synchronize it immediately.
  currentVehicleId = nil
  synchronizeVehicle()
  return true
end
-- ---------------------------------------------------------------------------
-- Update
-- ---------------------------------------------------------------------------
function M.onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    initialize()
    return
  end
  synchronizeVehicle()
end
-- ---------------------------------------------------------------------------
-- Public initialization entry point
-- ---------------------------------------------------------------------------
function M.init()
  initialize()
end
-- ---------------------------------------------------------------------------
-- Initialize immediately when the extension is loaded.
-- ---------------------------------------------------------------------------
initialize()
return M