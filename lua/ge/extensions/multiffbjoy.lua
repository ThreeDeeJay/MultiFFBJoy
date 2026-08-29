local M = {}

local socket = nil
local udp = nil
local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458

local initialized = false
local currentVehicleId = nil

local lastReacquireTime = -1000
local REACQUIRE_COOLDOWN = 1.0

-- Gear command verification state.
local pendingShift = nil
local nextShiftId = 0
local lastVerifiedGearIndex = nil
local lastVerifiedGearName = nil
local lastMetadataTime = -1000
local METADATA_POLL_INTERVAL = 0.25
local SHIFT_VERIFY_TIMEOUT = 1.5
local SHIFT_VERIFY_INTERVAL = 0.10

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

  if result == nil or result <= 0 then
    return nil
  end

  return result
end

local function getVehicleObject(vehicleId)
  if vehicleId == nil or vehicleId <= 0 or be == nil then
    return nil
  end

  local ok, vehicle = pcall(function()
    return be:getObjectByID(vehicleId)
  end)

  if not ok then
    log("getObjectByID(" .. tostring(vehicleId) .. ") failed: " .. tostring(vehicle))
    return nil
  end

  return vehicle
end

local function queueVehicleLua(vehicleId, command)
  local vehicle = getVehicleObject(vehicleId)
  if vehicle == nil then
    log("Cannot queue vehicle Lua command: vehicle " .. tostring(vehicleId) .. " is unavailable.")
    return false
  end

  local ok, err = pcall(function()
    vehicle:queueLuaCommand(command)
  end)

  if not ok then
    log("vehicle:queueLuaCommand failed: " .. tostring(err))
    return false
  end

  log("VLUA command queued: " .. command)
  return true
end

local function requestVehicleMetadata(reason)
  if currentVehicleId == nil then
    return false
  end

  local vehicleId = currentVehicleId
  local command = [[
local function __mffb_get(v)
  local function s(x)
    if x == nil then return "" end
    return tostring(x)
  end

  local gear = ""
  local gearIndex = ""
  local gearPosition = ""
  local gearboxMode = ""
  local transmission = ""
  local rawTransmission = ""
  local layout = ""
  local automaticModes = ""
  local defaultAutomaticMode = ""
  local vehicleName = ""
  local configName = ""

  pcall(function()
    if electrics then
      gear = s(electrics.gear)
      gearIndex = s(electrics.gearIndex)
      gearPosition = s(electrics.gear_A)
    end
  end)

  pcall(function()
    if controller then
      if controller.getGearName then
        gear = s(controller.getGearName())
      end
      if controller.getGearPosition then
        gearPosition = s(controller.getGearPosition())
      end
      if controller.currentGearIndex ~= nil then
        gearIndex = s(controller.currentGearIndex)
      end
    end
  end)

  pcall(function()
    if controller and controller.gearboxBehavior then
      gearboxMode = s(controller.gearboxBehavior)
    end
  end)

  pcall(function()
    if controller and controller.shiftLogicName then
      rawTransmission = s(controller.shiftLogicName)
    end
  end)

  pcall(function()
    if controller and controller.automaticModes then
      automaticModes = s(controller.automaticModes)
      layout = automaticModes
    end
    if controller and controller.defaultAutomaticMode then
      defaultAutomaticMode = s(controller.defaultAutomaticMode)
    end
  end)

  -- Best-effort vehicle/config names. These may be unavailable in VLUA;
  -- GE will fill them from its own metadata path when necessary.
  pcall(function()
    if v and v.vehicleDirectory then
      vehicleName = s(v.vehicleDirectory)
    end
  end)

  print(string.format(
    "[MultiFFBJoy/VLUA] Metadata: vehicle=%s config=%s type=%s transmission=%s rawTransmission=%s gear=%s gearIndex=%s gearPosition=%s layout=%s [electrics.gear=%s; electrics.gearIndex=%s; electrics.gear_A=%s; gearbox.getGearName=%s; gearbox.getGearPosition=%s; gearbox.currentGearIndex=%s; controller.getGearName=%s; controller.getGearPosition=%s; controller.currentGearIndex=%s; automaticModes=%s; defaultAutomaticMode=%s]",
    vehicleName,
    configName,
    "",
    transmission,
    rawTransmission,
    gear,
    gearIndex,
    gearPosition,
    layout,
    s(electrics and electrics.gear),
    s(electrics and electrics.gearIndex),
    s(electrics and electrics.gear_A),
    s((gearbox and gearbox.getGearName) and gearbox.getGearName()),
    s((gearbox and gearbox.getGearPosition) and gearbox.getGearPosition()),
    s((gearbox and gearbox.currentGearIndex) and gearbox.currentGearIndex),
    s((controller and controller.getGearName) and controller.getGearName()),
    s((controller and controller.getGearPosition) and controller.getGearPosition()),
    s(controller and controller.currentGearIndex),
    automaticModes,
    defaultAutomaticMode
  ))
end
__mffb_get()
]]

  -- The actual metadata command is intentionally fire-and-forget. The GE
  -- side below also polls the authoritative GE/VLUA state after a shift.
  if queueVehicleLua(vehicleId, command) then
    log("VLUA metadata request queued for vehicle " .. tostring(vehicleId) .. " (" .. tostring(reason) .. ").")
    return true
  end
  return false
