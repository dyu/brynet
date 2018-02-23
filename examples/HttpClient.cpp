#include <iostream>
#include <cstring>
#include <string>
#include <mutex>
#include <condition_variable>
#include <csignal>

#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/http/HttpService.h>
#include <brynet/net/http/HttpFormat.h>
#include <brynet/net/http/WebSocketFormat.h>

struct Signal
{
    sig_atomic_t count;
    std::mutex mutex;
    std::condition_variable cv;
    
    Signal(int val = 0) : count(val) {}
    Signal& set(int val)
    {
        count = val;
        return *this;
    }
    bool notify()
    {
        bool notify = 0 == --count;
        if (notify)
        {
            std::unique_lock<std::mutex> lock(mutex);
            lock.unlock();
            cv.notify_one();
        }
        return notify;
    }
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]{return 1 > count;});
    }
    
    // no copy
private:
    Signal(const Signal&) = delete;
    const Signal& operator=(const Signal&) = delete;
};

static const char DEFAULT_HOST[] = "127.0.0.1";

static const char* resolveIpPort(char* arg, int* port)
{
    if (arg == nullptr)
    {
        return DEFAULT_HOST;
    }
    
    char* colon = std::strchr(arg, ':');
    if (colon == nullptr)
    {
        return arg;
    }
    
    if (':' == *arg)
    {
        // assume local host
        *port = std::atoi(arg + 1);
        return DEFAULT_HOST;
    }
    
    *(colon++) = '\0';
    *port = std::atoi(colon);
    
    return arg;
}

static const char* resolveEndpoint(char* arg, int* port, bool* secure, std::string& uri)
{
    if (arg == nullptr)
    {
        *secure = false;
        return DEFAULT_HOST;
    }
    
    char* slash = std::strchr(arg, '/');
    if (slash == nullptr)
    {
        *secure = false;
        return resolveIpPort(arg, port);
    }
    
    if (':' != *(slash - 1))
    {
        // expect a trailing slash
        if ('\0' != *(slash + 1))
            uri.assign(slash);
        
        *secure = false;
        *slash = '\0';
        return resolveIpPort(arg, port); 
    }
    
    if ('/' != *(slash + 1))
    {
        // expecting ://
        return nullptr;
    }
    
    *secure = 's' == *(slash - 2);
    arg = slash + 2;
    
    if (nullptr != (slash = std::strchr(arg, '/')))
    {
        uri.assign(slash);
        *slash = '\0';
    }
    
    return resolveIpPort(arg, port);
}

struct Config
{
    const char* host; // ip address
    int port{0};
    bool secure; // ssl
    const char* hostname; // override host that appears in the request header
    std::string uri;
    std::string req_host;
};

int main(int argc, char* argv[])
{
    Config config;
    config.host = resolveEndpoint(argc > 1 ? argv[1] : nullptr, &config.port, &config.secure, config.uri);
    config.hostname = argc > 2 ? argv[2] : nullptr;
    
    if (config.host == nullptr)
    {
        fprintf(stderr, "Invalid endpoint %s\n", argv[1]);
        return 1;
    }
    
    if (config.uri.empty())
        config.uri.assign("/");
    
    if (config.port == 0)
    {
        config.port = config.secure ? 443 : 80;
        config.req_host.assign(config.hostname ? config.hostname : config.host);
    }
    else if (config.hostname)
    {
        config.req_host.assign(config.hostname);
    }
    else
    {
        config.req_host += config.host;
        config.req_host += ':';
        config.req_host += std::to_string(config.port);
    }
    
    brynet::net::WrapTcpService service;
    service.startWorkThread(2);
    
    //fprintf(stdout, "Connecting to http://%s:%d%s\n", host, port, uri);
    
    sock fd = brynet::net::base::Connect(false, config.host, config.port);
    if (fd == SOCKET_ERROR)
    {
        fprintf(stderr, "Could not connect to %s:%d\n", config.host, config.port);
        return 1;
    }
    
    if (config.secure)
        SSL_library_init();
    
    Signal signal(1);
    
    auto socket = brynet::net::TcpSocket::Create(fd, false);
    
    service.addSession(std::move(socket), [&signal, &config](const brynet::net::TCPSession::PTR& tcpSession) {
        //fprintf(stdout, "Connected.\n");
        brynet::net::HttpService::setup(tcpSession, [&signal, &config](const brynet::net::HttpSession::PTR& httpSession) {
            httpSession->setHttpCallback([&signal](const brynet::net::HTTPParser& httpParser, const brynet::net::HttpSession::PTR& session) {
                //http response handle
                std::cout << httpParser.getBody() << std::endl;
                signal.notify();
            });
            
            brynet::net::HttpRequest req;
            req.setMethod(brynet::net::HttpRequest::HTTP_METHOD::HTTP_METHOD_GET);
            req.setHost(config.req_host);
            req.setUrl(config.uri);
            
            std::string payload = req.getResult();
            httpSession->send(payload.c_str(), payload.size());
        });
    }, config.secure, nullptr, 1024 * 1024, false);
    
    signal.wait();
    return 0;
}
