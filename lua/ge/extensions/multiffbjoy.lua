local M = {}

local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458

local initialized = false
local currentVehicleId = nil
local currentVehicleCode = nil
local currentConfigurationCode = nil
local currentPartConfig = nil
local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0
local metadataTimer = 0
local metadataPendingVehicleId = nil
local METADATA_RETRY_INTERVAL = 0.25
local METADATA_RETRY_COUNT = 12
local metadataRetriesLeft = 0

local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end

local function safeToString(value)
  if value == nil then
    return ""
  end
  return tostring(value)
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
    log("Failed to create UDP socket: " .. tostring(socketResult))
    return false
  end

  udp = socketResult
  pcall(function()
    udp:settimeout(0)
  end)

  log("UDP initialized: " .. UDP_HOST .. ":" .. tostring(UDP_PORT))
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
    log("getPlayerVehicleID failed: " .. tostring(result))
    return nil
  end

  if result == nil or result < 0 then
    return nil
  end

  return result
end

local function getPlayerVehicle()
  if be == nil then
    return nil
  end

  local ok, result = pcall(function()
    return be:getPlayerVehicle(0)
  end)
  if not ok then
    return nil
  end
  return result
end

local function normalizeVehicleType(value)
  if value == nil then
    return ""
  end

  local text = tostring(value)
  if text == "" then
    return ""
  end

  local aliases = {
    ["car"] = "Car",
    ["cars"] = "Car",
    ["truck"] = "Truck",
    ["trucks"] = "Truck",
    ["aircraft"] = "Aircraft",
    ["plane"] = "Aircraft",
    ["planes"] = "Aircraft",
    ["helicopter"] = "Aircraft",
    ["boat"] = "Boat",
    ["boats"] = "Boat",
    ["motorcycle"] = "Motorcycle",
    ["motorcycles"] = "Motorcycle",
    ["trailer"] = "Trailer",
    ["trailers"] = "Trailer",
    ["prop"] = "Prop",
    ["utility"] = "Utility",
  }

  return aliases[string.lower(text)] or text
end

local function normalizeTransmission(value, rawType)
  local text = value
  if text == nil or text == "" then
    text = rawType
  end
  if text == nil then
    return ""
  end

  text = tostring(text)
  local lower = string.lower(text)

  if lower == "automaticgearbox" or lower == "automatic" then
    return "Automatic"
  elseif lower == "manualgearbox" or lower == "manual" then
    return "Manual"
  elseif lower == "sequentialgearbox" or lower == "sequential" then
    return "Sequential"
  elseif lower == "dctgearbox" or lower == "dct" then
    return "DCT"
  elseif lower == "cvtgearbox" or lower == "cvt" then
    return "CVT"
  elseif lower == "electricmotor" or lower == "electric" then
    return "Electric"
  elseif lower == "dummy" or lower == "shiftlogic-dummy" then
    return ""
  end

  return text
end

local function normalizeGearLayout(value)
  if value == nil then
    return ""
  end
  return tostring(value)
end

local function sendVehicleRequest(metadata, reason)
  if not metadata then
    return false
  end

  local vehicleCode = safeToString(metadata.vehicle)
  local configurationCode = safeToString(metadata.configuration)
  if vehicleCode == "" then
    log("Cannot send vehicle profile request without a vehicle codename.")
    return false
  end

  local game = safeToString(metadata.game)
  if game == "" then
    game = "BeamNG.drive"
  end

  local vehicleType = normalizeVehicleType(metadata.vehicleType)
  local transmission = normalizeTransmission(metadata.transmission, metadata.transmissionRaw)
  local gearLayout = normalizeGearLayout(metadata.gearLayout)

  local command = table.concat({
    "VEHICLE",
    game,
    vehicleType,
    vehicleCode,
    configurationCode,
    transmission,
    gearLayout,
  }, "|")

  log("Vehicle profile request (" .. tostring(reason or "unknown") .. "):")
  log("  Game = " .. game)
  log("  VehicleType = " .. (vehicleType ~= "" and vehicleType or "<unknown>"))
  log("  Vehicle = " .. vehicleCode)
  log("  Configuration = " .. (configurationCode ~= "" and configurationCode or "<unknown>"))
  log("  Transmission = " .. (transmission ~= "" and transmission or "<unknown>"))
  log("  GearLayout = " .. (gearLayout ~= "" and gearLayout or "<unknown>"))

  return sendCommand(command)
