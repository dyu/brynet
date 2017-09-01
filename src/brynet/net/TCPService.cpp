#include <iostream>
#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/EventLoop.h>
#include <brynet/net/ListenThread.h>
#include <brynet/net/TCPService.h>

const static unsigned int sDefaultLoopTimeOutMS = 100;
using namespace brynet;
using namespace brynet::net;

namespace brynet
{
    namespace net
    {
        /*  �˽ṹ���ڱ�ʾһ���ػ����߼��̺߳������߳�ͨ����ͨ���˽ṹ�Իػ�������ز���(������ֱ�Ӵ���Channel/DataSocketָ��)  */
        union SessionId
        {
            struct
            {
                uint16_t    loopIndex;      /*  �Ự������eventloop��(��mLoops�е�)����  */
                uint16_t    index;          /*  �Ự��mDataSockets[loopIndex]�е�����ֵ */
                uint32_t    iid;            /*  ����������   */
            }data;  /*  warn::so,���������֧��0xFFFF(65536)��io loop�̣߳�ÿһ��io loop���֧��0xFFFF(65536)�����ӡ�*/

            TcpService::SESSION_TYPE id;
        };

        class IOLoopData : public brynet::NonCopyable, public std::enable_shared_from_this<IOLoopData>
        {
        public:
            typedef std::shared_ptr<IOLoopData> PTR;

        public:
            static  PTR                         Create(EventLoop::PTR eventLoop, std::shared_ptr<std::thread> ioThread);

        public:
            void send(TcpService::SESSION_TYPE id, const DataSocket::PACKET_PTR& packet, const DataSocket::PACKED_SENDED_CALLBACK& callback);
            const EventLoop::PTR&           getEventLoop() const;

        private:
            TypeIDS<DataSocket::PTR>&       getDataSockets();
            std::shared_ptr<std::thread>&   getIOThread();
            int                             incID();

        private:
            explicit IOLoopData(EventLoop::PTR eventLoop,
                std::shared_ptr<std::thread> ioThread);

        private:
            const EventLoop::PTR            mEventLoop;
            std::shared_ptr<std::thread>    mIOThread;

            TypeIDS<DataSocket::PTR>        mDataSockets;
            int                             incId;

            friend class TcpService;
        };

        void IOLoopDataSend(const std::shared_ptr<IOLoopData>& ioLoopData, TcpService::SESSION_TYPE id, const DataSocket::PACKET_PTR& packet, const DataSocket::PACKED_SENDED_CALLBACK& callback)
        {
            ioLoopData->send(id, packet, callback);
        }

        const EventLoop::PTR& IOLoopDataGetEventLoop(const std::shared_ptr<IOLoopData>& ioLoopData)
        {
            return ioLoopData->getEventLoop();
        }
    }
}

TcpService::TcpService() noexcept
{
    static_assert(sizeof(SessionId) == sizeof(((SessionId*)nullptr)->id), "sizeof SessionId must equal int64_t");

    mEnterCallback = nullptr;
    mDisConnectCallback = nullptr;
    mDataCallback = nullptr;

    mRunIOLoop = false;
}

TcpService::~TcpService() noexcept
{
    stopWorkerThread();
}

void TcpService::setEnterCallback(TcpService::ENTER_CALLBACK callback)
{
    mEnterCallback = std::move(callback);
}

void TcpService::setDisconnectCallback(TcpService::DISCONNECT_CALLBACK callback)
{
    mDisConnectCallback = std::move(callback);
}

void TcpService::setDataCallback(TcpService::DATA_CALLBACK callback)
{
    mDataCallback = std::move(callback);
}

const TcpService::ENTER_CALLBACK& TcpService::getEnterCallback() const
{
    return mEnterCallback;
}

const TcpService::DISCONNECT_CALLBACK& TcpService::getDisconnectCallback() const
{
    return mDisConnectCallback;
}

const TcpService::DATA_CALLBACK& TcpService::getDataCallback() const
{
    return mDataCallback;
}

void TcpService::send(SESSION_TYPE id, const DataSocket::PACKET_PTR& packet, const DataSocket::PACKED_SENDED_CALLBACK& callback) const
{
    union  SessionId sid;
    sid.id = id;
    auto ioLoopData = getIOLoopDataBySocketID(id);
    if (ioLoopData != nullptr)
    {
        ioLoopData->send(id, packet, callback);
    }
}

