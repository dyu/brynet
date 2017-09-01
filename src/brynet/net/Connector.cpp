#include <cassert>
#include <set>
#include <vector>
#include <map>
#include <thread>
#include <string>
#include <cstring>

#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/fdset.h>

#include <brynet/net/Connector.h>

using namespace brynet;
using namespace brynet::net;

namespace brynet
{
    namespace net
    {
        class AsyncConnectAddr
        {
        public:
            AsyncConnectAddr() noexcept
            {
                mPort = 0;
            }

            AsyncConnectAddr(const char* ip, int port, std::chrono::milliseconds timeout, std::int64_t ud) : mIP(ip), mPort(port), mTimeout(timeout), mUD(std::move(ud))
            {
            }

            const auto&     getIP() const
            {
                return mIP;
            }

            auto                getPort() const
            {
                return mPort;
            }

            const auto&         getUD() const
            {
                return mUD;
            }

            auto                getTimeout() const
            {
                return mTimeout;
            }

        private:
            std::string         mIP;
            int                 mPort;
            std::chrono::milliseconds   mTimeout;
            std::int64_t            mUD;
        };

        class ConnectorWorkInfo final : public NonCopyable
        {
        public:
            typedef std::shared_ptr<ConnectorWorkInfo>    PTR;

            ConnectorWorkInfo(AsyncConnector::COMPLETED_CALLBACK, AsyncConnector::FAILED_CALLBACK) noexcept;

            void                checkConnectStatus(int timeout);
            bool                isConnectSuccess(sock clientfd) const;
            void                checkTimeout();
            void                processConnect(AsyncConnectAddr);

        private:
            AsyncConnector::COMPLETED_CALLBACK      mCompletedCallback;
            AsyncConnector::FAILED_CALLBACK         mFailedCallback;

            struct ConnectingInfo
            {
                std::chrono::steady_clock::time_point startConnectTime;
                std::chrono::milliseconds     timeout;
                std::int64_t ud;
            };

            std::map<sock, ConnectingInfo>  mConnectingInfos;
            std::set<sock>                  mConnectingFds;

            struct FDSetDeleter
            {
                void operator()(struct fdset_s* ptr) const
                {
                    ox_fdset_delete(ptr);
                }
            };

            std::unique_ptr<struct fdset_s, FDSetDeleter> mFDSet;
        };
    }
}

ConnectorWorkInfo::ConnectorWorkInfo(AsyncConnector::COMPLETED_CALLBACK completedCallback,
    AsyncConnector::FAILED_CALLBACK failedCallback) noexcept : 
    mCompletedCallback(std::move(completedCallback)), mFailedCallback(std::move(failedCallback))
{
    mFDSet.reset(ox_fdset_new());
}

bool ConnectorWorkInfo::isConnectSuccess(sock clientfd) const
{
    bool connect_ret = false;

    if (ox_fdset_check(mFDSet.get(), clientfd, WriteCheck))
    {
        int error;
        int len = sizeof(error);
        if (getsockopt(clientfd, SOL_SOCKET, SO_ERROR, (char*)&error, (socklen_t*)&len) != -1)
        {
            connect_ret = error == 0;
        }
    }

    return connect_ret;
}

void ConnectorWorkInfo::checkConnectStatus(int timeout)
{
    if (ox_fdset_poll(mFDSet.get(), timeout) <= 0)
    {
        return;
    }

    std::set<sock>       complete_fds;   /*  ��ɶ���    */
    std::set<sock>       failed_fds;     /*  ʧ�ܶ���    */

    for (auto& v : mConnectingFds)
    {
        if (ox_fdset_check(mFDSet.get(), v, ErrorCheck))
        {
            complete_fds.insert(v);
            failed_fds.insert(v);
        } 
        else if (ox_fdset_check(mFDSet.get(), v, WriteCheck))
        {
            complete_fds.insert(v);
            if (!isConnectSuccess(v))
            {
                failed_fds.insert(v);
            }
        }
    }

    for (auto fd : complete_fds)
    {
        ox_fdset_del(mFDSet.get(), fd, WriteCheck | ErrorCheck);

        auto it = mConnectingInfos.find(fd);
        if (it != mConnectingInfos.end())
        {
            if (failed_fds.find(fd) != failed_fds.end())
            {
                ox_socket_close(fd);
                if (mFailedCallback != nullptr)
                {
                    mFailedCallback(it->second.ud);
                }
            }
            else
            {
                mCompletedCallback(fd, it->second.ud);
            }

            mConnectingInfos.erase(it);
        }

        mConnectingFds.erase(fd);
    }
}

