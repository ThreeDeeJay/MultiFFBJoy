local M = {}
local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local initialized = false
local currentVehicleId = nil
local lastMetadataKey = nil
local lastMetadataRequestTime = -1000
local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0
local METADATA_COOLDOWN = 0.5
local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end
local function safeToString(value)
  if value == nil then
    return nil
  end
  local ok, result = pcall(function()
    return tostring(value)
  end)
  if ok then
    return result
  end
  return nil
end
local function nonEmptyString(value)
  local text = safeToString(value)
  if text == nil or text == "" then
    return nil
  end
  return text
end
local function firstNonEmpty(...)
  local values = {...}
  for i = 1, #values do
    local value = nonEmptyString(values[i])
    if value ~= nil then
      return value
    end
  end
  return nil
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
  if result == nil or result == 0 or result == -1 then
    return nil
  end
  return result
end
local function getPlayerVehicle()
  local vehicleId = getPlayerVehicleId()
  if vehicleId == nil then
    return nil, nil
  end
  if be == nil then
    return nil, vehicleId
  end
  local ok, vehicle = pcall(function()
    return be:getObjectByID(vehicleId)
  end)
  if not ok or vehicle == nil then
    return nil, vehicleId
  end
  return vehicle, vehicleId
end
local function getFieldSafe(vehicle, fieldName)
  if vehicle == nil then
    return nil
  end
  local ok, result = pcall(function()
    return vehicle:getField(fieldName, "")
  end)
  if not ok then
    return nil
  end
  return nonEmptyString(result)
end
local function getVehicleJBeam(vehicle)
  if vehicle == nil then
    return nil
  end
  local ok, result = pcall(function()
    return vehicle:getJBeamFilename()
  end)
  if ok then
    result = nonEmptyString(result)
    if result ~= nil then
      return result
    end
  end
  return firstNonEmpty(
    vehicle.JBeam,
    vehicle.jBeam,
    getFieldSafe(vehicle, "JBeam"),
    getFieldSafe(vehicle, "jBeam")
    )
end
local function getPartConfig(vehicle)
  return firstNonEmpty(
    vehicle and vehicle.partConfig,
    getFieldSafe(vehicle, "partConfig"),
    vehicle and vehicle.partconfig
    )
end
local function parsePartConfig(partConfig)
  if partConfig == nil then
    return nil, nil
  end
  local path = partConfig:gsub("\\", "/")
  local vehicleCode, configCode =
  path:match("vehicles/([^/]+)/([^/]+)%.pc$")
  if vehicleCode == nil then
    vehicleCode, configCode =
    path:match("([^/]+)/([^/]+)%.pc$")
  end
  return vehicleCode, configCode
end
local function getNestedValue(root, ...)
  local current = root
  local keys = {...}
  if current == nil then
    return nil
  end
  for i = 1, #keys do
    local key = keys[i]
    local ok, value = pcall(function()
      return current[key]
    end)
    if not ok or value == nil then
      return nil
    end
    current = value
  end
  return current
end
local function getVehicleType(vehicle)
  if vehicle == nil then
    return nil
  end
  local candidates = {
    vehicle.vehicleType,
    vehicle.type,
    vehicle.category,
    vehicle.class,
    getFieldSafe(vehicle, "vehicleType"),
    getFieldSafe(vehicle, "type"),
    getFieldSafe(vehicle, "category"),
    getFieldSafe(vehicle, "class"),
    getNestedValue(vehicle, "data", "Type"),
    getNestedValue(vehicle, "data", "type"),
    getNestedValue(vehicle, "data", "vehicleType"),
    getNestedValue(vehicle, "data", "category"),
    getNestedValue(vehicle, "data", "information", "Type"),
    getNestedValue(vehicle, "data", "information", "type")
  }
  local value = firstNonEmpty(unpack(candidates))
  if value == nil then
    return nil
  end
  return value
