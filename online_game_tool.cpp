#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <algorithm>
#include <cstring>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>
#include <isteamnetworkingutils.h>
#include <steamnetworkingtypes.h>
#include <boost/asio.hpp>
#include <memory>
#include "tcp_server.h"
#include "tcp/tcp_client.h"
#include "steam_message_handler.h"

using boost::asio::ip::tcp;

// Callback class for Steam Friends
class SteamFriendsCallbacks {
public:
    SteamFriendsCallbacks() {}
    STEAM_CALLBACK(SteamFriendsCallbacks, OnGameRichPresenceJoinRequested, GameRichPresenceJoinRequested_t);
};

// Global variables for Steam Networking
HSteamNetConnection g_hConnection = k_HSteamNetConnection_Invalid;  
bool g_isConnected = false;
HSteamListenSocket hListenSock = k_HSteamListenSocket_Invalid;
ISteamNetworkingSockets* m_pInterface = nullptr;
bool forwarding = false;
std::unique_ptr<TCPServer> server;

// Connection config for improved P2P reliability
SteamNetworkingConfigValue_t g_connectionConfig[2];
int g_retryCount = 0;
const int MAX_RETRIES = 3;
CSteamID g_hostSteamID;
int g_currentVirtualPort = 0;

// New variables for multiple connections and TCP clients
std::vector<HSteamNetConnection> connections;
std::map<HSteamNetConnection, TCPClient*> clientMap;
std::mutex clientMutex;
int localPort = 0;
bool g_isHost = false;
bool g_isClient = false;

// User info structure
struct UserInfo {
    CSteamID steamID;
    std::string name;
    int ping;
    bool isRelay;
};
std::map<HSteamNetConnection, UserInfo> userMap;

// Callback function for connection status changes
void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
    std::lock_guard<std::mutex> lock(clientMutex);
    std::cout << "Connection status changed: " << pInfo->m_info.m_eState << " for connection " << pInfo->m_hConn << std::endl;
    if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        std::cout << "Connection failed: " << pInfo->m_info.m_szEndDebug << std::endl;
    }
    if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_None && pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
    {
        // Incoming connection, accept it
        SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);
        connections.push_back(pInfo->m_hConn);
        g_hConnection = pInfo->m_hConn; // Keep for backward compatibility if needed
        g_isConnected = true;
        std::cout << "Accepted incoming connection from " << pInfo->m_info.m_identityRemote.GetSteamID().ConvertToUint64() << std::endl;
        // Add user info
        SteamNetConnectionInfo_t info;
        SteamNetConnectionRealTimeStatus_t status;
        if (m_pInterface->GetConnectionInfo(pInfo->m_hConn, &info) && m_pInterface->GetConnectionRealTimeStatus(pInfo->m_hConn, &status, 0, nullptr)) {
            UserInfo userInfo;
            userInfo.steamID = pInfo->m_info.m_identityRemote.GetSteamID();
            userInfo.name = SteamFriends()->GetFriendPersonaName(userInfo.steamID);
            userInfo.ping = status.m_nPing;
            userInfo.isRelay = (info.m_idPOPRelay != 0);
            userMap[pInfo->m_hConn] = userInfo;
            std::cout << "Incoming connection details: ping=" << status.m_nPing << "ms, relay=" << (info.m_idPOPRelay != 0 ? "yes" : "no") << std::endl;
        }
        // Removed: Create TCP Client here - now lazy connect on first message
    }
    else if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting && pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected)
    {
        // Client connected successfully
        g_isConnected = true;
        std::cout << "Connected to host" << std::endl;
        // Add user info
        SteamNetConnectionInfo_t info;
        SteamNetConnectionRealTimeStatus_t status;
        if (m_pInterface->GetConnectionInfo(pInfo->m_hConn, &info) && m_pInterface->GetConnectionRealTimeStatus(pInfo->m_hConn, &status, 0, nullptr)) {
            UserInfo userInfo;
            userInfo.steamID = pInfo->m_info.m_identityRemote.GetSteamID();
            userInfo.name = SteamFriends()->GetFriendPersonaName(userInfo.steamID);
            userInfo.ping = status.m_nPing;
            userInfo.isRelay = (info.m_idPOPRelay != 0);
            userMap[pInfo->m_hConn] = userInfo;
            std::cout << "Outgoing connection details: ping=" << status.m_nPing << "ms, relay=" << (info.m_idPOPRelay != 0 ? "yes" : "no") << std::endl;
        }
    }
    else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer || pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        // Connection closed
        g_isConnected = false;
        g_hConnection = k_HSteamNetConnection_Invalid;
        // Remove from connections
        auto it = connections.begin();
        while (it != connections.end()) {
            if (*it == pInfo->m_hConn) {
                it = connections.erase(it);
            } else {
                ++it;
            }
        }
        // Remove from userMap
        userMap.erase(pInfo->m_hConn);
        // Cleanup TCP Client
        if (clientMap.count(pInfo->m_hConn)) {
            clientMap[pInfo->m_hConn]->disconnect();
            delete clientMap[pInfo->m_hConn];
            clientMap.erase(pInfo->m_hConn);
            std::cout << "Cleaned up TCP Client for connection" << std::endl;
        }
        std::cout << "Connection closed" << std::endl;

        // Retry connection if client and retries left
        if (g_isClient && !g_isConnected && g_retryCount < MAX_RETRIES) {
            g_retryCount++;
            g_currentVirtualPort++;
            SteamNetworkingIdentity identity;
            identity.SetSteamID(g_hostSteamID);
            HSteamNetConnection newConn = m_pInterface->ConnectP2P(identity, g_currentVirtualPort, 2, g_connectionConfig);
            if (newConn != k_HSteamNetConnection_Invalid) {
                g_hConnection = newConn;
                std::cout << "Retrying connection attempt " << g_retryCount << " with virtual port " << g_currentVirtualPort << std::endl;
            } else {
                std::cerr << "Failed to initiate retry connection" << std::endl;
            }
        }
    }
}

