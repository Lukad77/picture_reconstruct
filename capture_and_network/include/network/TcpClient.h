#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <string>
#include <cstddef>
#include <cstdint>

class TcpClient {
public:
    TcpClient(const std::string& ip, int port);
    ~TcpClient();

    bool connectToServer();
    void closeConnection();

    bool isConnected() const;

    bool sendAll(const void* data, size_t size);
    bool recvAll(void* data, size_t size);

    void setSendTimeout(int seconds);
    void setRecvTimeout(int seconds);

private:
    std::string ip_;
    int port_;
    int socketFd_;
    bool connected_;
    int sendTimeoutSeconds_;
    int recvTimeoutSeconds_;
};

#endif