end

local function queueVehicleMetadataDiagnostic(vehicleId, reason, resetRetries)
  local vehicle = getPlayerVehicle()
  if vehicle == nil then
    log("Cannot queue VLUA metadata diagnostic; no player vehicle object.")
    return false
  end

  metadataPendingVehicleId = vehicleId

  -- Capture identity from the GE vehicle object before asking VLUA for
  -- powertrain metadata. These fields are known to work for BeamNG vehicles.
  local partConfig = safeToString((function()
    local ok, value = pcall(function()
      return vehicle.partConfig
    end)
    return ok and value or nil
  end)())
  local jBeam = safeToString((function()
    local ok, value = pcall(function()
      return vehicle.JBeam or vehicle.jBeam
    end)
    return ok and value or nil
  end)())

  if partConfig ~= "" then
    currentPartConfig = partConfig
  end
  if jBeam ~= "" then
    currentVehicleCode = jBeam
  end
  if currentConfigurationCode == nil or currentConfigurationCode == "" then
    local normalized = partConfig:gsub("\\", "/")
    local _, parsedConfiguration = normalized:match("vehicles/([^/]+)/([^/]+)%.pc$")
    if parsedConfiguration ~= nil and parsedConfiguration ~= "" then
      currentConfigurationCode = parsedConfiguration
    end
  end

  if resetRetries ~= false then
    metadataRetriesLeft = METADATA_RETRY_COUNT
    metadataTimer = METADATA_RETRY_INTERVAL
  end

  local command = [[
    extensions.load("multiffbjoy")
    if extensions.multiffbjoy and extensions.multiffbjoy.sendMetadata then
      extensions.multiffbjoy.sendMetadata()
    end
  ]]

  local ok, result = pcall(function()
    vehicle:queueLuaCommand(command)
  end)
  if not ok then
    log("Failed to queue VLUA metadata diagnostic: " .. tostring(result))
    return false
  end

  log("VLUA metadata request queued for vehicle " .. tostring(vehicleId) .. " (" .. tostring(reason or "unknown") .. ").")
  return true
end

local function handleVehicleChange(vehicleId, reason)
  if vehicleId == currentVehicleId and reason ~= "startup" then
    queueVehicleMetadataDiagnostic(vehicleId, reason)
    return
  end

  log("Player vehicle changed: " .. tostring(currentVehicleId) .. " -> " .. tostring(vehicleId) .. " (" .. tostring(reason or "unknown") .. ")")
  currentVehicleId = vehicleId
  currentVehicleCode = nil
  currentConfigurationCode = nil
  currentPartConfig = nil

  if vehicleId == nil or vehicleId < 0 then
    metadataPendingVehicleId = nil
    metadataRetriesLeft = 0
    return
  end

  requestReacquire()
  queueVehicleMetadataDiagnostic(vehicleId, reason)
end

local function onVehicleSwitched(oldId, newId)
  log("onVehicleSwitched: " .. tostring(oldId) .. " -> " .. tostring(newId))
  handleVehicleChange(newId, "switch")
end

local function onVehicleSpawned(vehicleId)
  log("onVehicleSpawned: " .. tostring(vehicleId))
  local playerId = getPlayerVehicleId()
  if playerId == vehicleId then
    handleVehicleChange(vehicleId, "spawn")
  end
end

local function onPlayerVehicleChanged(vehicleId)
  log("onPlayerVehicleChanged: " .. tostring(vehicleId))
  handleVehicleChange(vehicleId, "player-change")
end

local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end

  if udp == nil then
    initializeUDP()
  end

  if metadataPendingVehicleId == nil or metadataRetriesLeft <= 0 then
    return
  end

  metadataTimer = metadataTimer - dtReal
  if metadataTimer > 0 then
    return
  end
  metadataTimer = METADATA_RETRY_INTERVAL
  metadataRetriesLeft = metadataRetriesLeft - 1

  local playerId = getPlayerVehicleId()
  if playerId ~= metadataPendingVehicleId then
    return
  end

  -- Re-queue the request because the first VLUA can be created before its
  -- extensions have finished loading.
  queueVehicleMetadataDiagnostic(metadataPendingVehicleId, "retry", false)