void SteamFriendsCallbacks::OnGameRichPresenceJoinRequested(GameRichPresenceJoinRequested_t *pCallback) {
    CSteamID hostSteamID = pCallback->m_steamIDFriend;
    if (!g_isHost && !g_isConnected) {
        g_isClient = true;
        g_hostSteamID = hostSteamID;
        g_retryCount = 0;
        g_currentVirtualPort = 0;
        SteamNetworkingIdentity identity;
        identity.SetSteamID(hostSteamID);
        g_hConnection = m_pInterface->ConnectP2P(identity, g_currentVirtualPort, 2, g_connectionConfig);
        if (g_hConnection != k_HSteamNetConnection_Invalid) {
            std::cout << "Joined game room via invite from " << hostSteamID.ConvertToUint64() << ", attempting connection with virtual port " << g_currentVirtualPort << std::endl;
            // Start TCP Server
            server = std::make_unique<TCPServer>(8888);
            if (!server->start()) {
                std::cerr << "Failed to start TCP server" << std::endl;
            }
        }
    }
}

int main() {
    // Initialize Steam API
    if (!SteamAPI_Init()) {
        std::cerr << "Failed to initialize Steam API" << std::endl;
        return 1;
    }

    // Initialize Steam Friends callbacks
    SteamFriendsCallbacks steamFriendsCallbacks;

    // Initialize Steam Networking Sockets
    SteamNetworkingUtils()->InitRelayNetworkAccess();

    // Set global callback for connection status changes
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChanged);

    m_pInterface = SteamNetworkingSockets();

    // Initialize connection config for better P2P reliability
    g_connectionConfig[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 10000); // 10 seconds initial timeout
    g_connectionConfig[1].SetInt32(k_ESteamNetworkingConfig_NagleTime, 0); // Disable Nagle for UDP

    // Initialize boost::asio io_context
    boost::asio::io_context io_context;

    // Create Steam Message Handler
    SteamMessageHandler messageHandler(io_context, m_pInterface, connections, clientMap, clientMutex, server, g_isHost, localPort);

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        SteamAPI_Shutdown();
        return -1;
    }

    // Create window
    GLFWwindow* window = glfwCreateWindow(1280, 720, "在线游戏工具", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        SteamAPI_Shutdown();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    // Load Chinese font
    io.Fonts->AddFontFromFileTTF("font.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    ImGui::StyleColorsDark();

    // Initialize ImGui backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Get friends list
    std::vector<std::pair<CSteamID, std::string>> friendsList;
    int friendCount = SteamFriends()->GetFriendCount(k_EFriendFlagAll);
    for (int i = 0; i < friendCount; ++i) {
        CSteamID friendID = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagAll);
        const char* name = SteamFriends()->GetFriendPersonaName(friendID);
        friendsList.push_back({friendID, name});
    }

    // Start message handler
    messageHandler.start();

    // Steam Networking variables
    bool isHost = false;
    bool isClient = false;
    char joinBuffer[256] = "";
    char filterBuffer[256] = "";

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Poll events
        glfwPollEvents();

        // Run Steam callbacks
        SteamAPI_RunCallbacks();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window for online game tool
        ImGui::Begin("在线游戏工具");
        if (server) {
            ImGui::Text("TCP服务器监听端口8888");
            ImGui::Text("已连接客户端: %d", server->getClientCount());
        }
        ImGui::Separator();

        if (!g_isHost && !g_isConnected) {
            if (ImGui::Button("主持游戏房间")) {
                // Create listen socket
                hListenSock = m_pInterface->CreateListenSocketP2P(0, 0, nullptr);
                if (hListenSock != k_HSteamListenSocket_Invalid) {
                    g_isHost = true;
                    std::cout << "Created listen socket for hosting game room" << std::endl;
                    // Set Rich Presence
                    std::string connectStr = std::to_string(SteamUser()->GetSteamID().ConvertToUint64());
                    SteamFriends()->SetRichPresence("connect", connectStr.c_str());
                    SteamFriends()->SetRichPresence("status", "主持游戏房间");
                    std::cout << "Hosting game room. Connection string: " << connectStr << std::endl;
                } else {
                    std::cerr << "Failed to create listen socket for hosting" << std::endl;
                }
            }
            ImGui::InputText("主机Steam ID", joinBuffer, IM_ARRAYSIZE(joinBuffer));
            if (ImGui::Button("加入游戏房间")) {
                uint64 hostID = std::stoull(joinBuffer);
                CSteamID hostSteamID(hostID);
                g_isClient = true;
                g_hostSteamID = hostSteamID;
                g_retryCount = 0;
                g_currentVirtualPort = 0;
                // Connect to host
                SteamNetworkingIdentity identity;
                identity.SetSteamID(hostSteamID);
                g_hConnection = m_pInterface->ConnectP2P(identity, g_currentVirtualPort, 2, g_connectionConfig);
                if (g_hConnection != k_HSteamNetConnection_Invalid) {
                    std::cout << "Attempting to connect to host " << hostSteamID.ConvertToUint64() << " with virtual port " << g_currentVirtualPort << std::endl;
                    // Connection initiated, wait for callback to confirm
                    std::cout << "Connecting to host..." << std::endl;
                    // Start TCP Server
                    server = std::make_unique<TCPServer>(8888);
                    if (!server->start()) {
                        std::cerr << "Failed to start TCP server" << std::endl;
                    }
                }
            }
        }
        if (g_isHost) {
            ImGui::Text("正在主持游戏房间。邀请朋友！");
            ImGui::Separator();
            ImGui::InputInt("本地端口", &localPort);
            ImGui::Separator();
            ImGui::InputText("过滤朋友", filterBuffer, IM_ARRAYSIZE(filterBuffer));
            ImGui::Text("朋友:");
            for (const auto& friendPair : friendsList) {
                std::string nameStr = friendPair.second;
                std::string filterStr(filterBuffer);
                // Convert to lowercase for case-insensitive search
                std::transform(nameStr.begin(), nameStr.end(), nameStr.begin(), ::tolower);
                std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);
                if (filterStr.empty() || nameStr.find(filterStr) != std::string::npos) {
                    ImGui::PushID(friendPair.first.ConvertToUint64());
                    if (ImGui::Button(("邀请 " + friendPair.second).c_str())) {
                        // Send invite via Steam
                        SteamFriends()->InviteUserToGame(friendPair.first, "加入我的游戏房间!");
                    }
                    ImGui::PopID();
                }
            }
        }

        ImGui::End();

        // Room status window - only show when hosting or joined
        if (g_isHost || g_isClient) {
            ImGui::Begin("房间状态");
            if (server) {
                ImGui::Text("房间内玩家: %d", server->getClientCount() + 1); // +1 for host
            }
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                ImGui::Text("连接的好友: %d", (int)connections.size());
                ImGui::Text("活跃的TCP客户端: %d", (int)clientMap.size());
            }
            ImGui::Separator();
            ImGui::Text("用户列表:");
            if (ImGui::BeginTable("UserTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("名称");
                ImGui::TableSetupColumn("延迟 (ms)");
                ImGui::TableSetupColumn("连接类型");
                ImGui::TableHeadersRow();
                {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    for (const auto& pair : userMap) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", pair.second.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", pair.second.ping);
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", pair.second.isRelay ? "中继" : "直连");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Swap buffers
        glfwSwapBuffers(window);
    }

    // Stop message handler
    messageHandler.stop();

    // Cleanup
    if (g_hConnection != k_HSteamNetConnection_Invalid) {
        m_pInterface->CloseConnection(g_hConnection, 0, nullptr, false);
    }
    if (hListenSock != k_HSteamListenSocket_Invalid) {
        m_pInterface->CloseListenSocket(hListenSock);
    }
    // Cleanup TCP Clients
    {
        std::lock_guard<std::mutex> lock(clientMutex);
        for (auto& pair : clientMap) {
            pair.second->disconnect();
            delete pair.second;
        }
        clientMap.clear();
    }
    if (server) {
        server->stop();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    SteamAPI_Shutdown();

    return 0;
}