local M = {}

local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458

local initialized = false
local currentVehicleId = nil
local currentVehicleCode = ""
local currentConfigurationCode = ""
local currentPartConfig = ""
local currentMetadata = nil
local currentState = nil

local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0

local metadataTimer = 0
local metadataPendingVehicleId = nil
local METADATA_RETRY_INTERVAL = 0.25
local METADATA_RETRY_COUNT = 12
local metadataRetriesLeft = 0

local heartbeatTimer = 0
local HELLO_INTERVAL = 2.0
local HELPER_TIMEOUT = 6.0
local timeSinceHelperAck = HELPER_TIMEOUT + 1
local helperConnected = false

local lastVehicleCommandSignature = nil
local lastStateCommandSignature = nil
local sendVehicleRequest = nil
local sendVehicleState = nil

local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end

local function safeToString(value)
  if value == nil then return "" end
  return tostring(value)
end

local function firstNonEmpty(...)
  for i = 1, select("#", ...) do
    local value = select(i, ...)
    if value ~= nil then
      local text = tostring(value)
      if text ~= "" then
        return value
      end
    end
  end
  return nil
end


local function sendCommand(command, quiet)
  if udp == nil then
    if not quiet then log("Cannot send command; UDP is not initialized.") end
    return false
  end

  local ok, err = pcall(function()
    udp:sendto(command, UDP_HOST, UDP_PORT)
  end)
  if not ok then
    log("UDP send failed: " .. tostring(err))
    return false
  end
  if not quiet then log("TX: " .. tostring(command)) end
  return true
end

local function sendHello()
  return sendCommand("HELLO|BeamNG.drive", true)
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
  if udp ~= nil then return true end

  local ok, result = pcall(function() return require("socket") end)
  if not ok then
    log("Failed to load socket module: " .. tostring(result))
    return false
  end
  socket = result

  local socketOK, socketResult = pcall(function() return socket.udp() end)
  if not socketOK or socketResult == nil then
    log("Failed to create UDP socket: " .. tostring(socketResult))
    return false
  end

  udp = socketResult
  pcall(function() udp:settimeout(0) end)
  log("UDP initialized: " .. UDP_HOST .. ":" .. tostring(UDP_PORT))
  sendHello()
  return true
end

local function pollUdp()
  if udp == nil then return end
  for _ = 1, 8 do
    local ok, data = pcall(function()
      return udp:receivefrom()
    end)
    if not ok or data == nil then break end

    if data == "HELLO_ACK|MultiFFBJoy" or data:sub(1, 18) == "HELLO_ACK|MultiFFBJoy" then
      timeSinceHelperAck = 0
      if not helperConnected then
        helperConnected = true
        log("========================================")
        log("FFB helper connection is ALIVE.")
        log("========================================")
        -- Re-send the active profile/state after a helper restart.
        if currentMetadata ~= nil then
          sendVehicleRequest(currentMetadata, "helper reconnect")
        end
        if currentState ~= nil then
          sendVehicleState(currentState, "helper reconnect")
        end
      end
    elseif data == "ACK|VEHICLE" then
      timeSinceHelperAck = 0
    elseif data == "ACK|STATE" then
      timeSinceHelperAck = 0
    elseif data:sub(1, 6) == "SHIFT|" then
      local zone, index = data:match("^SHIFT|([^|]*)|?(.*)$")
      executeGearSelection(zone, index)
    end
  end
end

local function updateHelperConnection(dtReal)
  timeSinceHelperAck = timeSinceHelperAck + (dtReal or 0)
  heartbeatTimer = heartbeatTimer + (dtReal or 0)

  if heartbeatTimer >= HELLO_INTERVAL then
    heartbeatTimer = 0
    sendHello()
  end

  if helperConnected and timeSinceHelperAck > HELPER_TIMEOUT then
    helperConnected = false
    log("FFB helper connection appears to be LOST.")
  end
end

local function getPlayerVehicleId()
  if be == nil then return nil end
  local ok, result = pcall(function() return be:getPlayerVehicleID(0) end)
  if not ok then
    log("getPlayerVehicleID failed: " .. tostring(result))
    return nil
  end
  if result == nil or result < 0 then return nil end
  return result
end

local function getPlayerVehicle()
  if be == nil then return nil end
  local ok, result = pcall(function() return be:getPlayerVehicle(0) end)
  return ok and result or nil