end

function M.receiveVehicleMetadata(metadata)
  if type(metadata) ~= "table" then
    log("Received invalid vehicle metadata payload.")
    return
  end

  local vehicleId = tonumber(metadata.vehicleId)
  if vehicleId == nil or vehicleId ~= currentVehicleId then
    log("Ignoring metadata for non-current vehicle: " .. tostring(metadata.vehicleId))
    return
  end

  metadata.vehicle = safeToString(metadata.vehicle)
  metadata.configuration = safeToString(metadata.configuration)

  -- VLUA powertrain data is authoritative for transmission, but the vehicle
  -- VM does not reliably expose identity fields. Keep the GE-side identity
  -- (JBeam + partConfig) as a fallback so a valid vehicle request is always
  -- sent even when VLUA returns blank vehicle/configuration fields.
  if metadata.vehicle == "" and currentVehicleCode ~= nil then
    metadata.vehicle = currentVehicleCode
    log("VLUA did not return vehicle codename; using GE fallback: " .. metadata.vehicle)
  end
  if metadata.configuration == "" and currentConfigurationCode ~= nil then
    metadata.configuration = currentConfigurationCode
    log("VLUA did not return configuration codename; using GE fallback: " .. metadata.configuration)
  end
  if safeToString(metadata.partConfig) == "" and currentPartConfig ~= nil then
    metadata.partConfig = currentPartConfig
  end

  metadata.vehicleType = normalizeVehicleType(metadata.vehicleType)
  metadata.transmission = normalizeTransmission(metadata.transmission, metadata.transmissionRaw)
  metadata.gearLayout = normalizeGearLayout(metadata.gearLayout)

  currentVehicleCode = metadata.vehicle
  currentConfigurationCode = metadata.configuration
  metadataPendingVehicleId = nil
  metadataRetriesLeft = 0

  log("========================================")
  log("VEHICLE METADATA")
  log("========================================")
  log("Vehicle ID = " .. tostring(vehicleId))
  log("Game = BeamNG.drive")
  log("Vehicle codename = " .. (metadata.vehicle ~= "" and metadata.vehicle or "<unknown>"))
  log("Configuration codename = " .. (metadata.configuration ~= "" and metadata.configuration or "<unknown>"))
  log("Part config = " .. safeToString(metadata.partConfig))
  log("JBeam = " .. safeToString(metadata.jBeam))
  log("Vehicle type = " .. (metadata.vehicleType ~= "" and metadata.vehicleType or "<unknown>"))
  log("Transmission = " .. (metadata.transmission ~= "" and metadata.transmission or "<unknown>"))
  log("Transmission raw = " .. safeToString(metadata.transmissionRaw))
  log("Gearbox mode = " .. safeToString(metadata.gearboxMode))
  log("Gear = " .. safeToString(metadata.gear))
  log("Gear index = " .. safeToString(metadata.gearIndex))
  log("Gear layout = " .. (metadata.gearLayout ~= "" and metadata.gearLayout or "<unknown>"))
  log("Automatic modes = " .. safeToString(metadata.automaticModes))
  log("========================================")
  log("END VEHICLE METADATA")
  log("========================================")

  sendVehicleRequest(metadata, "VLUA")
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
      local vehicleId = getPlayerVehicleId()
      if vehicleId ~= nil then
        log("Initial player vehicle detected: " .. tostring(vehicleId))
        handleVehicleChange(vehicleId, "startup")
        return
      end
      job.sleep(0.5)
      elapsed = elapsed + 0.5
    end

    if initialized then
      log("No player vehicle detected during startup search.")
    end
  end)
end

local function onExtensionUnloaded()
  log("Extension unloading.")
  initialized = false
  currentVehicleId = nil
  currentVehicleCode = nil
  currentConfigurationCode = nil
  currentPartConfig = nil
  metadataPendingVehicleId = nil
  metadataRetriesLeft = 0

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
