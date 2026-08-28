#include "common.h"

#include <sstream>

namespace MultiFFBJoy
{
namespace
{
bool g_wsaStarted = false;

void TouchCommandWatchdog()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.lastCommand = std::chrono::steady_clock::now();
}

void ProcessCommand(const std::string& command)
{
    std::istringstream stream(command);
    std::string operation;
    if (command.rfind("VEHICLE|", 0) == 0)
        operation = "VEHICLE";
    else
        stream >> operation;
    if (operation.empty())
        return;

    if (operation == "PING")
    {
        Log("RX: PING");
        return;
    }
    if (operation == "VEHICLE")
    {
        // VEHICLE|game|vehicleType|vehicle|configuration|transmission|gearLayout
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= command.size())
        {
            const size_t end = command.find('|', start);
            if (end == std::string::npos)
            {
                fields.push_back(command.substr(start));
                break;
            }
            fields.push_back(command.substr(start, end - start));
            start = end + 1;
        }

        if (fields.size() < 7 || fields[0] != "VEHICLE")
        {
            Log("RX: malformed VEHICLE command.");
            return;
        }

        VehicleProfileRequest request;
        request.game = fields[1];
        request.vehicleType = fields[2];
        request.vehicle = fields[3];
        request.configuration = fields[4];
        request.transmission = fields[5];
        request.gearLayout = fields[6];

        Logf("RX: VEHICLE game=\"%s\" type=\"%s\" vehicle=\"%s\" config=\"%s\" transmission=\"%s\" layout=\"%s\".",
             request.game.c_str(), request.vehicleType.c_str(),
             request.vehicle.c_str(), request.configuration.c_str(),
             request.transmission.c_str(), request.gearLayout.c_str());

        if (!LoadResolvedVehicleProfile(request))
            Log("VEHICLE profile resolution failed.");
        return;
    }
    if (operation == "PROFILE")
    {
        std::string presetName;
        if (!(stream >> presetName))
        {
            Log("RX: malformed PROFILE command.");
            return;
        }
        if (presetName.size() < 4 ||
            _stricmp(presetName.c_str() + presetName.size() - 4, ".fff") != 0)
            presetName += ".fff";

        const auto path = GetApplicationDirectory() / "forcefields" / presetName;
        Logf("RX: PROFILE %s", presetName.c_str());
        if (!std::filesystem::exists(path))
        {
            Logf("PROFILE failed: preset does not exist: %s", path.string().c_str());
            return;
        }
        ClearForceFieldPreset();
        if (LoadForceFieldPreset(path))
        {
            StartPresetTest();
            Logf("PROFILE activated: %s", path.string().c_str());
        }
        return;
    }
    if (operation == "STOP")
    {
        Log("RX: STOP");
        ClearForceFieldPreset();
        return;
    }
    if (operation == "REACQUIRE")
    {
        Log("RX: REACQUIRE");
        Log(ReacquireFFBDevice() ? "REACQUIRE completed successfully." :
                                   "REACQUIRE failed.");
        return;
    }
    if (operation == "CENTER")
    {
        Log("RX: CENTER");
        StopTestConstantForce();
        if (!EnsureFFBDeviceReady() || !SetSpringStrength(1.0f))
            Log("CENTER failed.");
        else
            Log("CENTER completed successfully.");
        return;
    }
    if (operation == "SPRING")
    {
        float strength = 0.0f;
        if (!(stream >> strength))
        {
            Log("RX: malformed SPRING command.");
            return;
        }
        if (!EnsureFFBDeviceReady() || !SetSpringStrength(strength))
            Log("SPRING failed.");
        return;
    }
    if (operation == "TEST_FFB")
    {
        LONG x = 0, y = 0;
        if (!(stream >> x >> y))
        {
            Log("RX: malformed TEST_FFB command.");
            return;
        }
        if (!EnsureFFBDeviceReady() || !SetTestConstantForce(x, y))
            Log("TEST_FFB failed.");
        return;
    }
    Logf("RX: unknown command: %s", operation.c_str());
}

void NetworkThread()
{
    bool timeoutStopIssued = false;
    while (g_running && g_networkRunning)
    {
        char buffer[1024]{};
        sockaddr_in sender{};
        int senderLength = sizeof(sender);
        const int received = recvfrom(
            g_socket, buffer, sizeof(buffer) - 1, 0,
            reinterpret_cast<sockaddr*>(&sender), &senderLength);

        if (received > 0)
        {
            buffer[received] = '\0';
            TouchCommandWatchdog();
            timeoutStopIssued = false;
            ProcessCommand(buffer);
        }
        else if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAEINTR &&
                g_running && g_networkRunning)
                Logf("recvfrom() failed: %d", error);
        }

        bool timedOut = false;
        bool persistent = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_state.lastCommand);
            timedOut = elapsed.count() > COMMAND_TIMEOUT_MS;
            persistent = g_state.springPersistent;
        }
        if (timedOut && !persistent && !timeoutStopIssued)
        {
            StopSpring();
            timeoutStopIssued = true;
        }
        else if (!timedOut || persistent)
        {
            timeoutStopIssued = false;
        }
    }
}
} // namespace

bool StartUdpServer()
{
    if (g_networkRunning)
        return true;

    WSADATA wsaData{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        Logf("WSAStartup failed: %d", wsaResult);
        return false;
    }
    g_wsaStarted = true;

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        Logf("socket() failed: %d", WSAGetLastError());
        WSACleanup();
        g_wsaStarted = false;
        return false;
    }

    DWORD timeout = SOCKET_TIMEOUT_MS;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(UDP_PORT));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(g_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        Logf("bind() failed: %d", WSAGetLastError());
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        WSACleanup();
        g_wsaStarted = false;
        return false;
    }

    g_networkRunning = true;
    g_state.lastCommand = std::chrono::steady_clock::now();
    Logf("UDP server listening on 127.0.0.1:%d.", UDP_PORT);
    try
    {
        g_networkThread = std::thread(NetworkThread);
    }
    catch (const std::exception& error)
    {
        Logf("Failed to start UDP thread: %s", error.what());
        g_networkRunning = false;
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        WSACleanup();
        g_wsaStarted = false;
        return false;
    }
    return true;
}

void StopUdpServer()
{
    g_networkRunning = false;
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_networkThread.joinable())
        g_networkThread.join();
    if (g_wsaStarted)
    {
        WSACleanup();
        g_wsaStarted = false;
    }
}

void SendUdpCommand(const std::string& command)
{
    if (g_socket == INVALID_SOCKET)
    {
        Log("TX failed: UDP socket is not available.");
        return;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<u_short>(UDP_PORT));
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    const int result = sendto(
        g_socket, command.c_str(), static_cast<int>(command.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    if (result == SOCKET_ERROR)
        Logf("TX failed: %d", WSAGetLastError());
    else
        Logf("TX: %s", command.c_str());
}
} // namespace MultiFFBJoy