end

local function normalizeVehicleType(value)
  if value == nil then return "" end
  local text = tostring(value)
  if text == "" then return "" end
  local aliases = {
    car="Car", cars="Car", truck="Truck", trucks="Truck",
    aircraft="Aircraft", plane="Aircraft", planes="Aircraft", helicopter="Aircraft",
    boat="Boat", boats="Boat", motorcycle="Motorcycle", motorcycles="Motorcycle",
    trailer="Trailer", trailers="Trailer", prop="Prop", utility="Utility",
  }
  return aliases[string.lower(text)] or text
end

local function normalizeTransmission(value, rawType)
  local text = value
  if text == nil or text == "" then text = rawType end
  if text == nil then return "" end
  text = tostring(text)
  local lower = string.lower(text)
  if lower == "automaticgearbox" or lower == "automatic" then return "Automatic"
  elseif lower == "manualgearbox" or lower == "manual" then return "Manual"
  elseif lower == "sequentialgearbox" or lower == "sequential" then return "Sequential"
  elseif lower == "dctgearbox" or lower == "dct" then return "DCT"
  elseif lower == "cvtgearbox" or lower == "cvt" then return "CVT"
  elseif lower == "electricmotor" or lower == "electric" then return "Electric"
  elseif lower == "dummy" or lower == "shiftlogic-dummy" then return "" end
  return text
end

local function normalizeGearLayout(value)
  if value == nil then return "" end
  return tostring(value)
end

sendVehicleRequest = function(metadata, reason)
  if metadata == nil then return false end
  local vehicleCode = safeToString(metadata.vehicle)
  if vehicleCode == "" then
    log("Cannot send vehicle profile request without a vehicle codename.")
    return false
  end

  local game = safeToString(metadata.game)
  if game == "" then game = "BeamNG.drive" end
  local vehicleType = normalizeVehicleType(metadata.vehicleType)
  local transmission = normalizeTransmission(metadata.transmission, metadata.transmissionRaw)
  local configurationCode = safeToString(metadata.configuration)
  local gearLayout = normalizeGearLayout(metadata.gearLayout)

  local command = table.concat({
    "VEHICLE", game, vehicleType, vehicleCode, configurationCode,
    transmission, gearLayout
  }, "|")
  local signature = command

  if signature == lastVehicleCommandSignature then return true end
  lastVehicleCommandSignature = signature

  log("Vehicle profile request (" .. tostring(reason or "unknown") .. "):")
  log("  Game = " .. game)
  log("  VehicleType = " .. (vehicleType ~= "" and vehicleType or "<unknown>"))
  log("  Vehicle = " .. vehicleCode)
  log("  Configuration = " .. (configurationCode ~= "" and configurationCode or "<unknown>"))
  log("  Transmission = " .. (transmission ~= "" and transmission or "<unknown>"))
  log("  GearLayout = " .. (gearLayout ~= "" and gearLayout or "<unknown>"))
  return sendCommand(command)
end

sendVehicleState = function(state, reason)
  if state == nil then return false end
  local gear = safeToString(state.gear)
  local gearIndex = safeToString(state.gearIndex)
  local gearboxMode = safeToString(state.gearboxMode)
  local gearPosition = safeToString(state.gearPosition)
  local transmission = normalizeTransmission(state.transmission, state.transmissionRaw)

  local command = table.concat({
    "STATE", safeToString(state.vehicleId), safeToString(state.vehicle),
    safeToString(state.configuration), transmission, gear, gearIndex,
    gearboxMode, gearPosition, safeToString(state.automaticModes)
  }, "|")
  local signature = command
  if signature == lastStateCommandSignature then return true end
  lastStateCommandSignature = signature

  log("TX STATE" .. (reason and (" (" .. tostring(reason) .. ")") or "")
    .. ": gear=" .. (gear ~= "" and gear or "<unknown>")
    .. " gearIndex=" .. (gearIndex ~= "" and gearIndex or "<unknown>")
    .. " position=" .. (gearPosition ~= "" and gearPosition or "<unknown>"))
  return sendCommand(command, true)
end

