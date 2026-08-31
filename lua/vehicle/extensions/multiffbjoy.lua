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

-- BeamNG public controller APIs are exposed as functions on the controller
-- table; they are not colon-style methods. Calling method(object, ...) passes
-- the controller table as an extra first argument and breaks functions such
-- as manualGearbox.shiftToGearIndex(), which expects a numeric index.
local function safeCall(object, methodName, ...)
  if object == nil then return nil end
  local method = safeValue(object, methodName)
  if type(method) ~= "function" then return nil end
  local ok, result = pcall(method, ...)
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
    safeValue(v, "partConfigFilename"),
    safeValue(v, "partConfig"),
    safeValue(v and v.data, "partConfigFilename"),
    safeValue(v and v.data, "partConfig")
  ))
end

local function getIdentity(partConfig)
  local parsedVehicle, parsedConfiguration = parsePartConfig(partConfig)
  local vehicleCode = firstNonEmpty(
    safeValue(v, "JBeam"),
    safeValue(v, "jBeam"),
    safeCall(v, "getJBeamFilename"),
    safeValue(v and v.data, "JBeam"),
    safeValue(v and v.data, "jBeam"),
    parsedVehicle
  )
  local configurationCode = firstNonEmpty(
    safeValue(v, "configuration"),
    safeValue(v, "configurationName"),
    safeValue(v, "configName"),
    safeValue(v and v.data, "configuration"),
    safeValue(v and v.data, "configurationName"),
    parsedConfiguration
  )
  return stringify(vehicleCode), stringify(configurationCode), parsedVehicle
end

local function getVehicleType()
  local candidates = {
    safeValue(v and v.data, "vehicleType"),
    safeValue(v and v.data, "VehicleType"),
    safeValue(v and v.data, "Type"),
    safeValue(v and v.data, "type"),
    safeValue(v and v.data, "category"),
    safeValue(v, "vehicleType"),
    safeValue(v, "type")
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

  local rawType = firstNonEmpty(
    safeValue(gearbox, "type"),
    safeValue(gearbox, "deviceType"),
    safeValue(gearbox, "gearboxType")
  )
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

local function getMainController()
  if controller == nil then return nil end

  local main = safeValue(controller, "mainController")
  if main ~= nil then return main end

  if type(controller.getController) == "function" then
    local ok, result = pcall(controller.getController, "main")
    if ok then return result end
  end

  return nil
end

local function getGearState(gearbox)
  local values = electrics and electrics.values or nil
  local mainController = getMainController()

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

  if (gear == nil or tostring(gear) == "") and gearboxMode ~= nil then
    local mode = string.lower(tostring(gearboxMode))
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
  local mainController = getMainController()
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
    safeValue(gearbox, "gearLayout"),
    safeValue(gearbox, "shiftPattern"),
    safeValue(gearbox, "shiftLayout"),
    safeValue(gearbox, "gearPattern"),
    safeValue(gearbox, "gearPositions"),
    safeValue(gearbox, "positions"),
    safeValue(gearbox, "shifterPositions")
  }

  for _, value in ipairs(candidates) do
    if type(value) == "string" and value ~= "" then return value end
    if type(value) == "table" then
      local text = shallowStringifyTable(value, 64)
      if text ~= "" then return text end
    end
  end

  return automaticModes or ""
end

local function queueToGE(payload)
  if obj == nil or obj.queueGameEngineLua == nil then
    print("[MultiFFBJoy/VLUA] queueGameEngineLua unavailable.")
    return false
  end

  local functionName = payload._geFunction
  payload._geFunction = nil

  local ok, encoded = pcall(function() return jsonEncode(payload) end)
  if not ok or encoded == nil then
    print("[MultiFFBJoy/VLUA] jsonEncode failed: " .. tostring(encoded))
    return false
  end

  local command = "multiffbjoy." .. tostring(functionName)
    .. "(jsonDecode(" .. string.format("%q", encoded) .. "))"

  local queueOK, queueError = pcall(function()
    obj:queueGameEngineLua(command)
  end)

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
  local mainController = getMainController()

  local gearboxGearName = stringify(safeCall(gearbox, "getGearName"))
  local gearboxGearPosition = stringify(safeCall(gearbox, "getGearPosition"))
  local gearboxCurrentGearIndex = stringify(safeValue(gearbox, "currentGearIndex"))
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
    jBeam = stringify(firstNonEmpty(
      safeValue(v, "JBeam"),
      safeValue(v, "jBeam"),
      safeCall(v, "getJBeamFilename"),
      parsedVehicle
    )),
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
    gearDiagnostics =
      "electrics.gear=" .. electricsGear
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

