local M = {}
-- ============================================================================
-- MultiFFBJoy - BeamNG GE extension
--
-- Responsibilities:
--   1. Detect the current player vehicle.
--   2. Extract stable vehicle/configuration identifiers.
--   3. Inspect transmission-related metadata.
--   4. Load Configuration.txt from the mod VFS.
--   5. Resolve the most-specific matching FFB profile.
--   6. Tell the external MultiFFBJoy helper which preset to use.
--
-- Configuration hierarchy:
--
-- Profiles
--     BeamNG.drive
--         Vehicle
--             Car=PRND
--                 miramar=PRND21
--                     base=5RDR
--                     luxe_A=PRNDL
--             Aircraft=Flightstick
--         Transmission
--             Automatic=PRND
--             Manual=5RDR
--
-- More-specific entries override less-specific entries.
-- ============================================================================
-- ============================================================================
-- Constants
-- ============================================================================
local GAME_NAME = "BeamNG.drive"
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local CONFIG_FILENAME = "Configuration.txt"
-- How often the fallback vehicle detector runs.
-- Event callbacks are preferred; this is only a safety net.
local VEHICLE_POLL_INTERVAL = 0.50
-- Prevent duplicate REACQUIRE messages during vehicle replacement.
local REACQUIRE_COOLDOWN = 1.0
-- Configuration reload retry window.
local CONFIG_RETRY_INTERVAL = 0.25
local CONFIG_RETRY_COUNT = 20
-- ============================================================================
-- Module state
-- ============================================================================
local socket = nil
local udp = nil
local initialized = false
local currentVehicleId = nil
local currentVehicle = nil
local currentIdentity = nil
local currentProfile = nil
local configurationText = nil
local configurationPath = nil
local configLoaded = false
local configLoadAttempted = false
local lastReacquireTime = -1000
local vehiclePollTimer = 0
local configurationRetryTimer = 0
local configReloadPending = false
local metadataRefreshPending = false
-- ============================================================================
-- Logging
-- ============================================================================
local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end
local function logSeparator(title)
  log("========================================")
  if title ~= nil then
    log(tostring(title))
    log("========================================")
  end
end
-- ============================================================================
-- Utility functions
-- ============================================================================
local function trim(value)
  if value == nil then
    return ""
  end
  value = tostring(value)
  return value:match("^%s*(.-)%s*$") or ""
end
local function normalize(value)
  value = trim(value)
  if value == "" then
    return ""
  end
  return string.lower(value)
end
local function isValidVehicleId(vehicleId)
  return vehicleId ~= nil
  and type(vehicleId) == "number"
  and vehicleId > 0
end
local function stripExtension(value)
  if value == nil then
    return nil
  end
  return tostring(value):gsub("%.[^%.]+$", "")
end
local function basename(path)
  if path == nil then
    return nil
  end
  path = tostring(path)
  path = path:gsub("\\", "/")
  local result = path:match("([^/]+)$")
  return result
end
local function getBoolean(value)
  if value == true then
    return true
  end
  if value == false then
    return false
  end
  if type(value) == "string" then
    local n = normalize(value)
    if n == "true"
      or n == "yes"
      or n == "1"
      or n == "on" then
        return true
      end
      if n == "false"
        or n == "no"
        or n == "0"
        or n == "off" then
          return false
        end
      end
      return nil
    end
-- ============================================================================
-- UDP
-- ============================================================================
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
local function requestReacquire(force)
  local now = os.clock()
  if not force
    and now - lastReacquireTime < REACQUIRE_COOLDOWN then
      log("REACQUIRE suppressed by cooldown.")
      return false
    end
    lastReacquireTime = now
    log("Requesting FFB device re-acquisition.")
    return sendCommand("REACQUIRE")
  end
  local function requestPreset(preset)
    if preset == nil or trim(preset) == "" then
      log("No FFB preset selected.")
      return false
    end
    preset = trim(preset)
    log("Requesting FFB preset: " .. preset)
    return sendCommand("PRESET " .. preset)
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
-- ============================================================================
-- Vehicle access
-- ============================================================================
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
  if not isValidVehicleId(result) then
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
  if result == nil then
    return nil
  end
  return result
end
local function getVehicleById(vehicleId)
  if be == nil or not isValidVehicleId(vehicleId) then
    return nil
  end
  local ok, result = pcall(function()
    return be:getObjectByID(vehicleId)
  end)
  if not ok then
    return nil
  end
  return result
