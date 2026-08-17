local M = {}
local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local initialized = false
local currentVehicleId = nil
local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0
local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end
local function sendCommand(command)
  if udp == nil then
    log("Cannot send command; UDP is not initialized.")
    return false
  end
  local ok, err = pcall(function()
    udp:sendto(command, UDP_HOST, UDP_PORT)
  end)
  if not ok then
    log("UDP send failed: " .. tostring(err))
    return false
  end
  log("TX: " .. tostring(command))
  return true
end
local function requestReacquire()
  local now = os.clock()
  if now - lastReacquireTime < REACQUIRE_COOLDOWN then
    log("REACQUIRE suppressed by cooldown.")
    return
  end
  lastReacquireTime = now
  log("Requesting FFB device re-acquisition.")
  sendCommand("REACQUIRE")
end
local function center()
  log("Requesting FFB center.")
  sendCommand("CENTER")
end
local function initializeUDP()
  if udp ~= nil then
    return true
  end
  local ok, result = pcall(function()
    return require("socket")
  end)
  if not ok then
    log("Failed to load socket module: " .. tostring(result))
    return false
  end
  socket = result
  local socketOK, socketResult = pcall(function()
    return socket.udp()
  end)
  if not socketOK or socketResult == nil then
    log("Failed to create UDP socket: " .. tostring(socketResult))
    return false
  end
  udp = socketResult
  pcall(function()
    udp:settimeout(0)
  end)
  log(
    "UDP initialized: "
    .. UDP_HOST
    .. ":"
    .. tostring(UDP_PORT)
    )
  return true
end
local function handleVehicleChange(vehicleId)
  log(
    "Player vehicle changed: "
    .. tostring(currentVehicleId)
    .. " -> "
    .. tostring(vehicleId)
    )
  currentVehicleId = vehicleId
  if vehicleId == nil or vehicleId == 0 then
    log("No active player vehicle.")
    return
  end
  requestReacquire()
  -- Do not immediately center here.
  --
  -- REACQUIRE may have to wait for DirectInput to become usable.
  -- The helper will eventually need to report READY before CENTER
  -- is sent. For now, this only requests the re-acquisition.
end
local function onVehicleSwitched(oldId, newId)
  log(
    "onVehicleSwitched: "
    .. tostring(oldId)
    .. " -> "
    .. tostring(newId)
    )
  handleVehicleChange(newId)
end
local function onVehicleSpawned(vehicleId)
  log(
    "onVehicleSpawned: "
    .. tostring(vehicleId)
    )
  -- Only react if this is the player vehicle.
  local playerId = nil
  if core_vehicle_manager ~= nil then
    if core_vehicle_manager.getPlayerVehicleID then
      local ok, result = pcall(
        core_vehicle_manager.getPlayerVehicleID
        )
      if ok then
        playerId = result
      end
    end
  end
  if playerId == vehicleId then
    handleVehicleChange(vehicleId)
  end
end
local function onPlayerVehicleChanged(vehicleId)
  log(
    "onPlayerVehicleChanged: "
    .. tostring(vehicleId)
    )
  handleVehicleChange(vehicleId)
end
local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end
  if udp == nil then
    initializeUDP()
  end
end
local function onExtensionLoaded()
  if initialized then
    return
  end
  initialized = true
  log("Extension initialized.")
  initializeUDP()
  -- Try to discover the player vehicle once the extension has loaded.
  --
  -- This is deliberately delayed through a job so that the vehicle
  -- manager has a chance to finish initializing.
  core_jobsystem.create(function(job)
    job.sleep(1.0)
    if not initialized then
      return
    end
    local vehicleId = nil
    if core_vehicle_manager ~= nil then
      if core_vehicle_manager.getPlayerVehicleID then
        local ok, result = pcall(
          core_vehicle_manager.getPlayerVehicleID
          )
        if ok then
          vehicleId = result
        end
      end
    end
    log(
      "Initial player vehicle query returned: "
      .. tostring(vehicleId)
      )
    if vehicleId ~= nil and vehicleId ~= 0 then
      handleVehicleChange(vehicleId)
    end
  end)
end
local function onExtensionUnloaded()
  log("Extension unloading.")
  initialized = false
  currentVehicleId = nil
  if udp ~= nil then
    pcall(function()
      udp:close()
    end)
  end
  udp = nil
  socket = nil
end
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleSpawned = onVehicleSpawned
M.onPlayerVehicleChanged = onPlayerVehicleChanged
return M