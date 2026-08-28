local M = {}

local function safeValue(object, key)
  if object == nil then
    return nil
  end

  local ok, value = pcall(function()
    return object[key]
  end)
  if not ok then
    return nil
  end
  return value
end

local function safeCall(object, methodName)
  if object == nil then
    return nil
  end

  local ok, result = pcall(function()
    local method = object[methodName]
    if type(method) ~= "function" then
      return nil
    end
    return method(object)
  end)
  if not ok then
    return nil
  end
  return result
end

local function stringify(value)
  if value == nil then
    return ""
  end
  return tostring(value)
end

local function firstNonEmpty(...)
  local count = select("#", ...)
  for i = 1, count do
    local value = select(i, ...)
    if value ~= nil and tostring(value) ~= "" then
      return value
    end
  end
  return nil
end

local function parsePartConfig(partConfig)
  if type(partConfig) ~= "string" then
    return "", ""
  end

  local normalized = partConfig:gsub("\\", "/")
  local vehicleCode, configuration = normalized:match("vehicles/([^/]+)/([^/]+)%.pc$")
  return vehicleCode or "", configuration or ""
end

local function shallowStringifyTable(value, maxEntries)
  if type(value) ~= "table" then
    return ""
  end

  local pieces = {}
  local count = 0
  for key, entry in pairs(value) do
    count = count + 1
    if count > (maxEntries or 32) then
      break
    end
    local keyText = tostring(key)
    local valueText
    if type(entry) == "table" then
      valueText = "<table>"
    elseif type(entry) == "userdata" then
      valueText = "<userdata>"
    elseif type(entry) == "function" then
      valueText = "<function>"
    else
      valueText = tostring(entry)
    end
    pieces[#pieces + 1] = keyText .. "=" .. valueText
  end

  table.sort(pieces)
  return table.concat(pieces, ", ")
end

local function getVehicleType()
  local candidates = {
    safeValue(v and v.data, "vehicleType"),
    safeValue(v and v.data, "VehicleType"),
    safeValue(v and v.data, "Type"),
    safeValue(v and v.data, "type"),
    safeValue(v and v.data, "category"),
    safeValue(v and v, "vehicleType"),
    safeValue(v and v, "type"),
  }

  for i = 1, #candidates do
    local value = candidates[i]
    if value ~= nil and tostring(value) ~= "" then
      return tostring(value)
    end
  end

  return ""
end

local function getPartConfig()
  local value = firstNonEmpty(
    safeValue(v, "partConfigFilename"),
    safeValue(v, "partConfig"),
    safeValue(v and v.data, "partConfigFilename"),
    safeValue(v and v.data, "partConfig")
  )
  return stringify(value)
end

local function getTransmission()
  local gearbox = nil
  if powertrain and powertrain.getDevice then
    local ok, result = pcall(function()
      return powertrain.getDevice("gearbox")
    end)
    if ok then
      gearbox = result
    end
  end

  local rawType = firstNonEmpty(
    safeValue(gearbox, "type"),
    safeValue(gearbox, "deviceType"),
    safeValue(gearbox, "gearboxType")
  )

  local displayType = rawType
  local lower = rawType and string.lower(tostring(rawType)) or ""
  if lower == "automaticgearbox" then
    displayType = "Automatic"
  elseif lower == "manualgearbox" then
    displayType = "Manual"
  elseif lower == "sequentialgearbox" then
    displayType = "Sequential"
  elseif lower == "dctgearbox" then
    displayType = "DCT"
  elseif lower == "cvtgearbox" then
    displayType = "CVT"
  elseif lower == "electricmotor" then
    displayType = "Electric"
  end

  return stringify(displayType), stringify(rawType), gearbox
end

local function getAutomaticModes(gearbox)
  local candidates = {
    safeValue(gearbox, "availableModes"),
    safeValue(gearbox, "shiftModes"),
    safeValue(gearbox, "modes"),
    safeValue(gearbox, "availableShiftingModes"),
    safeValue(gearbox, "gearboxModes"),
    safeValue(gearbox, "defaultModes"),
  }

  for i = 1, #candidates do
    local value = candidates[i]
    if value ~= nil then
      if type(value) == "string" then
        return value
      end
      if type(value) == "table" then
        local text = shallowStringifyTable(value, 32)
        if text ~= "" then
          return text
        end
      end
    end
  end

  return ""
end

local function getGearLayout(gearbox)
  local candidates = {
    safeValue(gearbox, "gearLayout"),
    safeValue(gearbox, "shiftPattern"),
    safeValue(gearbox, "shiftLayout"),
    safeValue(gearbox, "gearPattern"),
    safeValue(gearbox, "gearPositions"),
    safeValue(gearbox, "positions"),
    safeValue(gearbox, "shifterPositions"),
  }

  for i = 1, #candidates do
    local value = candidates[i]
    if value ~= nil then
      if type(value) == "string" then
        return value
      end
      if type(value) == "table" then
        local text = shallowStringifyTable(value, 64)
        if text ~= "" then
          return text
        end
      end
    end
  end

  return ""
end

local function sendToGE(metadata)
  if obj == nil or obj.queueGameEngineLua == nil then
    print("[MultiFFBJoy/VLUA] queueGameEngineLua unavailable.")
    return false
  end

  local ok, encoded = pcall(function()
    return jsonEncode(metadata)
  end)
  if not ok or encoded == nil then
    print("[MultiFFBJoy/VLUA] jsonEncode failed: " .. tostring(encoded))
    return false
  end

  local command = "multiffbjoy.receiveVehicleMetadata(jsonDecode(" .. string.format("%q", encoded) .. "))"
  local queueOK, queueError = pcall(function()
    obj:queueGameEngineLua(command)
  end)
  if not queueOK then
    print("[MultiFFBJoy/VLUA] queueGameEngineLua failed: " .. tostring(queueError))
    return false
  end

  return true
end

function M.sendMetadata()
  local partConfig = getPartConfig()
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

  local transmission, transmissionRaw, gearbox = getTransmission()
  local automaticModes = getAutomaticModes(gearbox)
  local gearLayout = getGearLayout(gearbox)

  local metadata = {
    vehicleId = obj and obj.getID and obj:getID() or -1,
    vehicle = stringify(vehicleCode),
    configuration = stringify(configurationCode),
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
    gearboxMode = stringify(electrics and electrics.values and electrics.values.gearboxMode),
    gear = stringify(electrics and electrics.values and electrics.values.gear),
    gearIndex = stringify(electrics and electrics.values and electrics.values.gearIndex),
    automaticModes = automaticModes,
    gearLayout = gearLayout,
    gearboxFields = shallowStringifyTable(gearbox, 48),
  }

  print("[MultiFFBJoy/VLUA] Metadata: vehicle=" .. metadata.vehicle
    .. " config=" .. metadata.configuration
    .. " type=" .. metadata.vehicleType
    .. " transmission=" .. metadata.transmission
    .. " rawTransmission=" .. metadata.transmissionRaw
    .. " gear=" .. metadata.gear
    .. " gearIndex=" .. metadata.gearIndex
    .. " layout=" .. metadata.gearLayout)

  return sendToGE(metadata)
end

function M.onExtensionLoaded()
  -- The GE extension explicitly requests metadata after loading this VM extension.
end

return M