local function executeGearSelection(zoneName, requestedIndex)
  local vehicle = getPlayerVehicle()
  if vehicle == nil then
    log("Cannot execute FFB gear selection: no player vehicle.")
    return false
  end

  local index = tonumber(requestedIndex)
  local name = safeToString(zoneName)

  local command = nil

  if index ~= nil then
    command = string.format(
      "local c=controller.getController('main'); if c and c.shiftToGearIndex then c.shiftToGearIndex(%d) end",
      math.floor(index)
    )
  elseif name ~= "" then
    local escaped = string.format("%q", name)
    command = string.format(
      "local c=controller.getController('main'); if c and c.getGearName and c.shiftToGearIndex then local n=c.getGearName(); if n==%s then return end end",
      escaped
    )
  end

  if command == nil then
    log("FFB gear selection has no usable index.")
    return false
  end

  local ok, err = pcall(function()
    vehicle:queueLuaCommand(command)
  end)

  if not ok then
    log("Failed to queue FFB gear selection: " .. tostring(err))
    return false
  end

  log("FFB zone gear selection queued: " .. name ..
      (index ~= nil and (" -> index " .. tostring(math.floor(index))) or ""))
  return true
end

local function queueVehicleMetadataDiagnostic(vehicleId, reason, resetRetries)
  local vehicle = getPlayerVehicle()
  if vehicle == nil then
    log("Cannot queue VLUA metadata diagnostic; no player vehicle object.")
    return false
  end

  metadataPendingVehicleId = vehicleId

  local partConfig = ""
  local jBeam = ""
  pcall(function() partConfig = safeToString(vehicle.partConfig) end)
  pcall(function() jBeam = safeToString(vehicle.JBeam or vehicle.jBeam) end)
  if partConfig ~= "" then currentPartConfig = partConfig end
  if jBeam ~= "" then currentVehicleCode = jBeam end

  if currentConfigurationCode == "" and partConfig ~= "" then
    local normalized = partConfig:gsub("\\", "/")
    local _, parsedConfiguration = normalized:match("vehicles/([^/]+)/([^/]+)%.pc$")
    if parsedConfiguration ~= nil then currentConfigurationCode = parsedConfiguration end
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
  local ok, result = pcall(function() vehicle:queueLuaCommand(command) end)
  if not ok then
    log("Failed to queue VLUA metadata diagnostic: " .. tostring(result))
    return false
  end
  log("VLUA metadata request queued for vehicle " .. tostring(vehicleId) .. " (" .. tostring(reason or "unknown") .. ").")
  return true
end

local function handleVehicleChange(vehicleId, reason)
  if vehicleId == nil or vehicleId < 0 then
    currentVehicleId = vehicleId
    currentMetadata = nil
    currentState = nil
    metadataPendingVehicleId = nil
    metadataRetriesLeft = 0
    return
  end

  local changed = vehicleId ~= currentVehicleId
  if changed then
    log("Player vehicle changed: " .. tostring(currentVehicleId) .. " -> " .. tostring(vehicleId) .. " (" .. tostring(reason or "unknown") .. ")")
    currentVehicleId = vehicleId
    currentVehicleCode = ""
    currentConfigurationCode = ""
    currentPartConfig = ""
    currentMetadata = nil
    currentState = nil
    lastVehicleCommandSignature = nil
    lastStateCommandSignature = nil
    requestReacquire()
    queueVehicleMetadataDiagnostic(vehicleId, reason, true)
  elseif metadataPendingVehicleId == nil then
    -- Ignore duplicate switch/startup callbacks once this vehicle is known.
    return
  end
end

