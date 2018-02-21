#include <iostream>
#include <string>
#include <mutex>
#include <condition_variable>

#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/http/HttpService.h>
#include <brynet/net/http/HttpFormat.h>
#include <brynet/net/http/WebSocketFormat.h>

static const char DEFAULT_HOST[] = "127.0.0.1";
static const int DEFAULT_PORT = 8080;

static const char* resolveIpPort(char* arg, int* port)
{
    if (arg == nullptr)
    {
        *port = DEFAULT_PORT;
        return DEFAULT_HOST;
    }
    
    char* colon = strchr(arg, ':');
    if (colon == nullptr)
    {
        *port = DEFAULT_PORT;
        return arg;
    }
    
    if (':' == *arg)
    {
        *port = std::atoi(arg + 1);
        return DEFAULT_HOST;
    }
    
    *(colon++) = '\0';
    *port = std::atoi(colon);
    
    return arg;
}

int main(int argc, char* argv[])
{
    int port;
    const char* host = resolveIpPort(argc > 1 ? argv[1] : nullptr, &port);
    const char* uri = argc > 2 ? argv[2] : "/";
    
    auto service = std::make_shared<brynet::net::WrapTcpService>();
    service->startWorkThread(2);
    
    //fprintf(stdout, "Connecting to http://%s:%d%s\n", host, port, uri);
    
    sock fd = brynet::net::base::Connect(false, host, port);
    if (fd == SOCKET_ERROR)
    {
        fprintf(stderr, "Could not connect to %s:%d\n", host, port);
        return 1;
    }
    
    auto socket = brynet::net::TcpSocket::Create(fd, false);
    service->addSession(std::move(socket), [uri](const brynet::net::TCPSession::PTR& tcpSession) {
        //fprintf(stdout, "Connected.\n");
        brynet::net::HttpService::setup(tcpSession, [uri](const brynet::net::HttpSession::PTR& httpSession) {
            httpSession->setHttpCallback([](const brynet::net::HTTPParser& httpParser, const brynet::net::HttpSession::PTR& session) {
                //http response handle
                std::cout << httpParser.getBody() << std::endl;
                exit(0);
            });
            
            brynet::net::HttpRequest req;
            req.setMethod(brynet::net::HttpRequest::HTTP_METHOD::HTTP_METHOD_GET);
            req.setUrl(uri);
            
            std::string payload = req.getResult();
            httpSession->send(payload.c_str(), payload.size());
        });
    }, false, nullptr, 1024 * 1024, false);
    
    std::cin.get();
    //brynet::net::EventLoop mainLoop;
    //while (true)
    //{
    //    mainLoop.loop(1000);
    //}
    
    return 0;
}
