local M = {}
local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local initialized = false
local currentVehicleId = nil
local currentVehicleSignature = nil
local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0
local configuration = nil
local configurationPath = nil
local configurationLastHash = nil
local configurationSearchAttempts = 0
local CONFIGURATION_SEARCH_MAX_ATTEMPTS = 20
local GAME_NAME = "BeamNG.drive"
-- ============================================================================
-- Logging
-- ============================================================================
local function log(message)
  print("[MultiFFBJoy] " .. tostring(message))
end
local function logSeparator()
  log("------------------------------------------------------------")
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
-- ============================================================================
-- Utility
-- ============================================================================
local function trim(value)
  if value == nil then
    return ""
  end
  value = tostring(value)
  value = value:gsub("^%s+", "")
  value = value:gsub("%s+$", "")
  return value
end
local function normalizeKey(value)
  value = trim(value)
  if value == "" then
    return ""
  end
-- Quoted keys are intentionally language/display-name keys.
-- Keep their contents but normalize case for comparison.
if value:sub(1, 1) == '"' and value:sub(-1) == '"' then
  value = value:sub(2, -2)
end
return value:lower()
end
local function stripQuotes(value)
  value = trim(value)
  if value:sub(1, 1) == '"' and value:sub(-1) == '"' then
    return value:sub(2, -2)
  end
  return value
end
local function valueToString(value)
  if value == nil then
    return "nil"
  end
  if type(value) == "boolean" then
    return value and "true" or "false"
  end
  if type(value) == "table" then
    return "<table>"
  end
  return tostring(value)
end
local function safeGetField(object, field)
  if object == nil then
    return nil
  end
  local ok, result = pcall(function()
    return object:getField(field, "")
  end)
  if not ok then
    return nil
  end
  if result == nil or result == "" then
    return nil
  end
  return result
end
-- ============================================================================
-- Configuration file
-- ============================================================================
local function findConfigurationFile()
  if FS == nil then
    log("FS API is unavailable.")
    return nil
  end
  local recursiveLevels = FS.MAX_LEVELS or -1
  local ok, files = pcall(function()
    return FS:findFiles(
      "/",
      "Configuration.txt",
      recursiveLevels,
      true,
      false
      )
  end)
  if not ok or type(files) ~= "table" then
    log(
      "FS:findFiles(Configuration.txt) failed: "
      .. tostring(files)
      )
    return nil
  end
  local preferred = nil
  local fallback = nil
  for _, path in ipairs(files) do
    if type(path) == "string" then
      fallback = fallback or path
      local lower = path:lower()
      if lower:find("multiffbjoy", 1, true) then
        preferred = path
        break
      end
    end
  end
  return preferred or fallback
end
local function getConfigurationFileContents(path)
  if path == nil then
    return nil
  end
  local ok, contents = pcall(function()
    return readFile(path)
  end)
  if not ok then
    log(
      "readFile failed for "
      .. tostring(path)
      .. ": "
      .. tostring(contents)
      )
    return nil
  end
  if contents == nil then
    return nil
  end
  return contents
end
local function calculateSimpleHash(text)
  if text == nil then
    return nil
  end
  local hash = 0
  for i = 1, #text do
    hash = (hash * 31 + string.byte(text, i)) % 2147483647
  end
  return hash
end
-- Parse a tab-indented tree.
--
-- Each line is:
--
--     Node
--         Child
--             Grandchild=value
--
-- Both tabs and groups of four spaces are accepted as indentation.
--
-- The result is a node:
--
-- {
--   name = "...",
--   value = "...",
--   children = { ... }
-- }
--
local function parseConfigurationTree(contents)
  if contents == nil then
    return nil
  end
  local root = {
    name = "<root>",
    value = nil,
    children = {}
  }
  local stack = {
    {
      indent = -1,
      node = root
    }
  }
  for rawLine in contents:gmatch("[^\r\n]+") do
    local line = rawLine:gsub("%s+$", "")
    if trim(line) ~= "" then
      local prefix = line:match("^[\t ]*") or ""
      local indent = 0
      for i = 1, #prefix do
        local c = prefix:sub(i, i)
        if c == "\t" then
          indent = indent + 1
        elseif c == " " then
