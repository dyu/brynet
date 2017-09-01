#ifndef BRYNET_NET_DATASOCKET_H_
#define BRYNET_NET_DATASOCKET_H_

//#include <any>
#include <memory>
#include <functional>
#include <deque>

#include <brynet/net/Channel.h>
#include <brynet/net/EventLoop.h>
#include <brynet/timer/Timer.h>
#include <brynet/utils/NonCopyable.h>

#ifdef USE_OPENSSL

#ifdef  __cplusplus
extern "C" {
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#ifdef  __cplusplus
}
#endif

#endif

struct buffer_s;

/*  使用裸指针,且一旦投递到eventloop,只有在onEnterEventLoop失败或者断开回调中才能delete它(一个DataSocket的断开回调只会被调用一次)  */

namespace brynet
{
    namespace net
    {
        class DataSocket final : public Channel, public NonCopyable
        {
        public:
            typedef DataSocket*                                                         PTR;

            typedef std::function<void(PTR)>                                            ENTER_CALLBACK;
            typedef std::function<size_t(PTR, const char* buffer, size_t len)>          DATA_CALLBACK;
            typedef std::function<void(PTR)>                                            DISCONNECT_CALLBACK;
            typedef std::shared_ptr<std::function<void(void)>>                          PACKED_SENDED_CALLBACK;

            typedef std::shared_ptr<std::string>                                        PACKET_PTR;

        public:
            DataSocket(sock fd, size_t maxRecvBufferSize) noexcept;
            ~DataSocket() noexcept;

            /*  仅在网络线程中调用才可能返回成功 */
            bool                            onEnterEventLoop(const EventLoop::PTR& eventLoop);

            void                            send(const char* buffer, size_t len, const PACKED_SENDED_CALLBACK& callback = nullptr);
            void                            sendPacket(const PACKET_PTR&, const PACKED_SENDED_CALLBACK& callback = nullptr);
            void                            sendPacketInLoop(const PACKET_PTR&, const PACKED_SENDED_CALLBACK& callback = nullptr);

            void                            setEnterCallback(ENTER_CALLBACK cb);
            void                            setDataCallback(DATA_CALLBACK cb);
            void                            setDisConnectCallback(DISCONNECT_CALLBACK cb);

            /*  设置心跳检测时间,overtime为-1表示不检测   */
            void                            setCheckTime(int overtime);
            /*  主动(投递)断开连接,如果成功主动断开(表明底层没有先触发被动断开)则会触发断开回调  */
            void                            postDisConnect();
            void                            postShutdown();

            void                            setUD(std::int64_t value);
            const std::int64_t&             getUD() const;

#ifdef USE_OPENSSL
            bool                            initAcceptSSL(SSL_CTX*);
            bool                            initConnectSSL();
#endif

            static  DataSocket::PACKET_PTR  makePacket(const char* buffer, size_t len);

        private:
            void                            growRecvBuffer();

            void                            PingCheck();
            void                            startPingCheckTimer();

            void                            canRecv() override;
            void                            canSend() override;

            bool                            checkRead();
            bool                            checkWrite();

            void                            recv();
            void                            flush();
            void                            normalFlush();
            void                            quickFlush();

            void                            onClose() override;
            void                            closeSocket();
            void                            procCloseInLoop();
            void                            procShutdownInLoop();

            void                            runAfterFlush();
#ifdef PLATFORM_LINUX
            void                            removeCheckWrite();
#endif
#ifdef USE_OPENSSL
            void                            processSSLHandshake();
#endif

        private:

#ifdef PLATFORM_WINDOWS
            struct EventLoop::ovl_ext_s     mOvlRecv;
            struct EventLoop::ovl_ext_s     mOvlSend;

            bool                            mPostRecvCheck;     /*  是否投递了可读检测   */
            bool                            mPostWriteCheck;    /*  是否投递了可写检测   */
#endif

            sock                            mFD;
            bool                            mIsPostFinalClose;  /*  是否投递了最终的close处理    */

            bool                            mCanWrite;          /*  socket是否可写  */

            EventLoop::PTR                  mEventLoop;
            buffer_s*                       mRecvBuffer;
            size_t                          mMaxRecvBufferSize;

            struct pending_packet
            {
                PACKET_PTR  data;
                size_t      left;
                PACKED_SENDED_CALLBACK  mCompleteCallback;
            };

            typedef std::deque<pending_packet>   PACKET_LIST_TYPE;
            PACKET_LIST_TYPE                mSendList;          /*  发送消息列表  */

            ENTER_CALLBACK                  mEnterCallback;
            DATA_CALLBACK                   mDataCallback;
            DISCONNECT_CALLBACK             mDisConnectCallback;

            bool                            mIsPostFlush;       /*  是否已经放置flush消息的回调    */

            std::int64_t                    mUD;                /*  链接的用户自定义数据  */

#ifdef USE_OPENSSL
            SSL_CTX*                        mSSLCtx;            /*  mSSL不为null时，如果mSSLCtx不为null，则表示ssl的客户端链接，否则为accept链接  */
            SSL*                            mSSL;               /*  mSSL不为null，则表示为ssl安全连接   */
            bool                            mIsHandsharked;
#endif

            bool                            mRecvData;
            int                             mCheckTime;
            Timer::WeakPtr                  mTimer;
        };
    }
}

#endif
