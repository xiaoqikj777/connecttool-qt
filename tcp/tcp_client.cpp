#include "tcp_client.h"
#include <iostream>

TCPClient::TCPClient(const std::string& host, int port) : host_(host), port_(port), connected_(false), socket_(std::make_shared<tcp::socket>(io_context_)), work_(boost::asio::make_work_guard(io_context_)), buffer_(1024) {}

TCPClient::~TCPClient() { disconnect(); }

bool TCPClient::connect() {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));
        boost::asio::connect(*socket_, endpoints);
        connected_ = true;
        clientThread_ = std::thread([this]() { 
            std::cout << "Client thread started" << std::endl;
            io_context_.run(); 
            std::cout << "Client thread stopped" << std::endl;
        });
        start_read();
        std::cout << "Connected to " << host_ << ":" << port_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect: " << e.what() << std::endl;
        return false;
    }
}

void TCPClient::disconnect() {
    connected_ = false;
    io_context_.stop();
    if (clientThread_.joinable()) {
        clientThread_.join();
    }
    if (socket_->is_open()) {
        socket_->close();
    }
}

void TCPClient::send(const std::string& message) {
    send(message.c_str(), message.size());
}

void TCPClient::send(const char* data, size_t size) {
    if (!connected_) return;
    // std::cout << "Sending " << size << " bytes" << std::endl;
    boost::asio::async_write(*socket_, boost::asio::buffer(data, size), [this](const boost::system::error_code& error, std::size_t) {
        if (error) {
            std::cerr << "Send failed: " << error.message() << std::endl;
            disconnect();
        }
    });
}

void TCPClient::setReceiveCallback(std::function<void(const std::string&)> callback) {
    receiveCallback_ = callback;
}

void TCPClient::setReceiveCallback(std::function<void(const char*, size_t)> callback) {
    receiveCallbackBytes_ = callback;
}

void TCPClient::start_read() {
    socket_->async_read_some(boost::asio::buffer(buffer_), [this](const boost::system::error_code& error, std::size_t bytes_transferred) {
        handle_read(error, bytes_transferred);
    });
}

void TCPClient::handle_read(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) {
        // std::cout << "Received " << bytes_transferred << " bytes" << std::endl;
        if (receiveCallbackBytes_) {
            receiveCallbackBytes_(buffer_.data(), bytes_transferred);
        } else if (receiveCallback_) {
            std::string message(buffer_.data(), bytes_transferred);
            receiveCallback_(message);
        }
        start_read();
    } else {
        if (error == boost::asio::error::eof) {
            std::cout << "Connection closed by peer" << std::endl;
        } else {
            std::cerr << "Read failed: " << error.message() << std::endl;
        }
        disconnect();
    }
}