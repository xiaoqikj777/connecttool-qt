#ifndef STEAM_MESSAGE_HANDLER_H
#define STEAM_MESSAGE_HANDLER_H

#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <boost/asio.hpp>
#include <steamnetworkingtypes.h>
#include "tcp_server.h"
#include "tcp/tcp_client.h"
#include "control_packets.h"

class SteamMessageHandler {
public:
    SteamMessageHandler(boost::asio::io_context& io_context, ISteamNetworkingSockets* interface, std::vector<HSteamNetConnection>& connections, std::map<HSteamNetConnection, TCPClient*>& clientMap, std::mutex& clientMutex, std::unique_ptr<TCPServer>& server, bool& g_isHost, int& localPort);
    ~SteamMessageHandler();

    void start();
    void stop();

private:
    void run();
    void pollMessages();

    boost::asio::io_context& io_context_;
    ISteamNetworkingSockets* m_pInterface_;
    std::vector<HSteamNetConnection>& connections_;
    std::map<HSteamNetConnection, TCPClient*>& clientMap_;
    std::mutex& clientMutex_;
    std::unique_ptr<TCPServer>& server_;
    bool& g_isHost_;
    int& localPort_;

    std::thread thread_;
    bool running_;
};

#endif // STEAM_MESSAGE_HANDLER_H