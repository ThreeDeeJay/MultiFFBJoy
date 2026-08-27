local M = {}
local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458
local initialized = false
local currentVehicleId = nil
local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0
local diagnosticRequestId = 0
local lastDiagnosticRequest = -1
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
-- ============================================================================
-- Utility functions
-- ============================================================================
local function safeToString(value)
  local ok, result = pcall(function()
    return tostring(value)
  end)
  if ok then
    return result
  end
  return "<tostring failed>"
end
local function safeGetField(object, fieldName)
  if object == nil then
    return nil
  end
  local ok, result = pcall(function()
    return object:getField(fieldName, "")
  end)
  if not ok then
    return nil
  end
  if result == "" then
    return nil
  end
  return result
end
local function extractVehicleCodename(vehicle)
  if vehicle == nil then
    return nil
  end
  local value = nil
  local ok, result = pcall(function()
    return vehicle.JBeam
  end)
  if ok and result ~= nil and result ~= "" then
    value = result
  end
  if value == nil then
    value = safeGetField(vehicle, "JBeam")
  end
  if value == nil then
    local methodOK, methodResult = pcall(function()
      return vehicle:getJBeamFilename()
    end)
    if methodOK and methodResult ~= nil then
      value = methodResult
    end
  end
  return value
end
local function extractPartConfig(vehicle)
  if vehicle == nil then
    return nil
  end
  local value = nil
  local ok, result = pcall(function()
    return vehicle.partConfig
  end)
  if ok and result ~= nil and result ~= "" then
    value = result
  end
  if value == nil then
    value = safeGetField(vehicle, "partConfig")
  end
  return value
end
local function extractConfigurationCodename(partConfig)
  if partConfig == nil then
    return nil
  end
  local normalized = tostring(partConfig)
  normalized = normalized:gsub("\\", "/")
  local filename = normalized:match("([^/]+)%.pc$")
  if filename == nil then
    return nil
  end
  return filename
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
  if not ok then
    log(
      "getObjectByID failed: "
      .. tostring(vehicle)
      )
    return nil, vehicleId
  end
  return vehicle, vehicleId
end
-- ============================================================================
-- GELUA-side vehicle metadata
-- ============================================================================
local function printDirectVehicleMetadata(vehicle, vehicleId)
  log("========================================")
  log("VEHICLE METADATA DIAGNOSTIC")
  log("========================================")
  log("Vehicle ID = " .. tostring(vehicleId))
  if vehicle == nil then
    log("Vehicle object = nil")
    log("========================================")
    return
  end
  log("Vehicle object = " .. safeToString(vehicle))
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
    local ok, value = pcall(function()
      return vehicle[fieldName]
    end)
    if ok then
      log(
        "vehicle."
        .. fieldName
        .. " = "
        .. safeToString(value)
        )
    else
      log(
        "vehicle."
        .. fieldName
        .. " = <error>"
        )
    end
  end
  log("")
  log("getField() VALUES")
  log("-----------------")
  for _, fieldName in ipairs(directFields) do
    local value = safeGetField(vehicle, fieldName)
    if value == nil then
      value = ""
    end
    log(
      "getField("
      .. fieldName
      .. ") = "
      .. tostring(value)
      )
  end
  log("")
  log("METHODS")
  log("-------")
  local methodNames = {
    "getJBeamFilename",
    "getID",
    "getName"
  }
  for _, methodName in ipairs(methodNames) do
    local ok, value = pcall(function()
      local fn = vehicle[methodName]
      if type(fn) ~= "function" then
        return "<not available>"
      end
      return fn(vehicle)
    end)
    if ok then
      log(
        methodName
        .. "() = "
        .. safeToString(value)
        )
    else
      log(
        methodName
        .. "() = <error>"
        )
    end
  end
  local jbeam = extractVehicleCodename(vehicle)
  local partConfig = extractPartConfig(vehicle)
  local configuration = extractConfigurationCodename(partConfig)
  log("")
  log("IDENTITY")
  log("--------")
  log(
    "Vehicle codename = "
    .. tostring(jbeam)
    )
  log(
    "Part config = "
    .. tostring(partConfig)
    )
  log(
    "Configuration codename = "
    .. tostring(configuration)
    )
  log("")
  log("PART CONFIG PARSING")
  log("-------------------")
  if partConfig ~= nil then
    local normalized = tostring(partConfig):gsub("\\", "/")
    local parsedVehicle =
    normalized:match("vehicles/([^/]+)/")
    local parsedConfig =
    normalized:match("vehicles/[^/]+/([^/]+)%.pc$")
    log(
      "partConfig raw = "
      .. tostring(partConfig)
      )
    log(
      "Parsed vehicle codename = "
      .. tostring(parsedVehicle)
      )
    log(
      "Parsed configuration codename = "
      .. tostring(parsedConfig)
      )
  else
    log("partConfig unavailable.")
  end
  log("")
  log("OBJECT ID")
  log("---------")
  local ok, objectId = pcall(function()
    return vehicle:getID()
  end)
  if ok then
    log(
      "vehicle:getID() = "
      .. tostring(objectId)
      )
  else
    log("vehicle:getID() failed.")
  end
  log("========================================")