end

local function normalizeGearName(name)
  if name == nil then
    return ""
  end
  return string.upper(tostring(name))
end

local function gearNameMatchesExpected(actualName, expectedName)
  local actual = normalizeGearName(actualName)
  local expected = normalizeGearName(expectedName)

  if actual == expected then
    return true
  end

  -- Accept the common automatic-mode aliases used by BeamNG.
  local aliases = {
    ["PARK"] = "P",
    ["REVERSE"] = "R",
    ["NEUTRAL"] = "N",
    ["DRIVE"] = "D",
    ["LOW"] = "L",
  }

  return aliases[actual] == expected or aliases[expected] == actual
end

local function getGearStateFromVehicle(vehicleId)
  local vehicle = getVehicleObject(vehicleId)
  if vehicle == nil then
    return nil, nil, nil
  end

  local gear = nil
  local gearIndex = nil
  local gearPosition = nil

  -- GE/VLUA objects expose Lua-side vehicle state through queueLuaCommand,
  -- so this function is deliberately conservative. If the GE electrics
  -- table is available on the object, use it; otherwise return nil and let
  -- the explicit asynchronous verification query handle it.
  pcall(function()
    if vehicle.electrics then
      gear = vehicle.electrics.gear
      gearIndex = vehicle.electrics.gearIndex
      gearPosition = vehicle.electrics.gear_A
    end
  end)

  return gear, gearIndex, gearPosition
end

local function sendStateToHelper(gear, gearIndex, position, reason)
  local safeGear = tostring(gear or "")
  local safeIndex = tostring(gearIndex or "")
  local safePosition = tostring(position or "")

  log(
    "TX STATE (" .. tostring(reason) .. "): gear="
    .. safeGear
    .. " gearIndex="
    .. safeIndex
    .. " position="
    .. safePosition
  )

  sendCommand(
    "STATE|"
    .. safeGear
    .. "|"
    .. safeIndex
    .. "|"
    .. safePosition
  )
end

local function sendVehicleProfile(reason)
  if currentVehicleId == nil then
    return
  end

  -- The existing GE metadata/profile discovery remains in the helper
  -- protocol. Trigger a metadata request so the helper keeps its current
  -- vehicle/profile resolution behavior.
  sendCommand("REQUEST_VEHICLE_METADATA")
  log("Vehicle metadata/profile synchronization requested (" .. tostring(reason) .. ").")
end

