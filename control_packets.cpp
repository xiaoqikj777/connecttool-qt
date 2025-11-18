#include "control_packets.h"
#include <iostream>
#include <string_view>

// 假设需要访问全局变量，但为了简单，这里只打印
// 如果需要，可以传递引用或使用全局

void handleControlPacket(const char* data, size_t size, HSteamNetConnection conn) {
    std::string_view packetData(data, size);
    std::cout << "Received control packet: " << packetData << " from connection " << conn << std::endl;
    // 这里添加处理逻辑，例如解析JSON或命令
    // 例如，如果data是"ping"，回复"pong"
    if (packetData == "ping") {
        // 发送回复，但需要接口
        // 暂时只打印
        std::cout << "Responding to ping" << std::endl;
    }
    // 可以扩展为更多控制命令
}