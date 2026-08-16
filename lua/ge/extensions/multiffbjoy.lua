local M = {}
local UDP_IP = "127.0.0.1"
local UDP_PORT = 65458
local enabled = true
local lastVehicleId = nil
local lastUpdateTime = 0
local function log(message)
  print("[MultiFFBJoy] " .. message)
end
local function sendCommand(command)
  log("TX: " .. command)
-- UDP implementation will be added here.
end
local function sendCenter()
  sendCommand("CENTER")
end
local function sendStop()
  sendCommand("STOP")
end
local function onInit()
  log("Extension initialized.")
end
local function onReset()
  log("Vehicle reset.")
  sendStop()
end
local function onUpdate(dt)
  if not enabled then
    return
  end
  local vehicleId =
  be:getPlayerVehicleID(0)
  if vehicleId == nil then
    if lastVehicleId ~= nil then
      log("No active player vehicle.")
      sendStop()
      lastVehicleId = nil
    end
    return
  end
  if vehicleId ~= lastVehicleId then
    log(
      "Player vehicle changed: " ..
      tostring(vehicleId))
    lastVehicleId =
    vehicleId
    sendCenter()
  end
  lastUpdateTime =
  lastUpdateTime + dt
end
M.onInit = onInit
M.onReset = onReset
M.onUpdate = onUpdate
M.sendCenter = sendCenter
M.sendStop = sendStop
return M