local function requestShiftVerification(expectedName, expectedIndex)
  nextShiftId = nextShiftId + 1
  pendingShift = {
    id = nextShiftId,
    vehicleId = currentVehicleId,
    expectedName = expectedName,
    expectedIndex = expectedIndex,
    started = os.clock(),
    lastPoll = -1000,
    attempts = 0,
  }

  log(
    "Shift verification started: expected="
    .. tostring(expectedName)
    .. " index="
    .. tostring(expectedIndex)
  )
end

local function queueGearShift(zoneName, gearIndex)
  if currentVehicleId == nil then
    log("Ignoring gear command because no player vehicle is active.")
    return false
  end

  local expectedName = tostring(zoneName or "")
  local index = tonumber(gearIndex)

  if index == nil then
    log("Ignoring gear command with invalid index: " .. tostring(gearIndex))
    return false
  end

  index = math.floor(index)

  log(
    "FFB zone gear command queued: "
    .. expectedName
    .. " -> index "
    .. tostring(index)
  )

  -- BeamNG documents shiftToGearIndex(index) as a public vehicle-controller
  -- function for automatic/manual/sequential gearbox controllers.
  -- Calling the controller in VLUA is important: changing the helper's FFB
  -- zone alone must never be treated as a successful transmission shift.
  --
  -- The VLUA command also reports the authoritative post-command gearbox
  -- state back into GELUA via obj:queueGameEngineLua(). This lets GELUA
  -- distinguish "command was queued" from "the gearbox actually changed".
  local command = string.format([[
local expectedIndex = %d
local expectedName = %q
local vehicleId = %d

local function s(x)
  if x == nil then return "" end
  return tostring(x)
end

local function report()
  local gear = ""
  local gearIndex = ""
  local gearPosition = ""
  local controllerGear = ""
  local controllerIndex = ""
  local controllerPosition = ""

  pcall(function()
    if electrics then
      gear = s(electrics.gear)
      gearIndex = s(electrics.gearIndex)
      gearPosition = s(electrics.gear_A)
    end
  end)

  pcall(function()
    if controller then
      if controller.getGearName then controllerGear = s(controller.getGearName()) end
      if controller.currentGearIndex ~= nil then controllerIndex = s(controller.currentGearIndex) end
      if controller.getGearPosition then controllerPosition = s(controller.getGearPosition()) end
    end
  end)

  if obj and obj.queueGameEngineLua then
    obj:queueGameEngineLua(string.format(
      "if extensions and extensions.multiffbjoy and extensions.multiffbjoy.onGearState then extensions.multiffbjoy.onGearState(%d,%q,%q,%q,%q,%q,%q,%q,%q) end",
      vehicleId,
      expectedName,
      tostring(expectedIndex),
      gear,
      gearIndex,
      gearPosition,
      controllerGear,
      controllerIndex,
      controllerPosition
    ))
  end
end

if controller and controller.shiftToGearIndex then
  local ok, err = pcall(function()
    controller.shiftToGearIndex(expectedIndex)
  end)

  if not ok then
    print("[MultiFFBJoy/VLUA] shiftToGearIndex ERROR: " .. tostring(err))
  end
else
  print("[MultiFFBJoy/VLUA] ERROR: controller.shiftToGearIndex unavailable")
end

report()
]], index, expectedName, currentVehicleId)

  if not queueVehicleLua(currentVehicleId, command) then
    return false
  end

  requestShiftVerification(expectedName, index)
  return true
end