-- Four spaces = one indentation level.
-- Individual spaces are accumulated below.
end
end
if not prefix:find("\t", 1, true) then
  indent = math.floor(#prefix / 4)
end
local content = trim(line:sub(#prefix + 1))
-- Ignore comments.
if not content:match("^#")
  and not content:match("^//")
  then
    local name
    local value
    local equalsPosition = content:find("=", 1, true)
    if equalsPosition ~= nil then
      name = trim(content:sub(1, equalsPosition - 1))
      value = trim(content:sub(equalsPosition + 1))
    else
      name = trim(content)
      value = nil
    end
    if name ~= "" then
      local node = {
        name = name,
        value = value,
        children = {}
      }
      while #stack > 0
        and indent <= stack[#stack].indent
        do
          table.remove(stack)
        end
        local parent = stack[#stack]
        if parent == nil then
          parent = {
            indent = -1,
            node = root
          }
        end
        table.insert(parent.node.children, node)
        table.insert(
          stack,
          {
            indent = indent,
            node = node
          }
          )
      end
    end
  end
end
return root
end
local function findChild(node, name)
  if node == nil
    or type(node.children) ~= "table"
    then
      return nil
    end
    local wanted = normalizeKey(name)
    for _, child in ipairs(node.children) do
      if normalizeKey(child.name) == wanted then
        return child
      end
    end
    return nil
  end
  local function findChildValue(node, name)
    local child = findChild(node, name)
    if child == nil then
      return nil
    end
    return child.value
  end
  local function reloadConfiguration()
    local path = findConfigurationFile()
    if path == nil then
      configurationSearchAttempts =
      configurationSearchAttempts + 1
      if configurationSearchAttempts <=
        CONFIGURATION_SEARCH_MAX_ATTEMPTS
        then
          log(
            "Configuration.txt not found "
            .. "("
            .. tostring(configurationSearchAttempts)
            .. "/"
            .. tostring(CONFIGURATION_SEARCH_MAX_ATTEMPTS)
            .. ")."
            )
        end
        return false
      end
      local contents = getConfigurationFileContents(path)
      if contents == nil then
        configurationSearchAttempts =
        configurationSearchAttempts + 1
        log(
          "Configuration.txt found but could not be read: "
          .. tostring(path)
          )
        return false
      end
      local hash = calculateSimpleHash(contents)
      if configuration ~= nil
        and configurationPath == path
        and configurationLastHash == hash
        then
          return true
        end
        local tree = parseConfigurationTree(contents)
        if tree == nil then
          log("Configuration parser returned nil.")
          return false
        end
        configuration = tree
        configurationPath = path
        configurationLastHash = hash
        configurationSearchAttempts = 0
        log(
          "Configuration loaded: "
          .. tostring(path)
          )
        return true
      end
-- ============================================================================
-- Vehicle metadata
-- ============================================================================
local function getCurrentVehicleId()
  if be == nil then
    return nil
  end
  local ok, result = pcall(function()
    return be:getPlayerVehicleID(0)
  end)
  if not ok then
    return nil
  end
  if result == nil or result < 0 then
    return nil
  end
  return result
end
local function getVehicleData(vehicleId)
  if core_vehicle_manager == nil then
    return nil
  end
  local ok, result = pcall(function()
    return core_vehicle_manager.getVehicleData(vehicleId)
  end)
  if not ok then
    return nil
  end
  return result
end
local function getPlayerVehicleData()
  if core_vehicle_manager == nil then
    return nil
  end
  local ok, result = pcall(function()
    return core_vehicle_manager.getPlayerVehicleData()
  end)
  if not ok then
    return nil
  end
  return result
end
local function parsePartConfigFilename(partConfigFilename)
  if partConfigFilename == nil then
    return nil, nil
  end
  local value = tostring(partConfigFilename)
  value = value:gsub("\\", "/")
  local vehicleCode =
  value:match("vehicles/([^/]+)/")
  local configCode =
  value:match("vehicles/[^/]+/([^/]+)%.pc$")
  return vehicleCode, configCode
end
local function mapTransmissionType(value)
  if value == nil then
    return nil
  end
  local text = tostring(value):lower()
  if text:find("automatic", 1, true) then
    return "Automatic"
  end
  if text:find("manual", 1, true) then
    return "Manual"
  end
  if text:find("dct", 1, true) then
    return "DCT"
  end
  if text:find("cvt", 1, true) then
    return "CVT"
  end
  if text:find("sequential", 1, true) then
    return "Sequential"
  end
  return tostring(value)
end
local function getVehicleMetadata(vehicleId)
  local result = {
    id = vehicleId,
    vehicleCode = nil,
    configurationCode = nil,
    partConfigFilename = nil,
    vehicleType = nil,
    transmission = nil,
    shiftLogicName = nil,
    automaticModes = nil,
    gear = nil,
    gearIndex = nil,
    gearA = nil,
    isShifting = nil
  }
  local object = nil
  pcall(function()
    object = be:getObjectByID(vehicleId)
  end)
  local managerData = getVehicleData(vehicleId)
  local playerData = getPlayerVehicleData()
  local vdata = nil
  local config = nil
  if managerData ~= nil then
    vdata = managerData.vdata
    config = managerData.config
  elseif playerData ~= nil then
    vdata = playerData.vdata
    config = playerData.config
  end
-- -------------------------------------------------------------------------
-- Internal vehicle codename
-- -------------------------------------------------------------------------
if object ~= nil then
  result.vehicleCode =
  safeGetField(object, "JBeam")
  or safeGetField(object, "jBeam")
end
if result.vehicleCode == nil
  and vdata ~= nil
  then
    result.vehicleCode =
    vdata.model
    or vdata.modelName
    or vdata.mainPartName
  end
-- -------------------------------------------------------------------------
-- Part configuration filename
-- -------------------------------------------------------------------------
if config ~= nil then
  result.partConfigFilename =
  config.partConfigFilename
  or config.partConfig
  or config.filename
end
if result.partConfigFilename == nil
  and object ~= nil
  then
    result.partConfigFilename =
    safeGetField(object, "partConfig")
  end
  local parsedVehicleCode
  local parsedConfigCode
  parsedVehicleCode, parsedConfigCode =
  parsePartConfigFilename(
    result.partConfigFilename
    )
  if parsedVehicleCode ~= nil then
    result.vehicleCode = parsedVehicleCode
  end
  result.configurationCode =
  parsedConfigCode
-- -------------------------------------------------------------------------
-- Vehicle type
-- -------------------------------------------------------------------------
-- Try vehicle manager vdata first.
if vdata ~= nil then
  result.vehicleType =
  vdata.type
  or vdata.vehicleType
  or vdata.category
  or vdata.Type
  or vdata.Category
end
-- Try current vehicle details/model data.
if result.vehicleType == nil
  and core_vehicles ~= nil
  then
    local okDetails, details =
    pcall(function()
      return core_vehicles.getCurrentVehicleDetails()
    end)
    if okDetails and type(details) == "table" then
      local modelKey =
      details.model_key
      or details.model
      or result.vehicleCode
      if modelKey ~= nil then
        local okModel, modelData =
        pcall(function()
          return core_vehicles.getModel(modelKey)
        end)
        if okModel
          and type(modelData) == "table"
          then
            result.vehicleType =
            modelData.type
            or modelData.Type
            or modelData.vehicleType
            or modelData.category
            or modelData.Category
          end
        end
      end
    end
-- -------------------------------------------------------------------------
-- Vehicle controller / transmission
-- -------------------------------------------------------------------------
local vehicleController = nil
if vdata ~= nil then
  vehicleController =
  vdata.vehicleController
  or vdata.controller
end
if vehicleController ~= nil then
  result.shiftLogicName =
  vehicleController.shiftLogicName
  result.automaticModes =
  vehicleController.automaticModes
end
-- The vehicle controller's electrics are the documented runtime
-- transmission state.
--
-- These may be available in either the manager data or the global
-- electrics table depending on the current vehicle/state.
local electricsValues = nil
if vdata ~= nil then
  electricsValues =
  vdata.electrics
  or vdata.electricsValues
end
if electricsValues ~= nil then
  result.gear =
  electricsValues.gear
  result.gearIndex =
  electricsValues.gearIndex
  result.gearA =
  electricsValues.gear_A
  result.isShifting =
  electricsValues.isShifting
end
-- Try the actual player's electrics table as a fallback.
if electrics ~= nil
  and type(electrics.values) == "table"
  then
    local values = electrics.values
    result.gear =
    result.gear
    or values.gear
    result.gearIndex =
    result.gearIndex
    or values.gearIndex
    result.gearA =
    result.gearA
    or values.gear_A
    if result.isShifting == nil then
      result.isShifting =
      values.isShifting
    end
  end
-- Configuration metadata can contain the generated transmission
-- classification. This is useful as a fallback.
if config ~= nil then
  local configTransmission =
  config.Transmission
  or config.transmission
  if configTransmission ~= nil then
    result.transmission =
    mapTransmissionType(
      configTransmission
      )
  end
end
if result.transmission == nil
  and result.shiftLogicName ~= nil
  then
    result.transmission =
    mapTransmissionType(
      result.shiftLogicName
      )
  end
  return result
end
-- ============================================================================
-- Diagnostics
-- ============================================================================
local function dumpVehicleMetadata(metadata)
  logSeparator()
  log("VEHICLE METADATA")
  logSeparator()
  log("Vehicle ID = " .. valueToString(metadata.id))
  log("Internal vehicle code = "
    .. valueToString(metadata.vehicleCode))
  log("Part config filename = "
    .. valueToString(metadata.partConfigFilename))
  log("Configuration code = "
    .. valueToString(metadata.configurationCode))
  log("Vehicle type = "
    .. valueToString(metadata.vehicleType))
  log("Transmission = "
    .. valueToString(metadata.transmission))
  log("Shift logic name = "
    .. valueToString(metadata.shiftLogicName))
  log("Automatic modes = "
    .. valueToString(metadata.automaticModes))
  log("Current gear = "
    .. valueToString(metadata.gear))
  log("Gear index = "
    .. valueToString(metadata.gearIndex))
  log("gear_A = "
    .. valueToString(metadata.gearA))
  log("isShifting = "
    .. valueToString(metadata.isShifting))
  logSeparator()
end
-- ============================================================================
-- Configuration matching
-- ============================================================================
local function findPresetInNode(node, key)
  if node == nil or key == nil then
    return nil
  end
  local child = findChild(node, key)
  if child ~= nil
    and child.value ~= nil
    and trim(child.value) ~= ""
    then
      return stripQuotes(child.value)
    end
    return nil
  end
  local function getGameNode()
    if configuration == nil then
      return nil
    end
    local profiles =
    findChild(configuration, "Profiles")
    if profiles == nil then
      return nil
    end
    return findChild(
      profiles,
      GAME_NAME
      )
  end
  local function lookupVehiclePreset(metadata)
    local gameNode = getGameNode()
    if gameNode == nil then
      return nil, "game not found"
    end
    local vehicleRoot =
    findChild(
      gameNode,
      "Vehicle"
      )
    if vehicleRoot == nil then
      return nil, "Vehicle category not found"
    end
    local vehicleType =
    metadata.vehicleType
    if vehicleType == nil then
      return nil, "vehicle type unavailable"
    end
    local typeNode =
    findChild(
      vehicleRoot,
      vehicleType
      )
    if typeNode == nil then
      return nil,
      "vehicle type not found: "
      .. tostring(vehicleType)
    end
    local vehicleCode =
    metadata.vehicleCode
    if vehicleCode == nil then
      return nil, "vehicle code unavailable"
    end
    local vehicleNode =
    findChild(
      typeNode,
      vehicleCode
      )
-- -------------------------------------------------------------------------
-- Most specific: vehicle + configuration
-- -------------------------------------------------------------------------
if vehicleNode ~= nil
  and metadata.configurationCode ~= nil
  then
    local preset =
    findPresetInNode(
      vehicleNode,
      metadata.configurationCode
      )
    if preset ~= nil then
      return preset,
      "vehicle configuration override"
    end
  end
-- -------------------------------------------------------------------------
-- Vehicle-specific default
-- -------------------------------------------------------------------------
if vehicleNode ~= nil
  and vehicleNode.value ~= nil
  then
    local preset =
    stripQuotes(vehicleNode.value)
    if preset ~= "" then
      return preset,
      "vehicle override"
    end
  end
-- -------------------------------------------------------------------------
-- Vehicle category default
-- -------------------------------------------------------------------------
if typeNode.value ~= nil then
  local preset =
  stripQuotes(typeNode.value)
  if preset ~= "" then
    return preset,
    "vehicle type default"
  end
end
return nil,
"no vehicle-specific match"
end
local function lookupTransmissionPreset(metadata)
  local gameNode = getGameNode()
  if gameNode == nil then
    return nil, "game not found"
  end
  local transmissionRoot =
  findChild(
    gameNode,
    "Transmission"
    )
  if transmissionRoot == nil then
    return nil,
    "Transmission category not found"
  end
  if metadata.transmission == nil then
    return nil,
    "transmission unavailable"
  end
  local preset =
  findPresetInNode(
    transmissionRoot,
    metadata.transmission
    )
  if preset ~= nil then
    return preset,
    "transmission default"
  end
  return nil,
  "transmission not found"
end
local function lookupPreset(metadata)
  local preset
  local reason
  preset, reason =
  lookupVehiclePreset(metadata)
  if preset ~= nil then
    return preset, reason
  end
  preset, reason =
  lookupTransmissionPreset(metadata)
  if preset ~= nil then
    return preset, reason
  end
  return nil,
  reason or "no matching profile"
end
-- ============================================================================
-- Profile activation
-- ============================================================================
local function normalizePresetName(name)
  if name == nil then
    return nil
  end
  name = stripQuotes(name)
  name = trim(name)
  if name == "" then
    return nil
  end
  if not name:lower():match("%.fff$") then
    name = name .. ".fff"
  end
  return name
end
local function applyProfile(metadata)
  if not reloadConfiguration() then
    log("Cannot select FFB profile: Configuration.txt unavailable.")
    return
  end
  local preset, reason =
  lookupPreset(metadata)
  log("Vehicle profile lookup:")
  log("  Game: " .. GAME_NAME)
  log("  VehicleType: "
    .. valueToString(metadata.vehicleType))
  log("  Vehicle: "
    .. valueToString(metadata.vehicleCode))
  log("  Configuration: "
    .. valueToString(metadata.configurationCode))
  log("  Transmission: "
    .. valueToString(metadata.transmission))
  log("  ShiftLogic: "
    .. valueToString(metadata.shiftLogicName))
  if preset == nil then
    log(
      "  Match: "
      .. tostring(reason)
      )
    log("  No matching FFB preset.")
-- No profile means stop any previous profile rather than
-- leaving the previous vehicle's force active.
sendCommand("STOP")
return
end
preset =
normalizePresetName(preset)
log(
  "  Match: "
  .. tostring(reason)
  )
log(
  "  Preset: "
  .. tostring(preset)
  )
sendCommand(
  "PROFILE "
  .. preset
  )
end
-- ============================================================================
-- Vehicle changes
-- ============================================================================
local function handleVehicleChange(vehicleId)
  if vehicleId == nil
    or vehicleId < 0
    then
      log("No active player vehicle.")
      currentVehicleId = nil
      currentVehicleSignature = nil
      sendCommand("STOP")
      return
    end
    log(
      "Player vehicle changed: "
      .. valueToString(currentVehicleId)
      .. " -> "
      .. valueToString(vehicleId)
      )
    currentVehicleId = vehicleId
    requestReacquire()
-- Vehicle loading can complete asynchronously after
-- onVehicleSwitched(), so wait for valid vehicle-manager data.
core_jobsystem.create(function(job)
  local elapsed = 0
  while initialized and elapsed < 10 do
    local metadata =
    getVehicleMetadata(vehicleId)
    if metadata ~= nil
      and metadata.vehicleCode ~= nil
      and metadata.configurationCode ~= nil
      then
        local signature =
        table.concat(
        {
          tostring(vehicleId),
          tostring(metadata.vehicleCode),
          tostring(metadata.configurationCode),
          tostring(metadata.vehicleType),
          tostring(metadata.transmission),
          tostring(metadata.shiftLogicName)
        },
        "|"
        )
        if signature ~= currentVehicleSignature then
          currentVehicleSignature = signature
          dumpVehicleMetadata(metadata)
          applyProfile(metadata)
        end
        return
      end
      job.sleep(0.25)
      elapsed = elapsed + 0.25
    end
    if initialized then
      log(
        "Vehicle metadata was not ready within "
        .. "10 seconds for vehicle "
        .. tostring(vehicleId)
        )
-- Print whatever we can find at the end.
local metadata =
getVehicleMetadata(vehicleId)
if metadata ~= nil then
  dumpVehicleMetadata(metadata)
  applyProfile(metadata)
end
end
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
  getCurrentVehicleId()
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
-- ============================================================================
-- Update
-- ============================================================================
local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end
  if udp == nil then
    initializeUDP()
  end
-- The active vehicle's configuration can change without changing
-- the vehicle ID. For example, the player can replace a vehicle
-- configuration or respawn it.
--
-- Check this at a low frequency rather than every frame.
--
-- 0.5 seconds is more than sufficient for profile switching.
--
onUpdate._timer =
(onUpdate._timer or 0) + dtReal
if onUpdate._timer < 0.5 then
  return
end
onUpdate._timer = 0
local vehicleId =
getCurrentVehicleId()
if vehicleId ~= nil
  and vehicleId ~= currentVehicleId
  then
    handleVehicleChange(vehicleId)
    return
  end
  if vehicleId ~= nil then
    local metadata =
    getVehicleMetadata(vehicleId)
    if metadata ~= nil
      and metadata.vehicleCode ~= nil
      and metadata.configurationCode ~= nil
      then
        local signature =
        table.concat(
        {
          tostring(vehicleId),
          tostring(metadata.vehicleCode),
          tostring(metadata.configurationCode),
          tostring(metadata.vehicleType),
          tostring(metadata.transmission),
          tostring(metadata.shiftLogicName)
        },
        "|"
        )
        if currentVehicleSignature ~= nil
          and signature ~= currentVehicleSignature
          then
            log(
              "Vehicle metadata changed without "
              .. "vehicle ID changing."
              )
            currentVehicleSignature = signature
            dumpVehicleMetadata(metadata)
            applyProfile(metadata)
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
  reloadConfiguration()
  core_jobsystem.create(function(job)
    local elapsed = 0
    while initialized and elapsed < 30 do
      local vehicleId =
      getCurrentVehicleId()
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
-- ============================================================================
-- Shutdown
-- ============================================================================
local function onExtensionUnloaded()
  log("Extension unloading.")
  initialized = false
  currentVehicleId = nil
  currentVehicleSignature = nil
  configuration = nil
  configurationPath = nil
  configurationLastHash = nil
  if udp ~= nil then
    pcall(function()
      udp:close()
    end)
  end
  udp = nil
  socket = nil
end
-- ============================================================================
-- Export extension API
-- ============================================================================
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleSpawned = onVehicleSpawned
M.onPlayerVehicleChanged = onPlayerVehicleChanged
return M