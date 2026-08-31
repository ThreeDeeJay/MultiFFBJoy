local M = {}

local stateTimer = 0
local STATE_INTERVAL = 0.05
local lastStateSignature = nil
local metadataSent = false
local pendingShift = nil

local function safeValue(object, key)
  if object == nil then return nil end
  local ok, value = pcall(function() return object[key] end)
  return ok and value or nil
end

local function safeCall(object, methodName)
  if object == nil then return nil end
  local ok, result = pcall(function()
    local method = object[methodName]
    if type(method) ~= "function" then return nil end
    return method(object)
  end)
  return ok and result or nil
end

local function stringify(value)
  if value == nil then return "" end
  return tostring(value)
end

local function firstNonEmpty(...)
  for i = 1, select("#", ...) do
    local value = select(i, ...)
    if value ~= nil and tostring(value) ~= "" then return value end
  end
  return nil
end

local function parsePartConfig(partConfig)
  if type(partConfig) ~= "string" then return "", "" end
  local normalized = partConfig:gsub("\\", "/")
  local vehicleCode, configuration = normalized:match("vehicles/([^/]+)/([^/]+)%.pc$")
  return vehicleCode or "", configuration or ""
end

local function shallowStringifyTable(value, maxEntries)
  if type(value) ~= "table" then return "" end
  local pieces, count = {}, 0
  for key, entry in pairs(value) do
    count = count + 1
    if count > (maxEntries or 32) then break end
    local valueText
    if type(entry) == "table" then valueText = "<table>"
    elseif type(entry) == "userdata" then valueText = "<userdata>"
    elseif type(entry) == "function" then valueText = "<function>"
    else valueText = tostring(entry) end
    pieces[#pieces + 1] = tostring(key) .. "=" .. valueText
  end
  table.sort(pieces)
  return table.concat(pieces, ", ")
end

local function getPartConfig()
  return stringify(firstNonEmpty(
    safeValue(v, "partConfigFilename"), safeValue(v, "partConfig"),
    safeValue(v and v.data, "partConfigFilename"), safeValue(v and v.data, "partConfig")
  ))
end

local function getIdentity(partConfig)
  local parsedVehicle, parsedConfiguration = parsePartConfig(partConfig)
  local vehicleCode = firstNonEmpty(
    safeValue(v, "JBeam"), safeValue(v, "jBeam"), safeCall(v, "getJBeamFilename"),
    safeValue(v and v.data, "JBeam"), safeValue(v and v.data, "jBeam"), parsedVehicle)
  local configurationCode = firstNonEmpty(
    safeValue(v, "configuration"), safeValue(v, "configurationName"), safeValue(v, "configName"),
    safeValue(v and v.data, "configuration"), safeValue(v and v.data, "configurationName"), parsedConfiguration)
  return stringify(vehicleCode), stringify(configurationCode), parsedVehicle
end

local function getVehicleType()
  local candidates = {
    safeValue(v and v.data, "vehicleType"), safeValue(v and v.data, "VehicleType"),
    safeValue(v and v.data, "Type"), safeValue(v and v.data, "type"),
    safeValue(v and v.data, "category"), safeValue(v, "vehicleType"), safeValue(v, "type")
  }
  for _, value in ipairs(candidates) do
    if value ~= nil and tostring(value) ~= "" then return tostring(value) end
  end
  return ""
end

local function getTransmission()
  local gearbox = nil
  if powertrain and powertrain.getDevice then
    local ok, result = pcall(function() return powertrain.getDevice("gearbox") end)
    if ok then gearbox = result end
  end
  local rawType = firstNonEmpty(safeValue(gearbox, "type"), safeValue(gearbox, "deviceType"), safeValue(gearbox, "gearboxType"))
  local lower = rawType and string.lower(tostring(rawType)) or ""
  local display = rawType
  if lower == "automaticgearbox" then display = "Automatic"
  elseif lower == "manualgearbox" then display = "Manual"
  elseif lower == "sequentialgearbox" then display = "Sequential"
  elseif lower == "dctgearbox" then display = "DCT"
  elseif lower == "cvtgearbox" then display = "CVT"
  elseif lower == "electricmotor" then display = "Electric" end
  return stringify(display), stringify(rawType), gearbox
end

local function getGearState(gearbox)
  local values = electrics and electrics.values or nil
  local mainController = nil

  -- BeamNG's documented gear API belongs to the active vehicle-controller /
  -- shift-logic controller, not to the powertrain gearbox device itself.
  -- In particular, vehicleController exposes getGearName(), getGearPosition()
  -- and currentGearIndex.  The electrics values are also authoritative and
  -- are useful as a fallback.
  if controller ~= nil then
    mainController = safeValue(controller, "mainController")
  end

  local gear = firstNonEmpty(
    safeCall(mainController, "getGearName"),
    safeValue(mainController, "gear"),
    safeValue(mainController, "currentGear"),
    safeValue(values, "gear"),
    safeCall(gearbox, "getGearName"),
    safeValue(gearbox, "gear"),
    safeValue(gearbox, "currentGear")
  )

  local gearIndex = firstNonEmpty(
    safeValue(mainController, "currentGearIndex"),
    safeValue(mainController, "gearIndex"),
    safeCall(mainController, "getGearIndex"),
    safeValue(values, "gearIndex"),
    safeValue(gearbox, "currentGearIndex"),
    safeValue(gearbox, "gearIndex"),
    safeCall(gearbox, "getGearIndex")
  )

  local gearboxMode = firstNonEmpty(
    safeValue(mainController, "gearboxMode"),
    safeValue(mainController, "mode"),
    safeCall(mainController, "getGearboxMode"),
    safeValue(values, "gearboxMode"),
    safeValue(gearbox, "gearboxMode"),
    safeValue(gearbox, "mode")
  )

  local gearPosition = firstNonEmpty(
    safeCall(mainController, "getGearPosition"),
    safeValue(mainController, "gearPosition"),
    safeValue(mainController, "currentGearPosition"),
    safeValue(values, "gear_A"),
    safeCall(gearbox, "getGearPosition"),
    safeValue(gearbox, "gearPosition"),
    safeValue(gearbox, "currentGearPosition")
  )

  -- During the first frame after spawn, getGearName() can be empty even
  -- though the automatic shift logic already knows its startup mode.
  if (gear == nil or tostring(gear) == "") and gearboxMode ~= nil and tostring(gearboxMode) ~= "" then
    local mode = tostring(gearboxMode)
    if mode == "park" then gear = "P"
    elseif mode == "reverse" then gear = "R"
    elseif mode == "neutral" then gear = "N"
    elseif mode == "drive" then gear = "D"
    elseif mode == "low" then gear = "L" end
  end

  return stringify(gear), stringify(gearIndex), stringify(gearboxMode), stringify(gearPosition)
end

local function getAutomaticModes(gearbox)
  local values = electrics and electrics.values or nil
  local mainController = controller and safeValue(controller, "mainController") or nil
  local controllerConfig = firstNonEmpty(
    safeValue(jbeamData, "vehicleController"),
    safeValue(v and v.data, "vehicleController")
  )

  local candidates = {
    safeValue(controllerConfig, "automaticModes"),
    safeValue(jbeamData, "automaticModes"),
    safeValue(v and v.data, "automaticModes"),
    safeValue(mainController, "automaticModes"),
    safeValue(mainController, "availableModes"),
    safeValue(mainController, "shiftModes"),
    safeValue(gearbox, "automaticModes"),
    safeValue(gearbox, "availableModes"),
    safeValue(values, "automaticModes"),
  }

  for _, value in ipairs(candidates) do
    if type(value) == "string" and value ~= "" then return value end
    if type(value) == "table" then
      local text = shallowStringifyTable(value, 32)
      if text ~= "" then return text end
    end
  end
  return ""
end

local function getDefaultAutomaticMode()
  local controllerConfig = firstNonEmpty(
    safeValue(jbeamData, "vehicleController"),
    safeValue(v and v.data, "vehicleController")
  )
  return stringify(firstNonEmpty(
    safeValue(controllerConfig, "defaultAutomaticMode"),
    safeValue(controllerConfig, "defaultAutomaticForwardMode")
  ))
end

local function getGearLayout(gearbox, automaticModes)
  local candidates = {
    safeValue(gearbox, "gearLayout"), safeValue(gearbox, "shiftPattern"),
    safeValue(gearbox, "shiftLayout"), safeValue(gearbox, "gearPattern"),
    safeValue(gearbox, "gearPositions"), safeValue(gearbox, "positions"),
    safeValue(gearbox, "shifterPositions")
  }
  for _, value in ipairs(candidates) do
    if type(value) == "string" and value ~= "" then return value end
    if type(value) == "table" then
      local text = shallowStringifyTable(value, 64)
      if text ~= "" then return text end
    end
  end
  -- automaticModes is useful fallback metadata: PRND21/PRNDS21M etc.
  return automaticModes or ""
end

local function queueToGE(payload)
  if obj == nil or obj.queueGameEngineLua == nil then
    print("[MultiFFBJoy/VLUA] queueGameEngineLua unavailable.")
    return false
  end
  local ok, encoded = pcall(function() return jsonEncode(payload) end)
  if not ok or encoded == nil then
    print("[MultiFFBJoy/VLUA] jsonEncode failed: " .. tostring(encoded))
    return false
  end
  local command = "multiffbjoy." .. payload._geFunction .. "(jsonDecode(" .. string.format("%q", encoded) .. "))"
  payload._geFunction = nil
  local queueOK, queueError = pcall(function() obj:queueGameEngineLua(command) end)
  if not queueOK then
    print("[MultiFFBJoy/VLUA] queueGameEngineLua failed: " .. tostring(queueError))
    return false
  end
  return true
end

local function buildMetadata()
  local partConfig = getPartConfig()
  local vehicleCode, configurationCode, parsedVehicle = getIdentity(partConfig)
  local transmission, transmissionRaw, gearbox = getTransmission()
  local gear, gearIndex, gearboxMode, gearPosition = getGearState(gearbox)
  local automaticModes = getAutomaticModes(gearbox)
  local defaultAutomaticMode = getDefaultAutomaticMode()
  local values = electrics and electrics.values or nil
  local gearboxGearName = stringify(safeCall(gearbox, "getGearName"))
  local gearboxGearPosition = stringify(safeCall(gearbox, "getGearPosition"))
  local gearboxCurrentGearIndex = stringify(safeValue(gearbox, "currentGearIndex"))
  local mainController = controller and safeValue(controller, "mainController") or nil
  local controllerGearName = stringify(safeCall(mainController, "getGearName"))
  local controllerGearPosition = stringify(safeCall(mainController, "getGearPosition"))
  local controllerGearIndex = stringify(safeValue(mainController, "currentGearIndex"))
  local electricsGear = stringify(safeValue(values, "gear"))
  local electricsGearIndex = stringify(safeValue(values, "gearIndex"))
  local electricsGearA = stringify(safeValue(values, "gear_A"))

  return {
    vehicleId = obj and obj.getID and obj:getID() or -1,
    vehicle = vehicleCode,
    configuration = configurationCode,
    partConfig = partConfig,
    jBeam = stringify(firstNonEmpty(safeValue(v, "JBeam"), safeValue(v, "jBeam"), safeCall(v, "getJBeamFilename"), parsedVehicle)),
    vehicleType = getVehicleType(),
    transmission = transmission,
    transmissionRaw = transmissionRaw,
    gearboxMode = gearboxMode,
    gear = gear,
    gearIndex = gearIndex,
    gearPosition = gearPosition,
    automaticModes = automaticModes,
    defaultAutomaticMode = defaultAutomaticMode,
    gearLayout = getGearLayout(gearbox, automaticModes),
    gearboxFields = shallowStringifyTable(gearbox, 48),
    gearDiagnostics = "electrics.gear=" .. electricsGear
      .. "; electrics.gearIndex=" .. electricsGearIndex
      .. "; electrics.gear_A=" .. electricsGearA
      .. "; gearbox.getGearName=" .. gearboxGearName
      .. "; gearbox.getGearPosition=" .. gearboxGearPosition
      .. "; gearbox.currentGearIndex=" .. gearboxCurrentGearIndex
      .. "; controller.getGearName=" .. controllerGearName
      .. "; controller.getGearPosition=" .. controllerGearPosition
      .. "; controller.currentGearIndex=" .. controllerGearIndex
      .. "; automaticModes=" .. automaticModes
      .. "; defaultAutomaticMode=" .. defaultAutomaticMode,
  }
end

local function sendMetadataAndPrimeState()
  local metadata = buildMetadata()
  print("[MultiFFBJoy/VLUA] Metadata: vehicle=" .. metadata.vehicle
    .. " config=" .. metadata.configuration
    .. " type=" .. metadata.vehicleType
    .. " transmission=" .. metadata.transmission
    .. " rawTransmission=" .. metadata.transmissionRaw
    .. " gear=" .. metadata.gear
    .. " gearIndex=" .. metadata.gearIndex
    .. " gearPosition=" .. metadata.gearPosition
    .. " layout=" .. metadata.gearLayout
    .. " [" .. metadata.gearDiagnostics .. "]")
  metadata._geFunction = "receiveVehicleMetadata"
  metadataSent = queueToGE(metadata)
  lastStateSignature = nil
end

function M.sendMetadata()
  sendMetadataAndPrimeState()
end

local function queueGearVerification(expectedIndex, expectedName, ok, err)
  local metadata = buildMetadata()
  local payload = {
    vehicleId = metadata.vehicleId,
    expectedIndex = expectedIndex,
    expectedName = expectedName or "",
    shiftCallSucceeded = ok == true,
    shiftError = stringify(err),
    gear = metadata.gear,
    gearIndex = metadata.gearIndex,
    gearPosition = metadata.gearPosition,
    controllerGear = "",
    controllerIndex = "",
    controllerPosition = "",
  }

  local mainController = controller and safeValue(controller, "mainController") or nil
  payload.controllerGear = stringify(safeCall(mainController, "getGearName"))
  payload.controllerIndex = stringify(safeValue(mainController, "currentGearIndex"))
  payload.controllerPosition = stringify(safeCall(mainController, "getGearPosition"))
  payload._geFunction = "receiveGearVerification"
  queueToGE(payload)
end

function M.shiftToGearIndex(index, expectedName)
  local idx = tonumber(index)
  if idx == nil then
    print("[MultiFFBJoy/VLUA] SHIFT rejected: invalid index " .. tostring(index))
    return false
  end
  idx = math.floor(idx)

  local mainController = controller and safeValue(controller, "mainController") or nil
  if mainController == nil and controller and controller.getController then
    local ok, result = pcall(function() return controller.getController("main") end)
    if ok then mainController = result end
  end

  if mainController == nil or type(safeValue(mainController, "shiftToGearIndex")) ~= "function" then
    print("[MultiFFBJoy/VLUA] SHIFT failed: main vehicle controller has no shiftToGearIndex()")
    queueGearVerification(idx, expectedName, false, "main controller unavailable")
    return false
  end

  local transmission = select(1, getTransmission())
  local automatic = transmission == "Automatic" or transmission == "DCT" or transmission == "CVT"
  local callIndex = idx

  if automatic then
    -- BeamNG's automaticModes string is exposed as a 1-based Lua sequence
    -- (P=1, R=2, N=3, D=4, 1=5 for PRND1).  gearIndex is NOT this selector
    -- index; it is the actual transmission gear (for example D commonly has
    -- gearIndex=1).  The documented vehicle-controller shiftToGearIndex()
    -- accepts the selector position, so use the 1-based layout position.
    local metadata = buildMetadata()
    local target = string.lower(tostring(expectedName or ""))
    local aliases = {
      park = "p", reverse = "r", neutral = "n", drive = "d",
      low = "l", ["1st"] = "1", first = "1", sport = "s"
    }
    target = aliases[target] or target

    local targetPosition = nil
    local modes = tostring(metadata.automaticModes or "")
    for i = 1, #modes do
      local mode = string.lower(modes:sub(i, i))
      if mode == target then
        targetPosition = i
        break
      end
    end

    if targetPosition == nil then
      local err = "target mode '" .. tostring(target) .. "' not found in automaticModes='" .. modes .. "'"
      print("[MultiFFBJoy/VLUA] AUTO SHIFT rejected: " .. err)
      queueGearVerification(idx, expectedName, false, err)
      return false
    end
    callIndex = targetPosition
    print("[MultiFFBJoy/VLUA] AUTO SHIFT requested mode=" .. tostring(expectedName)
      .. " selectorIndex=" .. tostring(callIndex)
      .. " automaticModes=" .. modes)
  end

  -- Public BeamNG controller functions are dot-style functions; do not pass
  -- the controller table as an implicit self argument.
  local ok, err = pcall(function()
    mainController.shiftToGearIndex(callIndex)
  end)

  if not ok then
    print("[MultiFFBJoy/VLUA] SHIFT call failed index=" .. tostring(callIndex)
      .. ": " .. tostring(err))
  elseif not automatic then
    print("[MultiFFBJoy/VLUA] SHIFT requested index=" .. tostring(callIndex)
      .. " expected=" .. tostring(expectedName or ""))
  else
    print("[MultiFFBJoy/VLUA] AUTO SHIFT call succeeded selectorIndex="
      .. tostring(callIndex) .. " expected=" .. tostring(expectedName or ""))
  end

  pendingShift = {
    expectedIndex = idx,
    expectedName = tostring(expectedName or ""),
    automatic = automatic,
    elapsed = 0,
    age = 0,
    callSucceeded = ok,
    error = err,
  }

  if not ok then
    queueGearVerification(idx, expectedName, ok, err)
    pendingShift = nil
  end
  return ok
end

function M.onUpdate(dtReal, dtSim, dtRaw)
  local dt = dtReal or 0
  stateTimer = stateTimer + dt

  if pendingShift ~= nil then
    pendingShift.elapsed = pendingShift.elapsed + dt
    if pendingShift.elapsed >= 0.05 then
      pendingShift.elapsed = 0
      local metadata = buildMetadata()
      local actualIndex = tonumber(metadata.gearIndex)
      local mainController = controller and safeValue(controller, "mainController") or nil
      local controllerIndex = tonumber(safeValue(mainController, "currentGearIndex"))
      local expected = pendingShift.expectedIndex

      local confirmed = false
      if pendingShift.automatic then
        local actual = string.lower(tostring(metadata.gear or ""))
        local expectedName = string.lower(tostring(pendingShift.expectedName or ""))
        local aliases = { park="p", reverse="r", neutral="n", drive="d", low="l", ["1st"]="1", first="1", sport="s" }
        expectedName = aliases[expectedName] or expectedName
        confirmed = actual == expectedName
      else
        confirmed = actualIndex == expected or controllerIndex == expected
      end

      if confirmed then
        queueGearVerification(expected, pendingShift.expectedName, pendingShift.callSucceeded, pendingShift.error)
        pendingShift = nil
      end
    end

    -- Use a separate age accumulator so a permanently refused shift cannot
    -- leave the verification pending forever.
    pendingShift.age = (pendingShift.age or 0) + dt
    if pendingShift ~= nil and pendingShift.age >= 1.5 then
      queueGearVerification(pendingShift.expectedIndex, pendingShift.expectedName, pendingShift.callSucceeded, pendingShift.error)
      pendingShift = nil
    end
  end

  if stateTimer < STATE_INTERVAL then return end
  stateTimer = 0
  sendStateIfChanged()
end

function M.onExtensionLoaded()
  stateTimer = 0
  lastStateSignature = nil
  metadataSent = false
end

function M.onExtensionUnloaded()
  metadataSent = false
  lastStateSignature = nil
end

return M