void TcpService::shutdown(SESSION_TYPE id) const
{
    postSessionAsyncProc(id, [](DataSocket::PTR ds){
        ds->postShutdown();
    });
}

void TcpService::disConnect(SESSION_TYPE id) const
{
    postSessionAsyncProc(id, [](DataSocket::PTR ds){
        ds->postDisConnect();
    });
}

void TcpService::setPingCheckTime(SESSION_TYPE id, int checktime)
{
    postSessionAsyncProc(id, [checktime](DataSocket::PTR ds){
        ds->setCheckTime(checktime);
    });
}

void TcpService::postSessionAsyncProc(SESSION_TYPE id, std::function<void(DataSocket::PTR)> callback) const
{
    union  SessionId sid;
    sid.id = id;
    auto ioLoopData = getIOLoopDataBySocketID(id);
    if (ioLoopData == nullptr)
    {
        return;
    }

    const auto& eventLoop = ioLoopData->getEventLoop();
    eventLoop->pushAsyncProc([callbackCapture = std::move(callback), sid, shared_this = shared_from_this(), ioLoopDataCapture = std::move(ioLoopData)](){
        DataSocket::PTR tmp = nullptr;
        if (callbackCapture != nullptr &&
            ioLoopDataCapture->getDataSockets().get(sid.data.index, tmp) &&
            tmp != nullptr)
        {
            auto ud = static_cast<const int64_t*>(&tmp->getUD());
            //auto ud = std::any_cast<SESSION_TYPE>(&tmp->getUD());
            if (ud != nullptr && *ud == sid.id)
            {
                callbackCapture(tmp);
            }
        }
    });
}

void TcpService::stopWorkerThread()
{
    std::lock_guard<std::mutex> lck(mServiceGuard);
    std::lock_guard<std::mutex> lock(mIOLoopGuard);

    mRunIOLoop = false;

    for (const auto& v : mIOLoopDatas)
    {
        v->getEventLoop()->wakeup();
        if (v->getIOThread()->joinable())
        {
            v->getIOThread()->join();
        }
    }
    mIOLoopDatas.clear();
}

void TcpService::startWorkerThread(size_t threadNum, FRAME_CALLBACK callback)
{
    std::lock_guard<std::mutex> lck(mServiceGuard);
    std::lock_guard<std::mutex> lock(mIOLoopGuard);

    if (!mIOLoopDatas.empty())
    {
        return;
    }

    mRunIOLoop = true;

    mIOLoopDatas.resize(threadNum);
    for (auto& v : mIOLoopDatas)
    {
        auto eventLoop = std::make_shared<EventLoop>();
        v = IOLoopData::Create(eventLoop, std::make_shared<std::thread>([callback,
            shared_this = shared_from_this(),
            eventLoop]() {
            while (shared_this->mRunIOLoop)
            {
                eventLoop->loop(eventLoop->getTimerMgr()->isEmpty() ? sDefaultLoopTimeOutMS : eventLoop->getTimerMgr()->nearEndMs().count());
                if (callback != nullptr)
                {
                    callback(eventLoop);
                }
            }
        }));
    }
}

void TcpService::wakeup(SESSION_TYPE id) const
{
    union  SessionId sid;
    sid.id = id;
    auto eventLoop = getEventLoopBySocketID(id);
    if (eventLoop != nullptr)
    {
        eventLoop->wakeup();
    }
}

void TcpService::wakeupAll() const
{
    std::lock_guard<std::mutex> lock(mIOLoopGuard);

    for (const auto& v : mIOLoopDatas)
    {
        v->getEventLoop()->wakeup();
    }
}

EventLoop::PTR TcpService::getRandomEventLoop()
{
    EventLoop::PTR ret;
    {
        auto randNum = rand();
        std::lock_guard<std::mutex> lock(mIOLoopGuard);
        if (!mIOLoopDatas.empty())
        {
            ret = mIOLoopDatas[randNum % mIOLoopDatas.size()]->getEventLoop();
        }
    }

    return ret;
}

TcpService::PTR TcpService::Create()
{
    struct make_shared_enabler : public TcpService {};
    return std::make_shared<make_shared_enabler>();
}

