local M = {}
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local PING_INTERVAL = 0.5
local VEHICLE_SYNC_DELAY = 0.25
local REACQUIRE_DELAY = 0.25
local udp = nil
local helperReady = false
local waitingForHelper = true
local currentVehicleId = nil
local lastVehicleId = nil
local syncPending = false
local syncTimer = 0
local pingTimer = 0
local reacquireTimer = 0
local function log(msg)
  print("[MultiFFBJoy] " .. msg)
end
local function closeUdp()
  if udp then
    pcall(function()
      udp:close()
    end)
    udp = nil
  end
end
local function initializeUdp()
  closeUdp()
  local ok, socket = pcall(require, "socket")
  if not ok or not socket then
    log("ERROR: LuaSocket unavailable.")
    return false
  end
  local sock, err = socket.udp()
  if not sock then
    log("ERROR: unable to create UDP socket: " .. tostring(err))
    return false
  end
  udp = sock
  udp:settimeout(0)
  local okBind, bindErr =
  udp:setsockname(
    UDP_HOST,
    0)
  if not okBind then
    log(
      "WARNING: could not bind local UDP socket: "
      .. tostring(bindErr))
  end
  log(
    string.format(
      "UDP initialized: %s:%d",
      UDP_HOST,
      UDP_PORT))
  return true
end
local function sendCommand(command)
  if not udp then
    return false
  end
  local ok, err =
  udp:sendto(
    command,
    UDP_HOST,
    UDP_PORT)
  if not ok then
    return false
  end
  log("TX: " .. command)
  return true
end
local function receiveCommands()
  if not udp then
    return
  end
  while true do
    local data, ip, port =
    udp:receivefrom()
    if not data then
      break
    end
    log(
      string.format(
        "RX: %s",
        data))
    if data == "PONG" then
      if not helperReady then
        helperReady = true
        waitingForHelper = false
        log("FFB helper connected.")
        syncPending = true
        syncTimer = REACQUIRE_DELAY
      end
    elseif data == "READY" then
      helperReady = true
      waitingForHelper = false
      log("FFB helper ready.")
      syncPending = true
      syncTimer = 0.0
    elseif data == "REACQUIRE_OK" then
      helperReady = true
      log("FFB helper re-acquisition successful.")
      if currentVehicleId ~= nil then
        sendCommand("CENTER")
      end
    end
  end
end
local function getPlayerVehicleId()
  local playerVehicle =
  be:getPlayerVehicle(0)
  if not playerVehicle then
    return nil
  end
  return playerVehicle:getId()
end
local function requestReacquire()
  if not helperReady then
    return
  end
  log(
    "Requesting FFB device re-acquisition.")
  sendCommand("REACQUIRE")
  reacquireTimer =
  REACQUIRE_DELAY
end
local function synchronizeVehicle()
  if not helperReady then
    syncPending = true
    return
  end
  local vehicleId =
  getPlayerVehicleId()
  if vehicleId ~= currentVehicleId then
    lastVehicleId =
    currentVehicleId
    currentVehicleId =
    vehicleId
    log(
      string.format(
        "Player vehicle changed: %s -> %s",
        tostring(lastVehicleId),
        tostring(currentVehicleId)))
  end
  if currentVehicleId == nil then
    sendCommand("STOP")
    return
  end
  log(
    "Synchronizing FFB with current vehicle.")
  requestReacquire()
end
local function checkVehicleChange()
  local vehicleId =
  getPlayerVehicleId()
  if vehicleId ~= currentVehicleId then
    lastVehicleId =
    currentVehicleId
    currentVehicleId =
    vehicleId
    log(
      string.format(
        "Vehicle changed: %s -> %s",
        tostring(lastVehicleId),
        tostring(currentVehicleId)))
    syncPending = true
    syncTimer = VEHICLE_SYNC_DELAY
  end
end
function M.onExtensionLoaded()
  log("Extension initialized.")
  -- The startup script loads this extension before all other extensions
  -- have necessarily settled.  Manual unload mode is therefore established
  -- here, after the extension has actually been registered.
  pcall(function()
    setExtensionUnloadMode(
      "multiffbjoy",
      "manual")
  end)
  initializeUdp()
  helperReady = false
  waitingForHelper = true
  currentVehicleId = nil
  lastVehicleId = nil
  syncPending = true
  syncTimer = 0.5
  pingTimer = 0.0
  reacquireTimer = 0.0
end
function M.onExtensionUnloaded()
  log("Extension unloading.")
  if udp then
    sendCommand("STOP")
  end
  closeUdp()
  helperReady = false
  waitingForHelper = true
end
function M.onUpdate(dtReal, dtSim, dtRaw)
  receiveCommands()
  if not udp then
    return
  end
  pingTimer =
  pingTimer - dtReal
  if pingTimer <= 0 then
    pingTimer =
    PING_INTERVAL
    if not helperReady then
      sendCommand("PING")
    end
  end
  if reacquireTimer > 0 then
    reacquireTimer =
    reacquireTimer - dtReal
    if reacquireTimer < 0 then
      reacquireTimer = 0
    end
  end
  if syncPending then
    syncTimer =
    syncTimer - dtReal
    if syncTimer <= 0 then
      syncPending = false
      synchronizeVehicle()
    end
  end
  checkVehicleChange()
end
function M.onVehicleSwitched(oldId, newId)
  log(
    string.format(
      "Vehicle switched: %s -> %s",
      tostring(oldId),
      tostring(newId)))
  currentVehicleId =
  newId
  syncPending = true
  syncTimer = VEHICLE_SYNC_DELAY
end
function M.onVehicleSpawned(vehicleId)
  log(
    "Vehicle spawned: "
    .. tostring(vehicleId))
  if vehicleId == currentVehicleId then
    syncPending = true
    syncTimer = VEHICLE_SYNC_DELAY
  end
end
function M.onPlayerVehicleChange(vehicleId)
  log(
    "Player vehicle changed event: "
    .. tostring(vehicleId))
  currentVehicleId =
  vehicleId
  syncPending = true
  syncTimer = VEHICLE_SYNC_DELAY
end
return M