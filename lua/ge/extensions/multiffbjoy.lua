-- =========================================================================
-- MultiFFBJoy
--
-- BeamNG GE-side controller for the MultiFFBJoy helper application.
--
-- UDP:
--   127.0.0.1:65458
--
-- Commands:
--   PING
--   REACQUIRE
--   CENTER
--   STOP
--   SPRING <0..1>
-- =========================================================================
local M = {}
local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local currentVehicleId = nil
local initialized = false
local lastReacquireTime = 0
local REACQUIRE_COOLDOWN = 0.50
local vehicleTransitionPending = false
local transitionTimer = 0
local function log(msg)
  print("[MultiFFBJoy] " .. msg)
end
local function sendCommand(command)
  if udp == nil then
    return false
  end
  local ok, err = udp:sendto(
    command .. "\n",
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
local function initializeUdp()
  if udp ~= nil then
    return true
  end
  local ok, socketLib = pcall(require, "socket")
  if not ok or socketLib == nil then
    log("Unable to load LuaSocket.")
    return false
  end
  socket = socketLib
  udp = socket.udp()
  if udp == nil then
    log("Unable to create UDP socket.")
    return false
  end
  udp:settimeout(0)
  log(
    string.format(
      "UDP initialized: %s:%d",
      UDP_HOST,
      UDP_PORT
      )
    )
  return true
end
local function requestReacquire()
  local now = os.clock()
  if now - lastReacquireTime < REACQUIRE_COOLDOWN then
    return
  end
  lastReacquireTime = now
  log("Requesting FFB device re-acquisition.")
  sendCommand("REACQUIRE")
end
local function center()
  sendCommand("CENTER")
end
local function stop()
  sendCommand("STOP")
end
local function getPlayerVehicleId()
  local vehicleId = be:getPlayerVehicleID(0)
  if vehicleId == nil then
    return nil
  end
  if vehicleId == -1 then
    return nil
  end
  return vehicleId
end
local function synchronizeVehicle(vehicleId)
  if vehicleId == nil then
    return
  end
  log(
    string.format(
      "Player vehicle changed: %s -> %s",
      tostring(currentVehicleId),
      tostring(vehicleId)
      )
    )
  currentVehicleId = vehicleId
  /*
  * BeamNG may have just taken the DirectInput FFB interface while
  * creating/loading the vehicle.
  *
  * Give the helper ownership again before asking it to create the
  * persistent centering force.
  */
  requestReacquire()
  vehicleTransitionPending = true
  transitionTimer = 0
end
local function updateVehicleState(dtReal)
  local vehicleId = getPlayerVehicleId()
  if vehicleId ~= currentVehicleId then
    if vehicleId == nil then
      if currentVehicleId ~= nil then
        log("Player vehicle disappeared.")
        currentVehicleId = nil
        vehicleTransitionPending = false
        stop()
      end
      return
    end
    synchronizeVehicle(vehicleId)
    return
  end
  /*
  * After a vehicle transition, wait a short amount of time before
  * sending CENTER. This gives BeamNG time to finish initializing its
  * input/vehicle systems and relinquish the FFB interface.
  */
  if vehicleTransitionPending then
    transitionTimer =
    transitionTimer + dtReal
    if transitionTimer >= 0.25 then
      vehicleTransitionPending = false
      log("Vehicle transition settled; requesting center.")
      requestReacquire()
      center()
    end
  end
end
function M.onExtensionLoaded()
  if initialized then
    return
  end
  initialized = true
  log("Extension initialized.")
  initializeUdp()
  /*
  * Synchronize immediately if a vehicle already exists.
  *
  * This covers the case where the extension is loaded after a vehicle
  * has already been spawned.
  */
  local vehicleId = getPlayerVehicleId()
  if vehicleId ~= nil then
    synchronizeVehicle(vehicleId)
  end
end
function M.onExtensionUnloaded()
  log("Extension unloading.")
  if udp ~= nil then
    pcall(function()
      udp:close()
    end)
    udp = nil
  end
  socket = nil
  initialized = false
  currentVehicleId = nil
  vehicleTransitionPending = false
end
function M.onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end
  updateVehicleState(
    dtReal or
    dtSim or
    dtRaw or
    0
    )
end
function M.onVehicleSwitched(oldId, newId)
  -- This hook is useful when BeamNG explicitly reports a vehicle
  -- switch, but the onUpdate state check remains as a fallback.
  if newId == nil or newId == -1 then
    return
  end
  if newId ~= currentVehicleId then
    log(
      string.format(
        "Vehicle switched: %s -> %s",
        tostring(oldId),
        tostring(newId)
        )
      )
    synchronizeVehicle(newId)
  end
end
function M.onVehicleSpawned(vehicleId)
  if vehicleId == nil or vehicleId == -1 then
    return
  end
  -- Only react immediately if this is the player vehicle.
  local playerVehicleId =
  getPlayerVehicleId()
  if vehicleId == playerVehicleId then
    log(
      string.format(
        "Player vehicle spawned: %s",
        tostring(vehicleId)
        )
      )
    synchronizeVehicle(vehicleId)
  end
end
function M.onVehicleDestroyed(vehicleId)
  if vehicleId == currentVehicleId then
    log(
      string.format(
        "Player vehicle destroyed: %s",
        tostring(vehicleId)
        )
      )
    currentVehicleId = nil
    vehicleTransitionPending = false
    stop()
  end
end
return M