EventLoop::PTR TcpService::getEventLoopBySocketID(SESSION_TYPE id) const noexcept
{
    std::lock_guard<std::mutex> lock(mIOLoopGuard);

    union  SessionId sid;
    sid.id = id;
    assert(sid.data.loopIndex < mIOLoopDatas.size());
    if (sid.data.loopIndex < mIOLoopDatas.size())
    {
        return mIOLoopDatas[sid.data.loopIndex]->getEventLoop();
    }
    else
    {
        return nullptr;
    }
}

std::shared_ptr<IOLoopData> TcpService::getIOLoopDataBySocketID(SESSION_TYPE id) const noexcept
{
    std::lock_guard<std::mutex> lock(mIOLoopGuard);

    union  SessionId sid;
    sid.id = id;
    assert(sid.data.loopIndex < mIOLoopDatas.size());
    if (sid.data.loopIndex < mIOLoopDatas.size())
    {
        return mIOLoopDatas[sid.data.loopIndex];
    }
    else
    {
        return nullptr;
    }
}

TcpService::SESSION_TYPE TcpService::MakeID(size_t loopIndex, const std::shared_ptr<IOLoopData>& loopData)
{
    union SessionId sid;
    sid.data.loopIndex = static_cast<uint16_t>(loopIndex);
    sid.data.index = static_cast<uint16_t>(loopData->getDataSockets().claimID());
    sid.data.iid = loopData->incID();

    return sid.id;
}

void TcpService::procDataSocketClose(DataSocket::PTR ds)
{
    auto ud = static_cast<const int64_t*>(&ds->getUD());
    //auto ud = std::any_cast<SESSION_TYPE>(&ds->getUD());
    if (ud != nullptr)
    {
        union SessionId sid;
        sid.id = *ud;

        mIOLoopDatas[sid.data.loopIndex]->getDataSockets().set(nullptr, sid.data.index);
        mIOLoopDatas[sid.data.loopIndex]->getDataSockets().reclaimID(sid.data.index);
    }
}

bool TcpService::helpAddChannel(DataSocket::PTR channel, const std::string& ip, 
    const TcpService::ENTER_CALLBACK& enterCallback, const TcpService::DISCONNECT_CALLBACK& disConnectCallback, const TcpService::DATA_CALLBACK& dataCallback,
    bool forceSameThreadLoop)
{
    std::shared_ptr<IOLoopData> ioLoopData;
    size_t loopIndex = 0;
    {
        auto randNum = rand();
        std::lock_guard<std::mutex> lock(mIOLoopGuard);

        if (mIOLoopDatas.empty())
        {
            return false;
        }
        
        if (forceSameThreadLoop)
        {
            bool find = false;
            for (size_t i = 0; i < mIOLoopDatas.size(); i++)
            {
                if (mIOLoopDatas[i]->getEventLoop()->isInLoopThread())
                {
                    loopIndex = i;
                    find = true;
                    break;
                }
            }
            if (!find)
            {
                return false;
            }
        }
        else
        {
            /*  ���Ϊ�����ӷ���һ��eventloop */
            loopIndex = randNum % mIOLoopDatas.size();
        }

        ioLoopData = mIOLoopDatas[loopIndex];
    }
    

    const auto& loop = ioLoopData->getEventLoop();

    channel->setEnterCallback([ip, loopIndex, enterCallback, disConnectCallback, dataCallback, shared_this = shared_from_this(), loopDataCapture = std::move(ioLoopData)](DataSocket::PTR dataSocket){
        auto id = shared_this->MakeID(loopIndex, loopDataCapture);
        union SessionId sid;
        sid.id = id;
        loopDataCapture->getDataSockets().set(dataSocket, sid.data.index);
        dataSocket->setUD(id);
        dataSocket->setDataCallback([dataCallback, id](DataSocket::PTR ds, const char* buffer, size_t len){
            return dataCallback(id, buffer, len);
        });

        dataSocket->setDisConnectCallback([disConnectCallback, id, shared_this](DataSocket::PTR arg){
            shared_this->procDataSocketClose(arg);
            disConnectCallback(id);
            delete arg;
        });

        if (enterCallback != nullptr)
        {
            enterCallback(id, ip);
        }
    });

    loop->pushAsyncProc([loop, channel](){
        if (!channel->onEnterEventLoop(std::move(loop)))
        {
            delete channel;
        }
    });

    return true;
}