local function queueGearVerification(expectedIndex, expectedName, callSucceeded, err, attempt)
  local metadata = buildMetadata()
  local payload = {
    vehicleId = metadata.vehicleId,
    expectedIndex = expectedIndex,
    expectedName = expectedName or "",
    shiftCallSucceeded = callSucceeded == true,
    shiftError = stringify(err),
    attempt = attempt or 0,
    gear = metadata.gear,
    gearIndex = metadata.gearIndex,
    gearPosition = metadata.gearPosition,
    controllerGear = "",
    controllerIndex = "",
    controllerPosition = "",
    gearboxMode = metadata.gearboxMode,
    transmission = metadata.transmission,
    gearDiagnostics = metadata.gearDiagnostics,
  }

  local mainController = getMainController()
  payload.controllerGear = stringify(safeCall(mainController, "getGearName"))
  payload.controllerIndex = stringify(safeValue(mainController, "currentGearIndex"))
  payload.controllerPosition = stringify(safeCall(mainController, "getGearPosition"))

  payload._geFunction = "receiveGearVerification"
  queueToGE(payload)
end

local function normalizeGearName(name)
  if name == nil then return "" end
  local s = string.lower(tostring(name))
  s = s:gsub("^s", "")
  return s
end

local function expectedAutomaticName(expectedName)
  local s = normalizeGearName(expectedName)
  if s == "park" or s == "p" then return "p" end
  if s == "reverse" or s == "r" then return "r" end
  if s == "neutral" or s == "n" then return "n" end
  if s == "drive" or s == "d" then return "d" end
  if s == "low" or s == "l" then return "l" end
  return s
end

local function shiftStateMatches(pending)
  local metadata = buildMetadata()
  local actualGear = normalizeGearName(metadata.gear)
  local actualIndex = tonumber(metadata.gearIndex)
  local expectedIndex = pending.expectedIndex
  local expectedName = normalizeGearName(pending.expectedName)

  if metadata.transmission == "Automatic"
    or metadata.transmission == "DCT"
    or metadata.transmission == "CVT" then
    local expected = expectedAutomaticName(expectedName)
    if expected ~= "" and actualGear == expected then
      return true, metadata
    end

    -- Some automatic controllers report the current shifter index through
    -- the controller's currentGearIndex. Do not use electrics.gearIndex as
    -- the shifter-position index because BeamNG documents that gearIndex is
    -- the selected transmission gear, while gear_A is the automatic shifter
    -- position.
    local controllerIndex = tonumber(metadata.controllerIndex or "")
    if controllerIndex ~= nil and controllerIndex == expectedIndex then
      return true, metadata
    end

    return false, metadata
  end

  -- Manual/sequential: gearIndex is the authoritative selected gear index.
  if actualIndex ~= nil and actualIndex == expectedIndex then
    return true, metadata
  end

  return false, metadata
end