end
local function getTransmissionCandidates(vehicle)
  local candidates = {}
  local function add(value)
    value = nonEmptyString(value)
    if value ~= nil then
      candidates[#candidates + 1] = value
    end
  end
  if vehicle == nil then
    return candidates
  end
  add(vehicle.transmission)
  add(vehicle.Transmission)
  add(getFieldSafe(vehicle, "transmission"))
  add(getFieldSafe(vehicle, "Transmission"))
  add(getNestedValue(vehicle, "data", "Transmission"))
  add(getNestedValue(vehicle, "data", "transmission"))
  add(getNestedValue(vehicle, "data", "vehicleController", "transmission"))
  add(getNestedValue(vehicle, "data", "vehicleController", "transmissionType"))
  add(getNestedValue(vehicle, "data", "vehicleController", "gearboxType"))
  add(getNestedValue(vehicle, "data", "vehicleController", "gearbox"))
  add(getNestedValue(vehicle, "data", "gearbox", "type"))
  add(getNestedValue(vehicle, "data", "gearbox", "transmission"))
  return candidates
end
local function getAutomaticModes(vehicle)
  if vehicle == nil then
    return nil
  end
  local candidates = {
    getNestedValue(
      vehicle,
      "data",
      "vehicleController",
      "automaticModes"
      ),
    getNestedValue(
      vehicle,
      "data",
      "vehicleController",
      "automaticModesString"
      ),
    getNestedValue(
      vehicle,
      "data",
      "shiftLogic",
      "automaticModes"
      ),
    getNestedValue(
      vehicle,
      "data",
      "shiftLogicAutomatic",
      "automaticModes"
      ),
    vehicle.automaticModes,
    getFieldSafe(vehicle, "automaticModes")
  }
  return firstNonEmpty(unpack(candidates))
end
local function inferTransmission(vehicle)
  local directCandidates = getTransmissionCandidates(vehicle)
  for i = 1, #directCandidates do
    local value = directCandidates[i]
    local lower = string.lower(value)
    if string.find(lower, "automatic", 1, true) then
      return "Automatic", value
    end
    if string.find(lower, "manual", 1, true) then
      return "Manual", value
    end
    if string.find(lower, "cvt", 1, true) then
      return "CVT", value
    end
    if string.find(lower, "dct", 1, true) then
      return "DCT", value
    end
  end
  local automaticModes = getAutomaticModes(vehicle)
  if automaticModes ~= nil then
    return "Automatic", automaticModes
  end
  return nil, nil
end
local function normalizeVehicleType(value)
  if value == nil then
    return nil
  end
  local lower = string.lower(value)
  if lower == "car"
    or lower == "cars"
    or string.find(lower, "car", 1, true) then
      return "Car"
    end
    if lower == "aircraft"
      or lower == "plane"
      or lower == "airplane"
      or string.find(lower, "aircraft", 1, true) then
        return "Aircraft"
      end
      if lower == "truck"
        or string.find(lower, "truck", 1, true) then
          return "Truck"
        end
        if lower == "motorcycle"
          or lower == "bike"
          or string.find(lower, "motorcycle", 1, true) then
            return "Motorcycle"
          end
          if lower == "boat"
            or lower == "watercraft"
            or string.find(lower, "boat", 1, true) then
              return "Boat"
            end
            return value
          end
          local function getVehicleMetadata(vehicle)
            local partConfig = getPartConfig(vehicle)
            local parsedVehicleCode
            local parsedConfigCode
            parsedVehicleCode, parsedConfigCode =
            parsePartConfig(partConfig)
            local jbeam = getVehicleJBeam(vehicle)
            local vehicleCode =
            firstNonEmpty(
              parsedVehicleCode,
              jbeam
              )
            local configurationCode =
            firstNonEmpty(parsedConfigCode)
            local vehicleType =
            normalizeVehicleType(getVehicleType(vehicle))
            local transmission, transmissionRaw =
            inferTransmission(vehicle)
            local automaticModes =
            getAutomaticModes(vehicle)
            return {
              game = "BeamNG.drive",
              vehicleType = vehicleType or "",
              vehicle = vehicleCode or "",
              configuration = configurationCode or "",
              transmission = transmission or "",
              transmissionRaw = transmissionRaw or "",
              automaticModes = automaticModes or "",
              partConfig = partConfig or "",
              jbeam = jbeam or ""
            }
          end
          local function printVehicleMetadata(metadata, vehicleId)
            log("========================================")
            log("VEHICLE METADATA")
            log("========================================")
            log("Vehicle ID = " .. tostring(vehicleId))
            log("Game = " .. tostring(metadata.game))
            log("Vehicle codename = " .. tostring(metadata.vehicle))
            log("Configuration codename = " .. tostring(metadata.configuration))
            log("Part config = " .. tostring(metadata.partConfig))
            log("JBeam = " .. tostring(metadata.jbeam))
            log("Vehicle type = " .. tostring(metadata.vehicleType))
            log("Transmission = " .. tostring(metadata.transmission))
            log("Transmission raw = " .. tostring(metadata.transmissionRaw))
            log("Automatic modes = " .. tostring(metadata.automaticModes))
            log("========================================")
            log("END VEHICLE METADATA")
            log("========================================")
          end
          local function makeMetadataKey(metadata)
            return table.concat({
              metadata.game,
              metadata.vehicleType,
              metadata.vehicle,
              metadata.configuration,
              metadata.transmission,
              metadata.automaticModes
            }, "|")
          end
          local function sendVehicleMetadata(metadata)
            local now = os.clock()
            if now - lastMetadataRequestTime < METADATA_COOLDOWN then
              return
            end
            local key = makeMetadataKey(metadata)
            if key == lastMetadataKey then
              return
            end
            lastMetadataRequestTime = now
            lastMetadataKey = key
-- The helper owns Configuration.txt and resolves the profile.
--
-- Pipe-separated fields deliberately avoid whitespace parsing problems.
--
-- Empty fields are represented by "-".
local function field(value)
  if value == nil or value == "" then
    return "-"
  end
  return tostring(value)
end
local command =
"VEHICLE|"
.. field(metadata.game)
.. "|"
.. field(metadata.vehicleType)
.. "|"
.. field(metadata.vehicle)
.. "|"
.. field(metadata.configuration)
.. "|"
.. field(metadata.transmission)
.. "|"
.. field(metadata.automaticModes)
sendCommand(command)
end
local function processVehicle(vehicleId, reason)
  if vehicleId == nil or vehicleId == 0 or vehicleId == -1 then
    return
  end
  local vehicle, actualVehicleId = getPlayerVehicle()
  if vehicle == nil or actualVehicleId ~= vehicleId then
    return
  end
  local metadata = getVehicleMetadata(vehicle)
  printVehicleMetadata(metadata, vehicleId)
  sendVehicleMetadata(metadata)
  log(
    "Vehicle profile request sent for "
    .. tostring(metadata.vehicle)
    .. "/"
    .. tostring(metadata.configuration)
    .. " ("
    .. tostring(reason)
    .. ")."
    )
end
local function handleVehicleChange(vehicleId, reason)
  if vehicleId == nil or vehicleId == 0 or vehicleId == -1 then
-- BeamNG commonly transitions through -1 while replacing a vehicle.
-- Do not destroy the last valid metadata during that transition.
return
end
if currentVehicleId == vehicleId then
  processVehicle(vehicleId, reason or "update")
  return
end
log(
  "Player vehicle changed: "
  .. tostring(currentVehicleId)
  .. " -> "
  .. tostring(vehicleId)
  .. " ("
  .. tostring(reason)
  .. ")"
  )
currentVehicleId = vehicleId
-- The FFB device may need to be reacquired when BeamNG replaces
-- the active vehicle.
requestReacquire()
processVehicle(vehicleId, reason or "vehicle change")
end
local function onVehicleSwitched(oldId, newId)
  log(
    "onVehicleSwitched: "
    .. tostring(oldId)
    .. " -> "
    .. tostring(newId)
    )
  if newId ~= nil and newId ~= -1 then
    handleVehicleChange(newId, "switch")
  end
end
local function onVehicleSpawned(vehicleId)
  log(
    "onVehicleSpawned: "
    .. tostring(vehicleId)
    )
  local playerId = getPlayerVehicleId()
  if playerId == vehicleId then
    handleVehicleChange(vehicleId, "spawn")
  end
end
local function onPlayerVehicleChanged(vehicleId)
  log(
    "onPlayerVehicleChanged: "
    .. tostring(vehicleId)
    )
  if vehicleId ~= nil and vehicleId ~= -1 then
    handleVehicleChange(vehicleId, "player vehicle")
  end
end
local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end
  if udp == nil then
    initializeUDP()
  end
-- BeamNG can report vehicle switching events before the final player
-- vehicle is fully installed. Polling here is deliberately lightweight.
local vehicleId = getPlayerVehicleId()
if vehicleId ~= nil and vehicleId ~= currentVehicleId then
  handleVehicleChange(vehicleId, "poll")
end
end
local function onExtensionLoaded()
  if initialized then
    return
  end
  initialized = true
  log("Extension initialized.")
  if not initializeUDP() then
    return
  end
  core_jobsystem.create(function(job)
    local elapsed = 0
    while initialized and elapsed < 30 do
      local vehicleId = getPlayerVehicleId()
      if vehicleId ~= nil then
        log(
          "Initial player vehicle detected: "
          .. tostring(vehicleId)
          )
        handleVehicleChange(vehicleId, "startup")
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
  lastMetadataKey = nil
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