bool TcpService::addDataSocket(sock fd,
    const std::shared_ptr<ListenThread>& listenThread,
    const TcpService::ENTER_CALLBACK& enterCallback,
    const TcpService::DISCONNECT_CALLBACK& disConnectCallback,
    const TcpService::DATA_CALLBACK& dataCallback,
    bool isUseSSL,
    size_t maxRecvBufferSize,
    bool forceSameThreadLoop)
{
    std::string ip = ox_socket_getipoffd(fd);
    DataSocket::PTR channel = nullptr;
#ifdef USE_OPENSSL
    bool ret = true;
    channel = new DataSocket(fd, maxRecvBufferSize);
    if (isUseSSL)
    {
        if (listenThread != nullptr)
        {
            if (listenThread.getOpenSSLCTX() != nullptr)
            {
                ret = channel->initAcceptSSL(listenThread.getOpenSSLCTX());
            }
            else
            {
                ret = false;
            }
        }
        else
        {
            ret = channel->initConnectSSL();
        }
    }
#else
    bool ret = false;
    if (!isUseSSL)
    {
        channel = new DataSocket(fd, maxRecvBufferSize);
        ret = true;
    }
#endif
    if (ret)
    {
        ret = helpAddChannel(channel, ip, enterCallback, disConnectCallback, dataCallback, forceSameThreadLoop);
    }

    if (!ret)
    {
        if (channel != nullptr)
        {
            delete channel;
            channel = nullptr;
        }
        else
        {
            ox_socket_close(fd);
        }
    }

    return ret;
}

IOLoopData::PTR IOLoopData::Create(EventLoop::PTR eventLoop, std::shared_ptr<std::thread> ioThread)
{
    struct make_shared_enabler : public IOLoopData
    {
        make_shared_enabler(EventLoop::PTR eventLoop, std::shared_ptr<std::thread> ioThread) :
            IOLoopData(std::move(eventLoop), std::move(ioThread))
        {}
    };

    return std::make_shared<make_shared_enabler>(std::move(eventLoop), std::move(ioThread));
}

IOLoopData::IOLoopData(EventLoop::PTR eventLoop, std::shared_ptr<std::thread> ioThread)
    : mEventLoop(std::move(eventLoop)), mIOThread(std::move(ioThread))
{}

const EventLoop::PTR& IOLoopData::getEventLoop() const
{
    return mEventLoop;
}

brynet::TypeIDS<DataSocket::PTR>& IOLoopData::getDataSockets()
{
    return mDataSockets;
}

std::shared_ptr<std::thread>& IOLoopData::getIOThread()
{
    return mIOThread;
}

int IOLoopData::incID()
{
    return incId++;
}

void IOLoopData::send(TcpService::SESSION_TYPE id, const DataSocket::PACKET_PTR& packet, const DataSocket::PACKED_SENDED_CALLBACK& callback)
{
    union  SessionId sid;
    sid.id = id;
    /*  �����ǰ���������߳���ֱ��send,����ʹ��pushAsyncProc����lambda(���ٶ���Ӱ��),�����ʹ��postSessionAsyncProc */
    if (mEventLoop->isInLoopThread())
    {
        DataSocket::PTR tmp = nullptr;
        if (mDataSockets.get(sid.data.index, tmp) &&
            tmp != nullptr)
        {
            auto ud = static_cast<const int64_t*>(&tmp->getUD());
            //auto ud = std::any_cast<TcpService::SESSION_TYPE>(&tmp->getUD());
            if (ud != nullptr && *ud == sid.id)
            {
                tmp->sendPacketInLoop(packet, callback);
            }
        }
    }
    else
    {
        mEventLoop->pushAsyncProc([packetCapture = packet, callbackCapture = callback, sid, ioLoopDataCapture = shared_from_this()](){
            DataSocket::PTR tmp = nullptr;
            if (ioLoopDataCapture->mDataSockets.get(sid.data.index, tmp) &&
                tmp != nullptr)
            {
                auto ud = static_cast<const int64_t*>(&tmp->getUD());
                //auto ud = std::any_cast<TcpService::SESSION_TYPE>(&tmp->getUD());
                if (ud != nullptr && *ud == sid.id)
                {
                    tmp->sendPacketInLoop(packetCapture, callbackCapture);
                }
            }
        });
    }
}