end
-- ============================================================================
-- Safe vehicle field access
-- ============================================================================
local function safeField(vehicle, fieldName)
  if vehicle == nil then
    return nil
  end
  local ok, value = pcall(function()
    return vehicle[fieldName]
  end)
  if not ok then
    return nil
  end
  return value
end
local function safeGetField(vehicle, fieldName)
  if vehicle == nil then
    return nil
  end
  local ok, value = pcall(function()
    if vehicle.getField == nil then
      return nil
    end
    return vehicle:getField(fieldName, "")
  end)
  if not ok then
    return nil
  end
  if value == nil or value == "" then
    return nil
  end
  return value
end
local function safeMethod(vehicle, methodName)
  if vehicle == nil then
    return nil
  end
  local ok, method = pcall(function()
    return vehicle[methodName]
  end)
  if not ok or type(method) ~= "function" then
    return nil
  end
  local callOK, result = pcall(function()
    return method(vehicle)
  end)
  if not callOK then
    return nil
  end
  return result
end
-- ============================================================================
-- Vehicle identity
-- ============================================================================
local function getVehicleJBeam(vehicle)
  local candidates = {
    safeField(vehicle, "JBeam"),
    safeField(vehicle, "jBeam"),
    safeGetField(vehicle, "JBeam"),
    safeGetField(vehicle, "jBeam"),
    safeMethod(vehicle, "getJBeamFilename")
  }
  for i = 1, #candidates do
    local value = trim(candidates[i])
    if value ~= "" then
      value = value:gsub("\\", "/")
      value = basename(value)
      value = stripExtension(value)
      if value ~= "" then
        return value
      end
    end
  end
  return nil
end
local function getPartConfig(vehicle)
  local candidates = {
    safeField(vehicle, "partConfig"),
    safeGetField(vehicle, "partConfig"),
    safeField(vehicle, "config"),
    safeGetField(vehicle, "config")
  }
  for i = 1, #candidates do
    local value = trim(candidates[i])
    if value ~= "" then
      return value:gsub("\\", "/")
    end
  end
  return nil
end
local function parsePartConfig(partConfig)
  if partConfig == nil then
    return nil, nil
  end
  local normalized = tostring(partConfig):gsub("\\", "/")
  local vehicleCode =
  normalized:match("/vehicles/([^/]+)/")
  if vehicleCode == nil then
    vehicleCode =
    normalized:match("^vehicles/([^/]+)/")
  end
  local configFile =
  normalized:match("/([^/]+)%.pc$")
  if configFile == nil then
    configFile =
    normalized:match("^vehicles/[^/]+/([^/]+)%.pc$")
  end
  if vehicleCode == nil then
    return nil, nil
  end
  return vehicleCode, configFile
end
-- ============================================================================
-- Vehicle type detection
--
-- BeamNG does not guarantee a single universally-populated vehicle.type field.
-- Therefore this is intentionally layered.
-- ============================================================================
local function classifyVehicle(vehicle, jbeam)
  local candidates = {
    safeField(vehicle, "vehicleType"),
    safeField(vehicle, "type"),
    safeField(vehicle, "category"),
    safeGetField(vehicle, "vehicleType"),
    safeGetField(vehicle, "type"),
    safeGetField(vehicle, "category")
  }
  for i = 1, #candidates do
    local value = trim(candidates[i])
    if value ~= "" then
      local n = normalize(value)
      if n:find("aircraft", 1, true)
        or n:find("plane", 1, true) then
          return "Aircraft"
        end
        if n:find("helicopter", 1, true) then
          return "Helicopter"
        end
        if n:find("boat", 1, true)
          or n:find("water", 1, true) then
            return "Boat"
          end
          if n:find("truck", 1, true) then
            return "Truck"
          end
          if n:find("bus", 1, true) then
            return "Bus"
          end
          if n:find("car", 1, true)
            or n:find("vehicle", 1, true) then
              return "Car"
            end
          end
        end
-- Fallback:
--
-- BeamNG's normal ground vehicles generally expose JBeam identifiers
-- without an explicit "vehicleType" field. We deliberately don't pretend
-- to know more than we do here.
--
-- This allows configuration profiles to still match the stable vehicle
-- identifier even if category detection isn't available.
if jbeam ~= nil then
  return nil
