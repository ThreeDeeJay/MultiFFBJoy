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
    log(
      "Failed to create UDP socket: "
      .. tostring(socketResult)
      )
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
--[[
Known-good player vehicle lookup.
]]
local function getPlayerVehicleId()
  if be == nil then
    return nil
  end
  local ok, result = pcall(function()
    return be:getPlayerVehicleID(0)
  end)
  if not ok then
    log(
      "getPlayerVehicleID failed: "
      .. tostring(result)
      )
    return nil
  end
  if result == nil or result == 0 then
    return nil
  end
  return result
end
--[[
Try the cached/current player vehicle API first.
]]
local function getPlayerVehicleObject(vehicleId)
  local vehicle = nil
-- Preferred modern path.
if type(getPlayerVehicle) == "function" then
  local ok, result = pcall(function()
    return getPlayerVehicle(0)
  end)
  if ok and result ~= nil then
    vehicle = result
    log("getPlayerVehicle(0) succeeded.")
  else
    log(
      "getPlayerVehicle(0) unavailable/failed: "
      .. tostring(result)
      )
  end
end
-- Fallback to the known-good object lookup.
if vehicle == nil and be ~= nil then
  local ok, result = pcall(function()
    return be:getObjectByID(vehicleId)
  end)
  if ok and result ~= nil then
    vehicle = result
    log("be:getObjectByID() succeeded.")
  else
    log(
      "be:getObjectByID() failed: "
      .. tostring(result)
      )
  end
end
return vehicle
end
local function printField(vehicle, fieldName)
  if vehicle == nil then
    return
  end
  if type(vehicle.getField) ~= "function" then
    log(
      "getField() not available for "
      .. tostring(fieldName)
      )
    return
  end
  local ok, value = pcall(function()
    return vehicle:getField(fieldName, "")
  end)
  if not ok then
    log(
      "getField("
      .. tostring(fieldName)
      .. ") ERROR: "
      .. tostring(value)
      )
    return
  end
  log(
    "getField("
    .. tostring(fieldName)
    .. ") = "
    .. tostring(value)
    )
end
local function printDirectField(vehicle, fieldName)
  if vehicle == nil then
    return
  end
  local ok, value = pcall(function()
    return vehicle[fieldName]
  end)
  if not ok then
    log(
      "vehicle."
      .. tostring(fieldName)
      .. " ERROR: "
      .. tostring(value)
      )
    return
  end
  log(
    "vehicle."
    .. tostring(fieldName)
    .. " = "
    .. tostring(value)
    )
end
local function tryJBeamFilename(vehicle)
  if vehicle == nil then
    return
  end
  if type(vehicle.getJBeamFilename) ~= "function" then
    log("getJBeamFilename() not available.")
    return
  end
  local ok, value = pcall(function()
    return vehicle:getJBeamFilename()
  end)
  if not ok then
    log(
      "getJBeamFilename() ERROR: "
      .. tostring(value)
      )
    return
  end
  log(
    "getJBeamFilename() = "
    .. tostring(value)
    )
end
local function splitPartConfig(partConfig)
  if partConfig == nil then
    return nil, nil
  end
  partConfig = tostring(partConfig)
  if partConfig == "" then
    return nil, nil
  end
-- Normalize slashes.
partConfig = partConfig:gsub("\\", "/")
-- Remove trailing slash if any.
partConfig = partConfig:gsub("/+$", "")
-- Expected:
--
-- vehicles/miramar/luxe_A.pc
--
local vehicleName, configName =
partConfig:match(
  "^vehicles/([^/]+)/([^/]+)%.pc$"
  )
if vehicleName ~= nil then
  return vehicleName, configName
end
-- More permissive fallback in case the path has
-- additional prefixes.
vehicleName, configName =
partConfig:match(
  ".*/vehicles/([^/]+)/([^/]+)%.pc$"
  )