local function pollShiftVerification()
  if pendingShift == nil then
    return
  end

  if currentVehicleId ~= pendingShift.vehicleId then
    log("Shift verification cancelled: player vehicle changed.")
    pendingShift = nil
    return
  end

  local now = os.clock()
  if now - pendingShift.started > SHIFT_VERIFY_TIMEOUT then
    log(
      "SHIFT VERIFY FAILED: expected="
      .. tostring(pendingShift.expectedName)
      .. " index="
      .. tostring(pendingShift.expectedIndex)
      .. " after "
      .. tostring(pendingShift.attempts)
      .. " poll(s)."
    )

    -- Send one fresh authoritative metadata query so the log shows what
    -- the gearbox actually reports after the failed command.
    requestVehicleMetadata("shift verify timeout")
    pendingShift = nil
    return
  end

  if now - pendingShift.lastPoll < SHIFT_VERIFY_INTERVAL then
    return
  end

  pendingShift.lastPoll = now
  pendingShift.attempts = pendingShift.attempts + 1

  -- Ask the vehicle VM to report the authoritative state back to GELUA.
  -- We intentionally do not infer success from the requested index.
  local command = string.format([[
local vehicleId = %d
local expectedName = %q
local expectedIndex = %d

local function s(x)
  if x == nil then return "" end
  return tostring(x)
end

local gear = ""
local gearIndex = ""
local gearPosition = ""
local controllerGear = ""
local controllerIndex = ""
local controllerPosition = ""

pcall(function()
  if electrics then
    gear = s(electrics.gear)
    gearIndex = s(electrics.gearIndex)
    gearPosition = s(electrics.gear_A)
  end
end)

pcall(function()
  if controller then
    if controller.getGearName then controllerGear = s(controller.getGearName()) end
    if controller.currentGearIndex ~= nil then controllerIndex = s(controller.currentGearIndex) end
    if controller.getGearPosition then controllerPosition = s(controller.getGearPosition()) end
  end
end)

if obj and obj.queueGameEngineLua then
  obj:queueGameEngineLua(string.format(
    "if extensions and extensions.multiffbjoy and extensions.multiffbjoy.onGearState then extensions.multiffbjoy.onGearState(%d,%q,%q,%q,%q,%q,%q,%q,%q) end",
    vehicleId,
    expectedName,
    tostring(expectedIndex),
    gear,
    gearIndex,
    gearPosition,
    controllerGear,
    controllerIndex,
    controllerPosition
  ))
end
]], pendingShift.vehicleId, pendingShift.expectedName, pendingShift.expectedIndex)

  queueVehicleLua(pendingShift.vehicleId, command)

end

local function onGearState(
  vehicleId,
  expectedName,
  expectedIndex,
  gear,
  gearIndex,
  gearPosition,
  controllerGear,
  controllerIndex,
  controllerPosition
)
  if vehicleId ~= currentVehicleId then
    return
  end

  log(
    "SHIFT VERIFY: expected="
    .. tostring(expectedName)
    .. " index="
    .. tostring(expectedIndex)
    .. " actualGear="
    .. tostring(gear)
    .. " actualGearIndex="
    .. tostring(gearIndex)
    .. " actualGear_A="
    .. tostring(gearPosition)
    .. " controllerGear="
    .. tostring(controllerGear)
    .. " controllerIndex="
    .. tostring(controllerIndex)
    .. " controllerPosition="
    .. tostring(controllerPosition)
  )

  local expected = tonumber(expectedIndex)
  local actual = tonumber(gearIndex)
  local actualController = tonumber(controllerIndex)

  local indexMatches =
    expected ~= nil
    and (
      actual ~= nil and actual == expected
      or actualController ~= nil and actualController == expected
    )

  local nameMatches =
    gearNameMatchesExpected(gear, expectedName)
    or gearNameMatchesExpected(controllerGear, expectedName)

  if indexMatches or nameMatches then
    log(
      "SHIFT CONFIRMED: "
      .. tostring(expectedName)
      .. " index="
      .. tostring(expectedIndex)
    )

    if pendingShift ~= nil
      and pendingShift.vehicleId == vehicleId
      and tonumber(pendingShift.expectedIndex) == expected
    then
      pendingShift = nil
    end
  elseif pendingShift ~= nil
    and pendingShift.vehicleId == vehicleId
  then
    log(
      "SHIFT NOT YET CONFIRMED: expected="
      .. tostring(pendingShift.expectedName)
      .. " index="
      .. tostring(pendingShift.expectedIndex)
    )
  end

  lastVerifiedGearIndex = actual or actualController
  lastVerifiedGearName = gear ~= "" and gear or controllerGear