function M.shiftToGearIndex(index, expectedName)
  local idx = tonumber(index)
  if idx == nil then
    print("[MultiFFBJoy/VLUA] SHIFT rejected: invalid index " .. tostring(index))
    return false
  end

  idx = math.floor(idx)

  local mainController = getMainController()
  local shiftFunction = mainController and safeValue(mainController, "shiftToGearIndex") or nil

  if mainController == nil or type(shiftFunction) ~= "function" then
    local errorText = "main vehicle controller has no shiftToGearIndex()"
    print("[MultiFFBJoy/VLUA] SHIFT failed: " .. errorText)
    queueGearVerification(idx, expectedName, false, errorText, 0)
    return false
  end

  -- IMPORTANT: call the public BeamNG controller function with DOT syntax.
  -- The function is defined as shiftToGearIndex(index), not as a colon
  -- method. Calling mainController:shiftToGearIndex(idx) passes the whole
  -- controller table as argument #1 and causes manualGearbox.lua to throw:
  -- "bad argument #1 to 'max' (number expected, got table)".
  local ok, err = pcall(shiftFunction, idx)

  if not ok then
    print("[MultiFFBJoy/VLUA] SHIFT call failed index=" .. tostring(idx)
      .. ": " .. tostring(err))
  else
    print("[MultiFFBJoy/VLUA] SHIFT requested index=" .. tostring(idx)
      .. " expected=" .. tostring(expectedName or ""))
  end

  pendingShift = {
    expectedIndex = idx,
    expectedName = tostring(expectedName or ""),
    elapsed = 0,
    age = 0,
    callSucceeded = ok,
    error = err,
    attempts = 0,
  }

  -- Verify on subsequent simulation ticks. A successful Lua call only means
  -- the request was accepted by the controller; it does NOT mean the vehicle
  -- has reached the requested gear yet.
  return ok
end

local function getStateSignature()
  local metadata = buildMetadata()
  return table.concat({
    tostring(metadata.vehicleId),
    metadata.gear,
    metadata.gearIndex,
    metadata.gearPosition,
    metadata.gearboxMode,
  }, "|"), metadata
end

local function sendStateIfChanged()
  local metadata = buildMetadata()
  local signature = table.concat({
    tostring(metadata.vehicleId),
    metadata.gear,
    metadata.gearIndex,
    metadata.gearboxMode,
    metadata.gearPosition,
  }, "|")

  if signature == lastStateSignature then return end
  lastStateSignature = signature

  metadata._geFunction = "receiveVehicleState"
  queueToGE(metadata)
end

function M.onUpdate(dtReal, dtSim, dtRaw)
  local dt = dtReal or 0
  stateTimer = stateTimer + dt

  if pendingShift ~= nil then
    pendingShift.elapsed = pendingShift.elapsed + dt
    pendingShift.age = pendingShift.age + dt

    if pendingShift.elapsed >= 0.05 then
      pendingShift.elapsed = 0
      pendingShift.attempts = pendingShift.attempts + 1

      local confirmed, metadata = shiftStateMatches(pendingShift)
      if confirmed then
        print("[MultiFFBJoy/VLUA] SHIFT CONFIRMED: expected="
          .. tostring(pendingShift.expectedName)
          .. " index=" .. tostring(pendingShift.expectedIndex)
          .. " actualGear=" .. tostring(metadata.gear)
          .. " actualGearIndex=" .. tostring(metadata.gearIndex)
          .. " controllerIndex=" .. tostring(metadata.controllerIndex or "")
          .. " gearPosition=" .. tostring(metadata.gearPosition))

        queueGearVerification(
          pendingShift.expectedIndex,
          pendingShift.expectedName,
          pendingShift.callSucceeded,
          pendingShift.error,
          pendingShift.attempts
        )
        pendingShift = nil
      elseif pendingShift.age >= 1.5 then
        print("[MultiFFBJoy/VLUA] SHIFT NOT CONFIRMED: requested shift did not"
          .. " reach the authoritative vehicle state."
          .. " expected=" .. tostring(pendingShift.expectedName)
          .. " index=" .. tostring(pendingShift.expectedIndex)
          .. " actualGear=" .. tostring(metadata.gear)
          .. " actualGearIndex=" .. tostring(metadata.gearIndex)
          .. " controllerIndex=" .. tostring(metadata.controllerIndex or "")
          .. " gearPosition=" .. tostring(metadata.gearPosition))

        queueGearVerification(
          pendingShift.expectedIndex,
          pendingShift.expectedName,
          pendingShift.callSucceeded,
          pendingShift.error,
          pendingShift.attempts
        )
        pendingShift = nil
      end
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
  pendingShift = nil
end

function M.onExtensionUnloaded()
  metadataSent = false
  lastStateSignature = nil
  pendingShift = nil
end

return M
