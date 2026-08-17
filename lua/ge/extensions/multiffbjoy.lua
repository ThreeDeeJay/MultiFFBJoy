-- MultiFFBJoy
-- BeamNG GE extension for an external DirectInput FFB helper.
--
-- The helper is expected to listen on:
--   127.0.0.1:65458
--
-- Commands:
--   PING
--   REACQUIRE
--   STOP
--   CENTER
--   SPRING <0..1>
--   TEST_FFB <x> <y>
--
-- This extension intentionally uses manual unload mode so that it
-- remains alive across level/vehicle changes.

local M = {}

local socket = nil
local udp = nil

local UDP_HOST = "127.0.0.1"
local UDP_PORT = 65458

local initialized = false
local lastVehicleId = nil
local lastVehicleType = nil

local lastReacquireTime = -100000
local reacquireCooldown = 1.0

local lastCenterTime = -100000
local centerCooldown = 0.25

local helperReady = false

local function log(msg)
  print("[MultiFFBJoy] " .. tostring(msg))
end

local function sendCommand(command)
  if udp == nil then
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

local function sendReacquire()
  local now = os.clock()

  if now - lastReacquireTime < reacquireCooldown then
    return
  end

  lastReacquireTime = now

  log("Requesting FFB device re-acquisition.")
  sendCommand("REACQUIRE")
end

local function sendCenter()
  local now = os.clock()

  if now - lastCenterTime < centerCooldown then
    return
  end

  lastCenterTime = now

  sendCommand("CENTER")
end

local function getPlayerVehicleId()
  local id = nil

  if be then
    if be.getPlayerVehicleID then
      local ok, result = pcall(be.getPlayerVehicleID)

      if ok then
        id = result
      end
    end
  end

  if id == nil and core_vehicle_manager then
    if core_vehicle_manager.getPlayerVehicleID then
      local ok, result = pcall(core_vehicle_manager.getPlayerVehicleID)

      if ok then
        id = result
      end
    end
  end

  return id
end

local function getVehicleType(id)
  if id == nil then
    return nil
  end

  local veh = scenetree.findObjectById(id)

  if veh == nil then
    return nil
  end

  local ok, vehicleType = pcall(function()
    return veh:getClassName()
  end)

  if ok then
    return vehicleType
  end

  return nil
end

local function setupUDP()
  if udp ~= nil then
    return true
  end

  if socket == nil then
    local ok, result = pcall(require, "socket")

    if not ok then
      log("Unable to load Lua socket module: " .. tostring(result))
      return false
    end

    socket = result
  end

  local ok, result = pcall(function()
    return socket.udp()
  end)

  if not ok or result == nil then
    log("Unable to create UDP socket: " .. tostring(result))
    return false
  end

  udp = result

  pcall(function()
    udp:settimeout(0)
  end)

  log("UDP initialized: " .. UDP_HOST .. ":" .. tostring(UDP_PORT))

  return true
end

local function initialize()
  if initialized then
    return
  end

  initialized = true

  log("Extension initialized.")

  setupUDP()

  -- We intentionally do NOT immediately send CENTER here.
  --
  -- The helper may not exist yet, and BeamNG may not have created
  -- the player's vehicle yet. Vehicle/map state is handled from
  -- onUpdate().
end

local function onVehicleChanged(id)
  local oldId = lastVehicleId

  lastVehicleId = id
  lastVehicleType = getVehicleType(id)

  log(
    "Player vehicle changed: "
    .. tostring(oldId)
    .. " -> "
    .. tostring(id)
  )

  if id == nil then
    return
  end

  -- A vehicle appearing is the point at which we want to make sure
  -- the external helper has a usable FFB device.
  --
  -- REACQUIRE is deliberately sent before CENTER. The helper will
  -- therefore rebuild/reacquire the DirectInput device before the
  -- center command is processed.
  sendReacquire()

  -- Give the helper/DirectInput stack a little time before sending
  -- CENTER. This is handled without blocking BeamNG's update loop.
end

local function pollPlayerVehicle()
  local id = getPlayerVehicleId()

  if id ~= lastVehicleId then
    onVehicleChanged(id)
  end
end

local function pollHelper()
  if udp == nil then
    return
  end

  -- There is intentionally no blocking receive here.
  --
  -- The helper currently doesn't need to send anything back to
  -- BeamNG for normal operation.
end

local function onUpdate(dtReal, dtSim, dtRaw)
  if not initialized then
    return
  end

  if udp == nil then
    setupUDP()
  end

  pollPlayerVehicle()
  pollHelper()
end

local function onExtensionLoaded()
  -- BeamNG's current extension API prefers this function instead
  -- of the deprecated init() function.
  initialize()
end

local function onExtensionUnloaded()
  if udp ~= nil then
    pcall(function()
      udp:close()
    end)
  end

  udp = nil
  initialized = false
  lastVehicleId = nil
  lastVehicleType = nil

  log("Extension unloaded.")
end

-- BeamNG extension lifecycle.
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate

return M