end

local function handleVehicleChange(vehicleId)
  log(
    "Player vehicle changed: "
    .. tostring(currentVehicleId)
    .. " -> "
    .. tostring(vehicleId)
  )

  currentVehicleId = vehicleId
  pendingShift = nil
  lastVerifiedGearIndex = nil
  lastVerifiedGearName = nil

  if vehicleId == nil or vehicleId <= 0 then
    log("No active player vehicle.")
    return
  end

  -- Vehicle changes are NOT a reason to skip the startup reacquire logic.
  -- The helper owns the DirectInput reacquisition policy.
  requestReacquire()

  -- Give the vehicle VM a little time to finish loading its controller.
  core_jobsystem.create(function(job)
    job.sleep(0.25)
    if initialized and currentVehicleId == vehicleId then
      requestVehicleMetadata("vehicle change")
      sendVehicleProfile("vehicle change")
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
  log("onVehicleSpawned: " .. tostring(vehicleId))

  local playerId = getPlayerVehicleId()
  if playerId == vehicleId then
    handleVehicleChange(vehicleId)
  end
end

local function onPlayerVehicleChanged(vehicleId)
  log("onPlayerVehicleChanged: " .. tostring(vehicleId))
  handleVehicleChange(vehicleId)
end

local function pollUdp()
  if udp == nil then
    return
  end

  for _ = 1, 16 do
    local ok, data = pcall(function()
      return udp:receivefrom()
    end)

    if not ok or data == nil then
      break
    end

    log("RX: " .. tostring(data))

    local operation, a, b =
      tostring(data):match("^([%w_]+)|([^|]*)|([^|]*)$")

    if operation == "SHIFT" then
      local zoneName = a
      local index = tonumber(b)

      if index == nil then
        log("Invalid SHIFT index: " .. tostring(b))
      else
        queueGearShift(zoneName, index)
      end
    elseif tostring(data):match("^SHIFT|") then
      local parts = {}
      for part in tostring(data):gmatch("[^|]+") do
        parts[#parts + 1] = part
      end

      local zoneName = parts[2]
      local index = tonumber(parts[3])

      if zoneName == nil or index == nil then
        log("Malformed SHIFT command: " .. tostring(data))
      else
        queueGearShift(zoneName, index)
      end
    elseif data == "PING" then
      sendCommand("PONG")
    elseif data == "REQUEST_PROFILE" then
      sendVehicleProfile("helper request")
    elseif data == "REQUEST_METADATA" then
      requestVehicleMetadata("helper request")
    end
  end
end

local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end

  if udp == nil then
    initializeUDP()
  end

  pollUdp()
  pollShiftVerification()

  -- Keep the helper informed of the active player vehicle even if a game
  -- event was missed during startup.
  local playerId = getPlayerVehicleId()
  if playerId ~= nil and playerId ~= currentVehicleId then
    handleVehicleChange(playerId)
  end
end

local function onExtensionLoaded()
  if initialized then
    return
  end

  initialized = true
  log("Extension initialized.")
  initializeUDP()

  -- Startup reacquisition is deliberately retained. The game can load
  -- after the helper, and BeamNG can take the DirectInput interface during
  -- startup. The helper must be allowed to reacquire it.
  core_jobsystem.create(function(job)
    local elapsed = 0

    while initialized and elapsed < 30 do
      local vehicleId = getPlayerVehicleId()

      if vehicleId ~= nil then
        log("Initial player vehicle detected: " .. tostring(vehicleId))
        handleVehicleChange(vehicleId)
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
  pendingShift = nil

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
M.onGearState = onGearState

return M