end
-- ============================================================================
-- VLUA diagnostic bridge
--
-- The GELUA vehicle object does not expose the complete vehicle-controller
-- runtime state. BeamNG provides queueLuaCommand() specifically for sending
-- code into the vehicle's VLUA.
--
-- The vehicle VM sends the diagnostic table back through a mailbox.
-- ============================================================================
local function queueVehicleDiagnostic(vehicle, vehicleId)
  if vehicle == nil then
    log("Cannot run VLUA diagnostic: vehicle is nil.")
    return
  end
  diagnosticRequestId =
  diagnosticRequestId + 1
  local requestId = diagnosticRequestId
  log("")
  log("Requesting VLUA vehicle-controller diagnostic...")
  log("Diagnostic request ID = " .. tostring(requestId))
  local command = [[
  local result = {
    requestId = ]] .. tostring(requestId) .. [[,
    vehicleId = nil,
    jbeam = nil,
    electrics = {},
    gearbox = {},
    automatic = {},
    controller = {},
    powertrain = {},
    globals = {}
  }
  local function addValue(target, name, value)
  local ok, text = pcall(function()
  return tostring(value)
  end)
  if ok then
  target[name] = text
  else
  target[name] = "<tostring failed>"
  end
  end
  local function inspectTable(target, source, names)
  if type(source) ~= "table" then
  return
  end
  for _, name in ipairs(names) do
  local ok, value = pcall(function()
  return source[name]
  end)
  if ok and value ~= nil then
  addValue(target, name, value)
  end
  end
  end
  local function shallowInspect(target, source, prefix, depth, visited)
  if type(source) ~= "table" then
  return
  end
  if depth <= 0 then
  return
  end
  visited = visited or {}
  if visited[source] then
  return
  end
  visited[source] = true
  local count = 0
  for key, value in pairs(source) do
  count = count + 1
  if count > 100 then
  break
  end
  local keyText = tostring(key)
  if type(value) ~= "table" then
  addValue(
  target,
  prefix .. keyText,
  value
  )
  end
  end
  end
  -- Basic vehicle identity inside VLUA.
  pcall(function()
  result.vehicleId = obj:getID()
  end)
  pcall(function()
  result.jbeam = obj:getJBeamFilename()
  end)
  -- --------------------------------------------------------------------------
  -- Electrics
  -- --------------------------------------------------------------------------
  if type(electrics) == "table" then
  if type(electrics.values) == "table" then
  local interestingElectrics = {
    "gear",
    "gearIndex",
    "gear_A",
    "gear_M",
    "gear_R",
    "gear_D",
    "automaticGear",
    "automaticMode",
    "automaticGearIndex",
    "wheelspeed",
    "airspeed",
    "rpm",
    "throttle",
    "brake",
    "clutch",
    "steering",
    "parkingbrake",
    "engineRunning"
  }
  inspectTable(
  result.electrics,
  electrics.values,
  interestingElectrics
  )
  end
  end
  -- --------------------------------------------------------------------------
  -- Search for gearbox / transmission objects
  -- --------------------------------------------------------------------------
  local gearboxCandidates = {
    "gearbox",
    "automaticGearbox",
    "manualGearbox",
    "transmission",
    "powertrain",
    "vehicleController"
  }
  for _, name in ipairs(gearboxCandidates) do
  local ok, value = pcall(function()
  return _G[name]
  end)
  if ok and value ~= nil then
  addValue(
  result.globals,
  name,
  value
  )
  end
  end
  -- --------------------------------------------------------------------------
  -- Known vehicle-controller / gearbox variables
  -- --------------------------------------------------------------------------
  local controllerNames = {
    "controller",
    "vehicleController",
    "mainController",
    "automaticHandling",
    "gearboxHandling",
    "shiftLogic",
    "shiftLogicName"
  }
  for _, name in ipairs(controllerNames) do
  local ok, value = pcall(function()
  return _G[name]
  end)
  if ok and value ~= nil then
  if type(value) == "table" then
  shallowInspect(
  result.controller,
  value,
  name .. ".",
  2
  )
  else
  addValue(
  result.controller,
  name,
  value
  )
  end
  end
  end
  -- --------------------------------------------------------------------------
  -- Inspect likely automatic gearbox runtime variables.
  --
  -- The official gearbox controller documents these concepts:
  -- automaticModes
  -- currentGearIndex
  -- getGearName()
  -- getGearPosition()
  -- etc.
  -- --------------------------------------------------------------------------
  local automaticCandidates = {
    "automaticModes",
    "availableModes",
    "automaticHandling",
    "currentGearIndex",
    "currentGear",
    "gearPosition",
    "mode",
    "automaticMode",
    "defaultAutomaticMode",
    "defaultAutomaticForwardMode",
    "shiftLogicName"
  }
  for _, name in ipairs(automaticCandidates) do
  local found = false
  local ok, value = pcall(function()
  return _G[name]
  end)
  if ok and value ~= nil then
  addValue(
  result.automatic,
  name,
  value
  )
  found = true
  end
  if not found and type(controller) == "table" then
  local ok2, value2 = pcall(function()
  return controller[name]
  end)
  if ok2 and value2 ~= nil then
  addValue(
  result.automatic,
  "controller." .. name,
  value2
  )
  end
  end
  end
  -- --------------------------------------------------------------------------
  -- Powertrain inspection
  -- --------------------------------------------------------------------------
  if type(powertrain) == "table" then
  shallowInspect(
  result.powertrain,
  powertrain,
  "powertrain.",
  2
  )
  end
  -- --------------------------------------------------------------------------
  -- Search globals for names which look especially relevant.
  -- --------------------------------------------------------------------------
  local interestingGlobalWords = {
    "gear",
    "shift",
    "transmission",
    "automatic",
    "vehicle",
    "controller",
    "powertrain",
    "config",
    "jbeam",
    "type"
  }
  local globalCount = 0
  for name, value in pairs(_G) do
  if globalCount >= 300 then
  break
  end
  local nameText = tostring(name):lower()
  local interesting = false
  for _, word in ipairs(interestingGlobalWords) do
  if nameText:find(word, 1, true) then
  interesting = true
  break
  end
  end
  if interesting then
  globalCount = globalCount + 1
  addValue(
  result.globals,
  tostring(name),
  value
  )
  end
  end
  -- --------------------------------------------------------------------------
  -- Send the complete result back to GELUA.
  -- --------------------------------------------------------------------------
  pcall(function()
  be:sendToMailbox(
  "multiffbjoy.vehicleDiagnostic",
  result
  )
  end)
  ]]
  local ok, err = pcall(function()
    vehicle:queueLuaCommand(command)
  end)
  if not ok then
    log(
      "queueLuaCommand failed: "
      .. tostring(err)
      )
    return false
  end
  log("VLUA diagnostic queued successfully.")
  return true
