local M = {}
local socket = require("socket")
local UDP_IP = "127.0.0.1"
local UDP_PORT = 65458
local udp = nil
local lastVehicleId = nil
local function log(message)
  print("[MultiFFBJoy] " .. message)
end
local function initUdp()
  if udp then
    return true
  end
  local ok, result = pcall(socket.udp)
  if not ok or not result then
    log("Failed to create UDP socket.")
    return false
  end
  udp = result
  log(
    string.format(
      "UDP socket initialized: %s:%d",
      UDP_IP,
      UDP_PORT
      )
    )
  return true
end
local function sendCommand(command)
  if not initUdp() then
    return false
  end
  local bytes, err =
  udp:sendto(
    command,
    UDP_IP,
    UDP_PORT
    )
  if not bytes then
    log(
      string.format(
        "UDP send failed: %s",
        tostring(err)
        )
      )
    return false
  end
  log(
    string.format(
      "TX: %s",
      command
      )
    )
  return true
end
local function shutdownUdp()
  if not udp then
    return
  end
  udp:close()
  udp = nil
  log("UDP socket closed.")
end
function M.onInit()
  log("Extension initialized.")
  initUdp()
end
function M.onVehicleSwitched(vehicleId)
  if vehicleId == nil then
    return
  end
  lastVehicleId = vehicleId
  log(
    string.format(
      "Player vehicle changed: %s",
      tostring(vehicleId)
      )
    )
  sendCommand("CENTER")
end
function M.onExtensionUnloaded()
  shutdownUdp()
end
return M