void ConnectorWorkInfo::checkTimeout()
{
    for (auto it = mConnectingInfos.begin(); it != mConnectingInfos.end();)
    {
        auto now = std::chrono::steady_clock::now();
        if ((now - it->second.startConnectTime) < it->second.timeout)
        {
            ++it;
            continue;
        }

        auto fd = it->first;
        auto uid = it->second.ud;

        ox_fdset_del(mFDSet.get(), fd, WriteCheck | ErrorCheck);

        mConnectingFds.erase(fd);
        mConnectingInfos.erase(it++);

        ox_socket_close(fd);
        if (mFailedCallback != nullptr)
        {
            mFailedCallback(uid);
        }
    }
}

void ConnectorWorkInfo::processConnect(AsyncConnectAddr addr)
{
    bool addToFDSet = false;
    bool connectSuccess = false;

    struct sockaddr_in server_addr;
    sock clientfd = SOCKET_ERROR;

    ox_socket_init();

    clientfd = ox_socket_create(AF_INET, SOCK_STREAM, 0);
    ox_socket_nonblock(clientfd);

    if (clientfd != SOCKET_ERROR)
    {
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr(addr.getIP().c_str());
        server_addr.sin_port = htons(addr.getPort());

        int n = connect(clientfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr));
        if (n < 0)
        {
            int check_error = 0;
#if defined PLATFORM_WINDOWS
            check_error = WSAEWOULDBLOCK;
#else
            check_error = EINPROGRESS;
#endif
            if (check_error != sErrno)
            {
                ox_socket_close(clientfd);
                clientfd = SOCKET_ERROR;
            }
            else
            {
                ConnectingInfo ci;
                ci.startConnectTime = std::chrono::steady_clock::now();
                ci.ud = addr.getUD();
                ci.timeout = addr.getTimeout();

                mConnectingInfos[clientfd] = ci;
                mConnectingFds.insert(clientfd);

                ox_fdset_add(mFDSet.get(), clientfd, WriteCheck | ErrorCheck);
                addToFDSet = true;
            }
        }
        else if (n == 0)
        {
            connectSuccess = true;
        }
    }

    if (connectSuccess)
    {
        mCompletedCallback(clientfd, addr.getUD());
    }
    else
    {
        if (!addToFDSet && mFailedCallback != nullptr)
        {
            mFailedCallback(addr.getUD());
        }
    }
}

AsyncConnector::AsyncConnector()
{
    mIsRun = false;
}

AsyncConnector::~AsyncConnector()
{
    destroy();
}

void AsyncConnector::run()
{
    while (mIsRun)
    {
        mEventLoop.loop(10);
        mWorkInfo->checkConnectStatus(0);
        mWorkInfo->checkTimeout();
    }
}

void AsyncConnector::startThread(COMPLETED_CALLBACK completedCallback, FAILED_CALLBACK failedCallback)
{
    std::lock_guard<std::mutex> lck(mThreadGuard);
    if (mThread == nullptr)
    {
        mIsRun = true;
        mWorkInfo = std::make_shared<ConnectorWorkInfo>(std::move(completedCallback), std::move(failedCallback));
        mThread = std::make_shared<std::thread>([shared_this = shared_from_this()](){
            shared_this->run();
        });
    }
}

void AsyncConnector::destroy()
{
    std::lock_guard<std::mutex> lck(mThreadGuard);
    if (mThread != nullptr)
    {
        mIsRun = false;
        if (mThread->joinable())
        {
            mThread->join();
        }
        mThread = nullptr;
        mWorkInfo = nullptr;
    }
}

//TODO::�����Ѿ������˹����̣߳���Ͷ���첽������������⣨���޵ȴ���
void AsyncConnector::asyncConnect(const char* ip, int port, int ms, std::int64_t ud)
{
    mEventLoop.pushAsyncProc([shared_this = shared_from_this(), address = AsyncConnectAddr(ip, port, std::chrono::milliseconds(ms), ud)]() {
        shared_this->mWorkInfo->processConnect(address);
    });
}

AsyncConnector::PTR AsyncConnector::Create()
{
    struct make_shared_enabler : public AsyncConnector {};
    return std::make_shared<make_shared_enabler>();
}