function M.receiveVehicleMetadata(metadata)
  if type(metadata) ~= "table" then
    log("Received invalid vehicle metadata payload.")
    return
  end
  local vehicleId = tonumber(metadata.vehicleId)
  if vehicleId == nil or vehicleId ~= currentVehicleId then return end

  metadata.vehicle = safeToString(metadata.vehicle)
  metadata.configuration = safeToString(metadata.configuration)
  if metadata.vehicle == "" and currentVehicleCode ~= "" then
    metadata.vehicle = currentVehicleCode
    log("VLUA did not return vehicle codename; using GE fallback: " .. metadata.vehicle)
  end
  if metadata.configuration == "" and currentConfigurationCode ~= "" then
    metadata.configuration = currentConfigurationCode
    log("VLUA did not return configuration codename; using GE fallback: " .. metadata.configuration)
  end
  if safeToString(metadata.partConfig) == "" then metadata.partConfig = currentPartConfig end

  -- Vehicle Lua may not expose the model Type directly.  The GE vehicle
  -- manager does, however, expose model metadata for the active vehicle.
  -- Use it as the authoritative type fallback (Car, Aircraft, Truck, etc.).
  if safeToString(metadata.vehicleType) == "" then
    local modelKey = firstNonEmpty(metadata.vehicle, currentVehicleCode)
    local modelType = nil

    local okModel, result = pcall(function()
      if core_vehicles
        and core_vehicles.getModel
        and modelKey ~= nil
        and modelKey ~= "" then
        return core_vehicles.getModel(modelKey)
      end
      return nil
    end)

    if okModel and type(result) == "table" then
      -- BeamNG returns model metadata in result.model.
      if type(result.model) == "table" then
        modelType = firstNonEmpty(
          result.model.Type,
          result.model.type,
          result.model.vehicleType,
          result.model.category
        )
      end

      modelType = firstNonEmpty(
        modelType,
        result.Type,
        result.type,
        result.vehicleType,
        result.category
      )
    end

    if modelType ~= nil and tostring(modelType) ~= "" then
      metadata.vehicleType = tostring(modelType)
      log("Vehicle type from GE model metadata (" ..
          tostring(modelKey) .. "): " .. tostring(modelType))
    else
      log("GE model metadata did not provide a vehicle type for " ..
          tostring(modelKey or "<unknown>"))
    end
  end

  metadata.vehicleType = normalizeVehicleType(metadata.vehicleType)
  metadata.transmission = normalizeTransmission(metadata.transmission, metadata.transmissionRaw)
  metadata.gearLayout = normalizeGearLayout(metadata.gearLayout)
  metadata.game = "BeamNG.drive"

  currentVehicleCode = metadata.vehicle
  currentConfigurationCode = metadata.configuration
  currentPartConfig = safeToString(metadata.partConfig)
  currentMetadata = metadata
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
  log("Gear position = " .. safeToString(metadata.gearPosition))
  log("Gear layout = " .. (metadata.gearLayout ~= "" and metadata.gearLayout or "<unknown>"))
  log("Automatic modes = " .. safeToString(metadata.automaticModes))
  log("========================================")
  log("END VEHICLE METADATA")
  log("========================================")

  sendVehicleRequest(metadata, "metadata")
  sendVehicleState(metadata, "metadata")
end

function M.receiveVehicleState(state)
  if type(state) ~= "table" then return end
  local vehicleId = tonumber(state.vehicleId)
  if vehicleId == nil or vehicleId ~= currentVehicleId then return end

  state.transmission = normalizeTransmission(state.transmission, state.transmissionRaw)
  currentState = state
  sendVehicleState(state)
end

local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then return end
  if udp == nil then initializeUDP() end
  pollUdp()
  updateHelperConnection(dtReal)

  if metadataPendingVehicleId == nil or metadataRetriesLeft <= 0 then return end
  metadataTimer = metadataTimer - (dtReal or 0)
  if metadataTimer > 0 then return end
  metadataTimer = METADATA_RETRY_INTERVAL
  metadataRetriesLeft = metadataRetriesLeft - 1

  local playerId = getPlayerVehicleId()
  if playerId ~= metadataPendingVehicleId then return end
  queueVehicleMetadataDiagnostic(metadataPendingVehicleId, "retry", false)
end

local function onVehicleSwitched(oldId, newId)
  log("onVehicleSwitched: " .. tostring(oldId) .. " -> " .. tostring(newId))
  handleVehicleChange(newId, "switch")
end

local function onVehicleSpawned(vehicleId)
  log("onVehicleSpawned: " .. tostring(vehicleId))
  if getPlayerVehicleId() == vehicleId then handleVehicleChange(vehicleId, "spawn") end
end

local function onPlayerVehicleChanged(vehicleId)
  log("onPlayerVehicleChanged: " .. tostring(vehicleId))
  handleVehicleChange(vehicleId, "player-change")
end

local function onExtensionLoaded()
  if initialized then return end
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
    if initialized then log("No player vehicle detected during startup search.") end
  end)
end

local function onExtensionUnloaded()
  log("Extension unloading.")
  initialized = false
  currentVehicleId = nil
  currentMetadata = nil
  currentState = nil
  if udp ~= nil then pcall(function() udp:close() end) end
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