return vehicleName, configName
end
local function dumpVehicleMetadata(vehicleId)
  log("========================================")
  log("VEHICLE METADATA DIAGNOSTIC")
  log("========================================")
  log(
    "Vehicle ID = "
    .. tostring(vehicleId)
    )
  local vehicle =
  getPlayerVehicleObject(vehicleId)
  if vehicle == nil then
    log("ERROR: Could not obtain player vehicle object.")
    log("========================================")
    return
  end
  log(
    "Vehicle object = "
    .. tostring(vehicle)
    )
  log("")
  log("DIRECT VEHICLE FIELDS")
  log("---------------------")
  local directFields = {
    "JBeam",
    "jBeam",
    "partConfig",
    "config",
    "configName",
    "configuration",
    "configurationName",
    "model",
    "modelName",
    "vehicleName",
    "vehicleType",
    "type",
    "category",
    "name"
  }
  for _, fieldName in ipairs(directFields) do
    printDirectField(
      vehicle,
      fieldName
      )
  end
  log("")
  log("getField() VALUES")
  log("-----------------")
  local getFieldNames = {
    "JBeam",
    "jBeam",
    "partConfig",
    "config",
    "configName",
    "configuration",
    "configurationName",
    "model",
    "modelName",
    "vehicleName",
    "vehicleType",
    "type",
    "category",
    "name"
  }
  for _, fieldName in ipairs(getFieldNames) do
    printField(
      vehicle,
      fieldName
      )
  end
  log("")
  log("METHODS")
  log("-------")
  tryJBeamFilename(vehicle)
  log("")
  log("PART CONFIG PARSING")
  log("-------------------")
  local ok, partConfig =
  pcall(function()
    return vehicle:getField(
      "partConfig",
      ""
      )
  end)
  if ok then
    log(
      "partConfig raw = "
      .. tostring(partConfig)
      )
    local vehicleName, configName =
    splitPartConfig(partConfig)
    log(
      "Parsed vehicle codename = "
      .. tostring(vehicleName)
      )
    log(
      "Parsed configuration codename = "
      .. tostring(configName)
      )
  else
    log(
      "Could not read partConfig: "
      .. tostring(partConfig)
      )
  end
  log("")
  log("OBJECT ID")
  log("---------")
  if type(vehicle.getID) == "function" then
    local idOK, objectId =
    pcall(function()
      return vehicle:getID()
    end)
    if idOK then
      log(
        "vehicle:getID() = "
        .. tostring(objectId)
        )
    else
      log(
        "vehicle:getID() failed: "
        .. tostring(objectId)
        )
    end
  else
    log("vehicle:getID() not available.")
  end
  log("")
  log("AVAILABLE OBJECT KEYS")
  log("---------------------")
-- userdata cannot normally be iterated with pairs(),
-- but attempt it safely in case the object exposes keys.
local pairsOK, pairsError =
pcall(function()
  local count = 0
  for key, value in pairs(vehicle) do
    log(
      "  "
      .. tostring(key)
      .. " = "
      .. tostring(value)
      )
    count = count + 1
-- Avoid flooding the console if the object exposes
-- a huge number of fields.
if count >= 100 then
  log("  ... truncated after 100 keys")
  break
end
end
if count == 0 then
  log("  <no enumerable keys>")
end
end)
if not pairsOK then
  log(
    "pairs(vehicle) unavailable: "
    .. tostring(pairsError)
    )
end
log("========================================")
log("END VEHICLE METADATA DIAGNOSTIC")
log("========================================")
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
-- Give BeamNG a short opportunity to finish populating
-- the vehicle object's configuration information.
core_jobsystem.create(function(job)
  job.sleep(0.5)
  if not initialized then
    return
  end
  if currentVehicleId ~= vehicleId then
    return
  end
  dumpVehicleMetadata(vehicleId)
end)
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
  local playerId =
  getPlayerVehicleId()
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
  core_jobsystem.create(function(job)
    local elapsed = 0
    while initialized and elapsed < 30 do
      local vehicleId =
      getPlayerVehicleId()
      if vehicleId ~= nil then
        log(
          "Initial player vehicle detected: "
          .. tostring(vehicleId)
          )
        handleVehicleChange(vehicleId)
        return
      end
      job.sleep(0.5)
      elapsed = elapsed + 0.5
    end
    if initialized then
      log(
        "No player vehicle detected during startup search."
        )
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