end
return nil
end
-- ============================================================================
-- Transmission diagnostics
--
-- We probe a broad set of names because different BeamNG versions / vehicle
-- controllers expose different information to GE Lua.
--
-- The result is normalized to:
--   Automatic
--   Manual
--   Sequential
--   CVT
--   DCT
--   Unknown
-- ============================================================================
local TRANSMISSION_FIELDS = {
  "transmissionType",
  "Transmission",
  "transmission",
  "gearboxType",
  "gearbox",
  "gearboxMode",
  "shiftMode",
  "shiftlogic",
  "shiftLogic",
  "gearMode",
  "gearboxName"
}
local function classifyTransmissionValue(value)
  if value == nil then
    return nil
  end
  local n = normalize(value)
  if n == "" then
    return nil
  end
  if n:find("dual", 1, true)
    or n:find("dct", 1, true) then
      return "DCT"
    end
    if n:find("cvt", 1, true) then
      return "CVT"
    end
    if n:find("sequential", 1, true)
      or n:find("seq", 1, true) then
        return "Sequential"
      end
      if n:find("manual", 1, true)
        or n:find("mt", 1, true) then
          return "Manual"
        end
        if n:find("automatic", 1, true)
          or n == "at"
          or n:find("auto", 1, true) then
            return "Automatic"
          end
          return nil
        end
        local function getTransmission(vehicle)
          for i = 1, #TRANSMISSION_FIELDS do
            local fieldName = TRANSMISSION_FIELDS[i]
            local value =
            safeField(vehicle, fieldName)
            local result =
            classifyTransmissionValue(value)
            if result ~= nil then
              return result, fieldName, value
            end
            value =
            safeGetField(vehicle, fieldName)
            result =
            classifyTransmissionValue(value)
            if result ~= nil then
              return result, fieldName, value
            end
          end
          return nil, nil, nil
        end
-- ============================================================================
-- Additional metadata
-- ============================================================================
local function getMetadata(vehicle, vehicleId)
  local jbeam = getVehicleJBeam(vehicle)
  local partConfig = getPartConfig(vehicle)
  local parsedVehicle, parsedConfig =
  parsePartConfig(partConfig)
  if jbeam == nil then
    jbeam = parsedVehicle
  end
  local configuration = parsedConfig
  local vehicleType =
  classifyVehicle(vehicle, jbeam)
  local transmission,
  transmissionField,
  transmissionRaw =
  getTransmission(vehicle)
  local metadata = {
    vehicleId = vehicleId,
    vehicle = jbeam,
    configuration = configuration,
    partConfig = partConfig,
    vehicleType = vehicleType,
    transmission = transmission,
    transmissionField = transmissionField,
    transmissionRaw = transmissionRaw
  }
  return metadata
end
-- ============================================================================
-- Metadata logging
-- ============================================================================
local function logMetadata(metadata)
  if metadata == nil then
    return
  end
  logSeparator("VEHICLE METADATA")
  log("Vehicle ID = " .. tostring(metadata.vehicleId))
  log("Game = " .. GAME_NAME)
  log("Vehicle codename = " .. tostring(metadata.vehicle))
  log("Configuration codename = " .. tostring(metadata.configuration))
  log("Part config = " .. tostring(metadata.partConfig))
  log("Vehicle type = " .. tostring(metadata.vehicleType))
  log(
    "Transmission = "
    .. tostring(metadata.transmission)
    )
  log(
    "Transmission field = "
    .. tostring(metadata.transmissionField)
    )
  log(
    "Transmission raw = "
    .. tostring(metadata.transmissionRaw)
    )
  logSeparator("END VEHICLE METADATA")
end
-- ============================================================================
-- Configuration.txt parser
--
-- We intentionally implement this as a small indentation tree parser.
--
-- Accepted:
--
-- Profiles
--     BeamNG.drive
--         Vehicle
--             Car=PRND
--                 miramar=PRND21
--                     luxe_A=PRNDL
--         Transmission
--             Automatic=PRND
--             Manual=5RDR
--
-- Blank lines and comments beginning with # or ; are ignored.
-- ============================================================================
local function parseConfigLine(line)
  if line == nil then
    return nil
  end
  line = line:gsub("\r", "")
  if line:match("^%s*$") then
    return nil
  end
  local trimmed = trim(line)
  if trimmed == ""
    or trimmed:sub(1, 1) == "#"
    or trimmed:sub(1, 1) == ";" then
      return nil
    end
