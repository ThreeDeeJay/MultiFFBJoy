local M = {}

local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458

local socket = nil
local udp = nil

local initialized = false
local currentVehicleId = nil
local lastSentState = nil

local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end

local function send(command)
  if not udp then
    return false
  end

  local ok, err = udp:sendto(
    command,
    UDP_HOST,
    UDP_PORT
  )

  if not ok then
    log("UDP send failed: " .. tostring(err))
    return false
  end

  log("TX: " .. command)
  return true
end

local function stopFFB()
  if udp then
    send("STOP")
  end
end

local function centerFFB()
  if udp then
    send("CENTER")
  end
end

local function getPlayerVehicleId()
  local id = be:getPlayerVehicleID(0)

  if id == nil or id == 0 then
    return nil
  end

  return id
end

local function synchronizeVehicle(vehicleId, reason)
  if not initialized then
    return
  end

  vehicleId = vehicleId or getPlayerVehicleId()

  if not vehicleId then
    log("No player vehicle; stopping FFB.")
    currentVehicleId = nil
    lastSentState = nil
    stopFFB()
    return
  end

  currentVehicleId = vehicleId

  log(
    string.format(
      "Synchronizing vehicle %s (%s).",
      tostring(vehicleId),
      tostring(reason)
    )
  )

  /*
   * Step 2 intentionally uses the same simple behavior
   * for every vehicle:
   *
   *   vehicle present -> full-strength centering
   *
   * The aircraft/car/gear-specific FFB logic comes later.
   */
  if lastSentState ~= "CENTER" then
    if centerFFB() then
      lastSentState = "CENTER"
    end
  end
end

local function initializeUDP()
  if initialized then
    return true
  end

  socket = require("socket")

  udp = socket.udp()

  if not udp then
    log("Could not create UDP socket.")
    return false
  end

  udp:settimeout(0)

  initialized = true

  log(
    string.format(
      "UDP initialized: %s:%d",
      UDP_HOST,
      UDP_PORT
    )
  )

  return true
end

local function initialize()
  if not initializeUDP() then
    return
  end

  /*
   * The extension can be loaded while sitting in the
   * main menu, so don't assume a vehicle exists yet.
   */
  local vehicleId = getPlayerVehicleId()

  if vehicleId then
    synchronizeVehicle(
      vehicleId,
      "initialization"
    )
  else
    log("Initialized with no active player vehicle.")
  end
end

function M.onExtensionLoaded()
  log("Extension initialized.")
  initialize()
end

function M.onExtensionUnloaded()
  log("Extension shutting down.")

  /*
   * Stop the physical force before destroying our socket.
   */
  if udp then
    stopFFB()
  end

  if udp then
    udp:close()
  end

  udp = nil
  socket = nil

  initialized = false
  currentVehicleId = nil
  lastSentState = nil
end

function M.onVehicleSpawned(vehicleId)
  log(
    "Player vehicle spawned: " ..
    tostring(vehicleId)
  )

  /*
   * onVehicleSpawned can be called for vehicles that
   * aren't the player vehicle, so synchronize only if
   * this is actually the current player vehicle.
   */
  local playerId = getPlayerVehicleId()

  if playerId == vehicleId then
    synchronizeVehicle(
      vehicleId,
      "vehicle spawned"
    )
  end
end

function M.onVehicleSwitched(
  oldId,
  newId,
  player
)
  /*
   * Player 0 is the local player. Some BeamNG versions
   * may omit the player argument, so nil is treated as
   * the local player as well.
   */
  if player ~= nil and player ~= 0 then
    return
  end

  log(
    string.format(
      "Player vehicle switched: %s -> %s",
      tostring(oldId),
      tostring(newId)
    )
  )

  /*
   * Clear the state cache so the new vehicle receives
   * an explicit CENTER command.
   */
  lastSentState = nil
  currentVehicleId = newId

  synchronizeVehicle(
    newId,
    "vehicle switched"
  )
end

function M.onVehicleDestroyed(vehicleId)
  if vehicleId ~= currentVehicleId then
    return
  end

  log(
    "Current player vehicle destroyed: " ..
    tostring(vehicleId)
  )

  currentVehicleId = nil
  lastSentState = nil

  stopFFB()
end

function M.onClientEndMission()
  log("Mission ended.")

  currentVehicleId = nil
  lastSentState = nil

  stopFFB()
end

function M.onClientStartMission()
  log("Mission started.")

  /*
   * The player vehicle may not exist at the exact moment
   * this hook fires, so use the current player ID if one
   * is already available.
   */

  local vehicleId = getPlayerVehicleId()

  if vehicleId then
    synchronizeVehicle(
      vehicleId,
      "mission started"
    )
  end
end

return M