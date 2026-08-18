#include "common.h"
#include <sstream>
namespace MultiFFBJoy
{
    static void ProcessCommand(const std::string& command)
    {
        std::istringstream stream(command);
        std::string operation;
        stream >> operation;
        if (operation == "PING")
        {
            Log("RX: PING");
            return;
        }
        if (operation == "STOP")
        {
            Log("RX: STOP");
            StopSpring();
            StopTestConstantForce();
            return;
        }
        if (operation == "REACQUIRE")
        {
            Log("RX: REACQUIRE");
            if (ReacquireFFBDevice())
                Log("REACQUIRE completed successfully.");
            else
                Log("REACQUIRE failed.");
            return;
        }
        if (operation == "CENTER")
        {
            Log("RX: CENTER");
            if (!EnsureFFBDeviceReady())
            {
                Log("CENTER ignored: FFB device unavailable.");
                return;
            }
            if (!SetSpringStrength(1.0f))
            {
                Log("CENTER failed: could not start spring.");
            }
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
            Logf("RX: SPRING %.3f", strength);
            if (!EnsureFFBDeviceReady())
            {
                Log("SPRING ignored: FFB device unavailable.");
                return;
            }
            if (!SetSpringStrength(strength))
            {
                Log("SPRING failed.");
            }
            return;
        }
        if (operation == "TEST_FFB")
        {
            LONG x = 0;
            LONG y = 0;
            if (!(stream >> x >> y))
            {
                Log("RX: malformed TEST_FFB command.");
                return;
            }
            x = std::clamp<LONG>(
                x,
                -DI_FFNOMINALMAX,
                DI_FFNOMINALMAX);
            y = std::clamp<LONG>(
                y,
                -DI_FFNOMINALMAX,
                DI_FFNOMINALMAX);
            Logf(
                "RX: TEST_FFB %ld %ld",
                x,
                y);
            if (x == 0 && y == 0)
            {
                StopTestConstantForce();
                return;
            }
            if (!EnsureFFBDeviceReady())
            {
                Log("TEST_FFB ignored: FFB device unavailable.");
                return;
            }
            if (!SetTestConstantForce(x, y))
            {
                Log("TEST_FFB failed.");
            }
            return;
        }
        Logf(
            "RX: unknown command: %s",
            operation.c_str());
    }
    static void NetworkThread()
    {
        while (g_running && g_networkRunning)
        {
            char buffer[1024]{};
            sockaddr_in sender{};
            int senderLength = sizeof(sender);
            const int received = recvfrom(
                g_socket,
                buffer,
                sizeof(buffer) - 1,
                0,
                reinterpret_cast<sockaddr*>(&sender),
                &senderLength);
            if (received > 0)
            {
                buffer[received] = '\0';
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_state.lastCommand =
                    std::chrono::steady_clock::now();
                }
                ProcessCommand(std::string(buffer));
            }
            else if (received == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error != WSAETIMEDOUT &&
                    error != WSAEINTR &&
                    g_running &&
                    g_networkRunning)
                {
                    Logf(
                        "recvfrom() failed: %d",
                        error);
                }
            }
            bool timedOut = false;
            bool springPersistent = false;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto now =
                std::chrono::steady_clock::now();
                const auto elapsed =
                std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    now - g_state.lastCommand);
                timedOut =
                elapsed.count() >
                COMMAND_TIMEOUT_MS;
                springPersistent =
                g_state.springPersistent;
            }
            if (timedOut && !springPersistent)
            {
                StopSpring();
            }
        }
    }
    bool StartUdpServer()
    {
        if (g_networkRunning)
            return true;
        WSADATA wsaData{};
        const int result = WSAStartup(
            MAKEWORD(2, 2),
            &wsaData);
        if (result != 0)
        {
            Logf(
                "WSAStartup failed: %d",
                result);
            return false;
        }
        g_socket = socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP);
        if (g_socket == INVALID_SOCKET)
        {
            Logf(
                "socket() failed: %d",
                WSAGetLastError());
            WSACleanup();
            return false;
        }
        DWORD receiveTimeout = SOCKET_TIMEOUT_MS;
        setsockopt(
            g_socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receiveTimeout),
            sizeof(receiveTimeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port =
        htons(static_cast<u_short>(UDP_PORT));
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr);
        if (bind(
            g_socket,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) != 0)
        {
            Logf(
                "bind() failed: %d",
                WSAGetLastError());
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
        Logf(
            "UDP server listening on 127.0.0.1:%d.",
            UDP_PORT);
        g_networkRunning = true;
        g_running = true;
        g_networkThread = std::thread(NetworkThread);
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
        WSACleanup();
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
        destination.sin_port =
        htons(static_cast<u_short>(UDP_PORT));
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &destination.sin_addr);
        const int result = sendto(
            g_socket,
            command.c_str(),
            static_cast<int>(command.size()),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (result == SOCKET_ERROR)
        {
            Logf(
                "TX failed: %d",
                WSAGetLastError());
            return;
        }
        Logf(
            "TX: %s",
            command.c_str());
    }
} // namespace MultiFFBJoy