end
-- ============================================================================
-- Print VLUA diagnostic result
-- ============================================================================
local function printDiagnosticTable(title, tableValue)
  log("")
  log(title)
  log(string.rep("-", #title))
  if type(tableValue) ~= "table" then
    log("<not a table>")
    return
  end
  local keys = {}
  for key, _ in pairs(tableValue) do
    table.insert(keys, tostring(key))
  end
  table.sort(keys)
  for _, key in ipairs(keys) do
    local value = tableValue[key]
    log(
      tostring(key)
      .. " = "
      .. safeToString(value)
      )
  end
end
local function processVehicleDiagnostic()
  if be == nil then
    return
  end
  local vehicle = nil
  local ok, result = pcall(function()
    return be:getLastMailbox(
      "multiffbjoy.vehicleDiagnostic"
      )
  end)
  if not ok or result == nil then
    return
  end
  if type(result) ~= "table" then
    return
  end
  local requestId = result.requestId
  if requestId == nil then
    return
  end
  if requestId == lastDiagnosticRequest then
    return
  end
  lastDiagnosticRequest = requestId
  log("")
  log("========================================")
  log("VLUA VEHICLE DIAGNOSTIC RESULT")
  log("========================================")
  log(
    "Vehicle ID = "
    .. tostring(result.vehicleId)
    )
  log(
    "VLUA JBeam = "
    .. tostring(result.jbeam)
    )
  printDiagnosticTable(
    "ELECTRICS",
    result.electrics
    )
  printDiagnosticTable(
    "AUTOMATIC / TRANSMISSION",
    result.automatic
    )
  printDiagnosticTable(
    "CONTROLLER",
    result.controller
    )
  printDiagnosticTable(
    "POWERTRAIN",
    result.powertrain
    )
  printDiagnosticTable(
    "INTERESTING GLOBALS",
    result.globals
    )
  log("")
  log("========================================")
  log("END VLUA VEHICLE DIAGNOSTIC")
  log("========================================")
-- Keep the variable around for future configuration lookup.
vehicle = result
end
-- ============================================================================
-- Combined vehicle diagnostic
-- ============================================================================
local function runVehicleDiagnostic()
  local vehicle, vehicleId = getPlayerVehicle()
  if vehicle == nil or vehicleId == nil then
    log(
      "Cannot run vehicle diagnostic: "
      .. "no active player vehicle."
      )
    return
  end
  printDirectVehicleMetadata(
    vehicle,
    vehicleId
    )
  queueVehicleDiagnostic(
    vehicle,
    vehicleId
    )
end
-- ============================================================================
-- Vehicle handling
-- ============================================================================
local function handleVehicleChange(vehicleId)
  log(
    "Player vehicle changed: "
    .. tostring(currentVehicleId)
    .. " -> "
    .. tostring(vehicleId)
    )
  currentVehicleId = vehicleId
  if vehicleId == nil or vehicleId == 0 or vehicleId == -1 then
    log("No active player vehicle.")
    return
  end
  requestReacquire()
-- Vehicle spawning is asynchronous. Give the VLUA a little time to
-- finish initializing before asking it for gearbox/controller state.
core_jobsystem.create(function(job)
  local elapsed = 0
  while initialized and elapsed < 5.0 do
    local playerId = getPlayerVehicleId()
    if playerId == vehicleId then
      local vehicle = nil
      if be ~= nil then
        local ok, result = pcall(function()
          return be:getObjectByID(vehicleId)
        end)
        if ok then
          vehicle = result
        end
      end
      if vehicle ~= nil then
        log(
          "Running vehicle metadata diagnostic for "
          .. tostring(vehicleId)
          .. "."
          )
        printDirectVehicleMetadata(
          vehicle,
          vehicleId
          )
        queueVehicleDiagnostic(
          vehicle,
          vehicleId
          )
        return
      end
    end
    job.sleep(0.25)
    elapsed = elapsed + 0.25
  end
  if initialized then
    log(
      "Vehicle diagnostic timed out for ID "
      .. tostring(vehicleId)
      )
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
  local playerId = getPlayerVehicleId()
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
  processVehicleDiagnostic()
end
-- ============================================================================
-- Extension lifecycle
-- ============================================================================
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
-- ============================================================================
-- Exports
-- ============================================================================
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleSpawned = onVehicleSpawned
M.onPlayerVehicleChanged = onPlayerVehicleChanged
return M