-- Tabs are preferred.
-- We also tolerate groups of spaces for convenience.
local prefix = line:match("^(%s*)")
local indentation = 0
for i = 1, #prefix do
  local ch = prefix:sub(i, i)
  if ch == "\t" then
    indentation = indentation + 1
  elseif ch == " " then
-- Treat four spaces as one indentation level.
indentation = indentation + 0.25
end
end
indentation = math.floor(indentation + 0.0001)
if indentation < 0 then
  indentation = 0
end
local key, value =
trimmed:match("^(.-)%s*=%s*(.-)%s*$")
if key == nil then
  key = trimmed
  value = nil
end
key = trim(key)
if value ~= nil then
  value = trim(value)
end
return {
  indent = indentation,
  key = key,
  value = value
}
end
local function createConfigNode(key, value)
  return {
    key = key,
    value = value,
    children = {}
  }
end
local function parseConfiguration(text)
  local root =
  createConfigNode("__ROOT__", nil)
  local stack = {
    [-1] = root
  }
  local lineNumber = 0
  for line in text:gmatch("[^\n]+") do
    lineNumber = lineNumber + 1
    local parsed =
    parseConfigLine(line)
    if parsed ~= nil then
      local indent = parsed.indent
      while stack[indent - 1] == nil
        and indent > 0 do
          indent = indent - 1
        end
        local parent =
        stack[indent - 1]
        if parent == nil then
          parent = root
        end
        local node =
        createConfigNode(
          parsed.key,
          parsed.value
          )
        node.line = lineNumber
        parent.children[#parent.children + 1] =
        node
        stack[indent] = node
-- Discard deeper indentation levels.
local nextLevel = indent + 1
while stack[nextLevel] ~= nil do
  stack[nextLevel] = nil
  nextLevel = nextLevel + 1
end
end
end
return root
end
-- ============================================================================
-- Configuration file loading
-- ============================================================================
local function readTextFile(path)
  if path == nil then
    return nil
  end
-- BeamNG exposes readFile() in the Lua environment.
if type(readFile) == "function" then
  local ok, result =
  pcall(function()
    return readFile(path)
  end)
  if ok and type(result) == "string" then
    return result
  end
end
return nil
end
local function findConfigurationFile()
  if type(findFiles) ~= "function" then
    log("findFiles() is unavailable.")
    return nil
  end
  local searchRoots = {
    "mods/unpacked/multiffbjoy",
    "/mods/unpacked/multiffbjoy",
    "multiffbjoy",
    "/multiffbjoy"
  }
  for i = 1, #searchRoots do
    local root = searchRoots[i]
    local ok, result =
    pcall(function()
      return findFiles(
        root,
        CONFIG_FILENAME,
        10,
        true,
        false
        )
    end)
    if ok
      and type(result) == "table"
      and #result > 0 then
        for j = 1, #result do
          local path = result[j]
          if path ~= nil then
            path = tostring(path)
            if path:lower():sub(-#CONFIG_FILENAME)
              == CONFIG_FILENAME:lower() then
                return path
              end
            end
          end
        end
      end
      return nil
    end
    local function loadConfiguration()
      configLoadAttempted = true
      local path =
      findConfigurationFile()
      if path == nil then
        log(
          "Could not locate "
          .. CONFIG_FILENAME
          .. " through the BeamNG VFS."
          )
        configLoaded = false
        configurationText = nil
        configurationPath = nil
        return false
      end
      local text =
      readTextFile(path)
      if text == nil then
        log(
          "Configuration file found but could not be read: "
          .. tostring(path)
          )
        configLoaded = false
        configurationText = nil
        configurationPath = nil
        return false
      end
      configurationText = text
      configurationPath = path
      configLoaded = true
      log(
        "Configuration loaded: "
        .. tostring(path)
        )
      return true
    end
-- ============================================================================
-- Configuration tree matching
-- ============================================================================
local function keyMatches(actual, configured)
  if actual == nil or configured == nil then
    return false
  end
  actual = normalize(actual)
  configured = normalize(configured)
  if actual == "" or configured == "" then
    return false
  end
-- Exact internal identifiers / names.
if actual == configured then
  return true
end
-- Allow quoted names in Configuration.txt.
if configured:sub(1, 1) == '"'
  and configured:sub(-1) == '"' then
    configured =
    configured:sub(
      2,
      #configured - 1
      )
    return actual == normalize(configured)
  end
  return false
end
local function findChild(parent, key)
  if parent == nil then
    return nil
  end
  local children =
  parent.children
  if children == nil then
    return nil
  end
  for i = 1, #children do
    local child = children[i]
    if keyMatches(key, child.key) then
      return child
    end
  end
  return nil
end
local function findPresetAtNode(node)
  if node == nil then
    return nil
  end
  if node.value ~= nil
    and trim(node.value) ~= "" then
      return trim(node.value)
    end
    return nil
  end
-- ============================================================================
-- Hierarchical profile resolution
--
-- Resolution order:
--
--   1. Vehicle + configuration
--   2. Vehicle
--   3. Vehicle category
--   4. Transmission
--
-- The most specific matching entry wins.
--
-- Example:
--
-- Car=PRND
--     miramar=PRND21
--         luxe_A=PRNDL
--
-- For miramar/luxe_A:
--     PRNDL
--
-- For miramar/base:
--     PRND21
--
-- For another Car:
--     PRND
--
-- If transmission is Automatic:
--     Transmission/Automatic=PRND
-- ============================================================================
local function resolveVehicleProfile(metadata)
  if not configLoaded
    or configurationText == nil then
      return nil, "configuration not loaded"
    end
    local root =
    parseConfiguration(configurationText)
    local profiles =
    findChild(root, "Profiles")
    if profiles == nil then
      return nil, "Profiles section not found"
    end
    local game =
    findChild(profiles, GAME_NAME)
    if game == nil then
      return nil, "game not found"
    end
    local bestPreset = nil
    local bestScore = -1
    local bestDescription = nil
-- --------------------------------------------------------------------------
-- Vehicle branch
-- --------------------------------------------------------------------------
local vehicleSection =
findChild(game, "Vehicle")
if vehicleSection ~= nil then
  local categoryNode = nil
  if metadata.vehicleType ~= nil then
    categoryNode =
    findChild(
      vehicleSection,
      metadata.vehicleType
      )
  end
-- If the vehicle category is unavailable, search every category.
local categoryCandidates = {}
if categoryNode ~= nil then
  categoryCandidates[1] = categoryNode
else
  for i = 1, #vehicleSection.children do
    categoryCandidates[#categoryCandidates + 1] =
    vehicleSection.children[i]
  end
end
for i = 1, #categoryCandidates do
  local category =
  categoryCandidates[i]
-- Category-level preset, e.g.:
--
-- Aircraft=Flightstick
local categoryPreset =
findPresetAtNode(category)
if categoryPreset ~= nil then
  local score = 10
  if score > bestScore then
    bestScore = score
    bestPreset = categoryPreset
    bestDescription =
    "Vehicle category: "
    .. tostring(category.key)
  end
end
-- Vehicle-level preset.
local vehicleNode =
findChild(
  category,
  metadata.vehicle
  )
if vehicleNode ~= nil then
  local vehiclePreset =
  findPresetAtNode(vehicleNode)
  if vehiclePreset ~= nil then
    local score = 20
    if score > bestScore then
      bestScore = score
      bestPreset = vehiclePreset
      bestDescription =
      "Vehicle: "
      .. tostring(metadata.vehicle)
    end
  end
-- Configuration-specific override.
local configNode =
findChild(
  vehicleNode,
  metadata.configuration
  )
if configNode ~= nil then
  local configPreset =
  findPresetAtNode(configNode)
  if configPreset ~= nil then
    local score = 30
    if score > bestScore then
      bestScore = score
      bestPreset = configPreset
      bestDescription =
      "Configuration: "
      .. tostring(metadata.vehicle)
      .. "/"
      .. tostring(metadata.configuration)
    end
  end
end
end
end
end
-- --------------------------------------------------------------------------
-- Transmission branch
-- --------------------------------------------------------------------------
local transmissionSection =
findChild(game, "Transmission")
if transmissionSection ~= nil
  and metadata.transmission ~= nil then
    local transmissionNode =
    findChild(
      transmissionSection,
      metadata.transmission
      )
    if transmissionNode ~= nil then
      local preset =
      findPresetAtNode(transmissionNode)
      if preset ~= nil then
        local score = 15
        if score > bestScore then
          bestScore = score
          bestPreset = preset
          bestDescription =
          "Transmission: "
          .. tostring(metadata.transmission)
        end
      end
    end
  end
  if bestPreset == nil then
    return nil, "no matching profile"
  end
  return bestPreset, bestDescription
end
-- ============================================================================
-- Vehicle configuration application
-- ============================================================================
local function applyVehicleProfile(metadata)
  if metadata == nil then
    return
  end
  if not configLoaded then
    log("Configuration is not loaded; cannot resolve profile.")
    return
  end
  local preset, reason =
  resolveVehicleProfile(metadata)
  logSeparator("FFB PROFILE RESOLUTION")
  log(
    "Game: "
    .. GAME_NAME
    )
  log(
    "Vehicle type: "
    .. tostring(metadata.vehicleType)
    )
  log(
    "Vehicle: "
    .. tostring(metadata.vehicle)
    )
  log(
    "Configuration: "
    .. tostring(metadata.configuration)
    )
  log(
    "Transmission: "
    .. tostring(metadata.transmission)
    )
  log(
    "Match: "
    .. tostring(reason)
    )
  if preset == nil then
    log("No matching FFB preset.")
    currentProfile = nil
    return
  end
  log(
    "Selected FFB preset: "
    .. tostring(preset)
    )
  currentProfile = preset
  requestPreset(preset)
  logSeparator("END FFB PROFILE RESOLUTION")
end
-- ============================================================================
-- Vehicle metadata refresh
-- ============================================================================
local function refreshVehicleMetadata(vehicleId)
  if not isValidVehicleId(vehicleId) then
    return false
  end
  local vehicle =
  getVehicleById(vehicleId)
  if vehicle == nil then
    log(
      "Vehicle object not available yet for ID "
      .. tostring(vehicleId)
      )
    return false
  end
  local metadata =
  getMetadata(
    vehicle,
    vehicleId
    )
  if metadata == nil then
    return false
  end
  currentVehicle = vehicle
  currentIdentity = metadata
  logMetadata(metadata)
  applyVehicleProfile(metadata)
  return true
end
-- ============================================================================
-- Configuration reload scheduling
-- ============================================================================
local function scheduleConfigurationReload()
  configReloadPending = true
  configurationRetryTimer = 0
  configLoadAttempted = false
end
local function ensureConfigurationLoaded()
  if configLoaded then
    return true
  end
  return loadConfiguration()
end
local function processConfigurationReload(dtReal)
  if not configReloadPending then
    return
  end
  configurationRetryTimer =
  configurationRetryTimer + dtReal
  if configurationRetryTimer < CONFIG_RETRY_INTERVAL then
    return
  end
  configurationRetryTimer = 0
  if ensureConfigurationLoaded() then
    configReloadPending = false
    if currentVehicleId ~= nil then
      refreshVehicleMetadata(currentVehicleId)
    end
    return
  end
end
-- ============================================================================
-- Vehicle change handling
-- ============================================================================
local function handleVehicleChange(vehicleId, reason)
  if not isValidVehicleId(vehicleId) then
    return
  end
  if currentVehicleId == vehicleId
    and currentIdentity ~= nil then
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
    currentVehicle = nil
    currentIdentity = nil
    currentProfile = nil
-- Configuration.txt is intentionally reloaded whenever the vehicle changes.
scheduleConfigurationReload()
-- FFB ownership may have been affected by vehicle loading.
requestReacquire(false)
-- Metadata may not be available until the vehicle has completed spawning.
metadataRefreshPending = true
end
-- ============================================================================
-- Event callbacks
-- ============================================================================
local function onVehicleSwitched(oldId, newId)
  log(
    "onVehicleSwitched: "
    .. tostring(oldId)
    .. " -> "
    .. tostring(newId)
    )
  if isValidVehicleId(newId) then
    handleVehicleChange(
      newId,
      "onVehicleSwitched"
      )
  end
end
local function onVehicleSpawned(vehicleId)
  log(
    "onVehicleSpawned: "
    .. tostring(vehicleId)
    )
  local playerId =
  getPlayerVehicleId()
  if playerId ~= vehicleId then
    return
  end
  handleVehicleChange(
    vehicleId,
    "onVehicleSpawned"
    )
end
local function onPlayerVehicleChanged(vehicleId)
  log(
    "onPlayerVehicleChanged: "
    .. tostring(vehicleId)
    )
  if isValidVehicleId(vehicleId) then
    handleVehicleChange(
      vehicleId,
      "onPlayerVehicleChanged"
      )
  end
end
-- ============================================================================
-- Update loop
--
-- IMPORTANT:
-- Do NOT attach state to the onUpdate function itself.
--
-- The previous implementation did:
--
--   onUpdate._timer = ...
--
-- which caused:
--
--   attempt to index upvalue 'onUpdate' (a function value)
--
-- Keep the timer in vehiclePollTimer instead.
-- ============================================================================
local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end
  if udp == nil then
    initializeUDP()
  end
-- Configuration loading/retry.
processConfigurationReload(dtReal)
-- Lightweight fallback vehicle detector.
vehiclePollTimer =
vehiclePollTimer + dtReal
if vehiclePollTimer < VEHICLE_POLL_INTERVAL then
  return
end
vehiclePollTimer = 0
local playerId =
getPlayerVehicleId()
if isValidVehicleId(playerId) then
  if currentVehicleId ~= playerId then
    handleVehicleChange(
      playerId,
      "poll"
      )
  elseif metadataRefreshPending then
-- Try to obtain the vehicle metadata after spawning.
if refreshVehicleMetadata(playerId) then
  metadataRefreshPending = false
end
end
end
end
-- ============================================================================
-- Startup
-- ============================================================================
local function onExtensionLoaded()
  if initialized then
    return
  end
  initialized = true
  log("Extension initialized.")
  initializeUDP()
  scheduleConfigurationReload()
-- Don't treat -1 as a real vehicle.
--
-- Wait for a valid player vehicle ID.
core_jobsystem.create(function(job)
  local elapsed = 0
  while initialized and elapsed < 30 do
    local vehicleId =
    getPlayerVehicleId()
    if isValidVehicleId(vehicleId) then
      log(
        "Initial player vehicle detected: "
        .. tostring(vehicleId)
        )
      handleVehicleChange(
        vehicleId,
        "startup"
        )
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
-- ============================================================================
-- Shutdown
-- ============================================================================
local function onExtensionUnloaded()
  log("Extension unloading.")
  initialized = false
  currentVehicleId = nil
  currentVehicle = nil
  currentIdentity = nil
  currentProfile = nil
  configurationText = nil
  configurationPath = nil
  configLoaded = false
  configLoadAttempted = false
  configReloadPending = false
  metadataRefreshPending = false
  vehiclePollTimer = 0
  configurationRetryTimer = 0
  if udp ~= nil then
    pcall(function()
      udp:close()
    end)
  end
  udp = nil
  socket = nil
end
-- ============================================================================
-- Optional diagnostic commands
--
-- These are intentionally exported so they can be called from the BeamNG
-- console while debugging:
--
--   extensions.multiffbjoy.dumpVehicleMetadata()
--   extensions.multiffbjoy.reloadConfiguration()
-- ============================================================================
local function dumpVehicleMetadata()
  local vehicleId =
  getPlayerVehicleId()
  if not isValidVehicleId(vehicleId) then
    log("No valid player vehicle.")
    return
  end
  local vehicle =
  getVehicleById(vehicleId)
  if vehicle == nil then
    log("Player vehicle object unavailable.")
    return
  end
  local metadata =
  getMetadata(
    vehicle,
    vehicleId
    )
  logMetadata(metadata)
  if configLoaded then
    applyVehicleProfile(metadata)
  end
end
local function reloadConfiguration()
  log("Manual Configuration.txt reload requested.")
  configLoaded = false
  configurationText = nil
  configurationPath = nil
  if loadConfiguration() then
    configReloadPending = false
    if isValidVehicleId(currentVehicleId) then
      refreshVehicleMetadata(currentVehicleId)
    end
  else
    configReloadPending = true
  end
end
-- ============================================================================
-- Public extension API
-- ============================================================================
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleSpawned = onVehicleSpawned
M.onPlayerVehicleChanged = onPlayerVehicleChanged
M.dumpVehicleMetadata = dumpVehicleMetadata
M.reloadConfiguration = reloadConfiguration
return M