#pragma once

#include "platform.h"

namespace socklib {
    enum class CancelReason {
        nocanceled,
        cancel_by_parent,
        blocking,
        os_error,
        ssl_error,
        timeout,
        interrupt,
        mustcancel,
        unknwon,
    };

    //like golang context.Context
    struct CancelContext {
       protected:
        CancelContext* parent = nullptr;
        CancelReason reason_ = CancelReason::nocanceled;
        bool canceled = false;

       public:
        constexpr CancelContext() {}
        constexpr CancelContext(CancelContext* c)
            : parent(c) {}

        virtual bool on_cancel() {
            if (parent && parent->on_cancel()) {
                canceled = true;
                reason_ = CancelReason::cancel_by_parent;
                return true;
            }
            return false;
        }

        virtual bool cancel() {
            return false;
        }

        virtual bool wait() {
            return !canceled;
        }

        CancelReason reason() const {
            return reason_;
        }

        CancelReason deep_reason() const {
            if (parent && reason_ == CancelReason::cancel_by_parent) return parent->deep_reason();
            return reason_;
        }

        void set_parent(CancelContext* ctx) {
            parent = ctx;
        }

        virtual ~CancelContext() {}
    };

    struct OsErrorContext : CancelContext {
       protected:
        bool cancel_when_block = false;
        int err = 0;
        friend struct Conn;
        using CancelContext::CancelContext;
        constexpr OsErrorContext(bool c, CancelContext* parent = nullptr)
            : cancel_when_block(c), CancelContext(parent) {}

       public:
        virtual bool on_cancel() override {
            if (CancelContext::on_cancel()) return true;
#ifdef _WIN32
            err = WSAGetLastError();
            bool block = err == WSAEWOULDBLOCK;
#else
            bool block = err == EAGAIN;
            err = errno;
#endif
            if (cancel_when_block && block) {
                reason_ = CancelReason::blocking;
                canceled = true;
                return true;
            }
            if (!block && err != 0) {
                reason_ = CancelReason::os_error;
                canceled = true;
                return true;
            }
            return false;
        }
    };

    struct SSLErrorContext : OsErrorContext {
       protected:
        ::SSL* ssl = nullptr;
        int sslerr = 0;

        friend struct SecureConn;

        SSLErrorContext(::SSL* ssl, CancelContext* parent = nullptr, bool cancel_when_block = false)
            : ssl(ssl), OsErrorContext(cancel_when_block, parent) {}

       public:
        virtual bool on_cancel() override {
            if (CancelContext::on_cancel()) return true;
            auto reason = SSL_get_error(ssl, 0);
            switch (reason) {
                case (SSL_ERROR_WANT_READ):
                case (SSL_ERROR_WANT_WRITE):
                    return false;
                case (SSL_ERROR_SYSCALL):
                    if (OsErrorContext::on_cancel()) {
                        return true;
                    }
                    break;
                default:
                    break;
            }
            reason_ = CancelReason::ssl_error;
            return true;
        }
    };

    //MustCancelContext is used with non-blocking socket
    //when blocked, on_cancel is called
    struct MustCancelContext : CancelContext {
        using CancelContext::CancelContext;
        virtual bool on_cancel() override {
            reason_ = CancelReason::mustcancel;
            canceled = true;
            return true;
        }
    };

    struct TimeoutContext : CancelContext {
       private:
        std::time_t timeout = 0, begintime = 0;

       public:
        using CancelContext::CancelContext;

        TimeoutContext(std::time_t timeout, CancelContext* parent = nullptr)
            : timeout(timeout), begintime(std::time(nullptr)), CancelContext(parent) {}
        virtual bool on_cancel() override {
            if (CancelContext::on_cancel()) return true;
            auto nowtime = std::time(nullptr);
            if (timeout < nowtime - begintime) {
                reason_ = CancelReason::timeout;
                canceled = true;
                return true;
            }
            return false;
        }

        virtual bool cancel() override {
            timeout = 0;
            return true;
        }
    };

    template <class Flag = bool>
    struct InterruptContext : CancelContext {
       private:
        Flag& flag;

       public:
        InterruptContext(Flag& f, CancelContext* parent = nullptr)
            : flag(f), CancelContext(parent) {}

        virtual bool cancel() override {
            flag = Flag(true);
            return true;
        }

        virtual bool on_cancel() override {
            if (CancelContext::on_cancel()) return true;
            if ((bool)flag) {
                reason_ = CancelReason::interrupt;
                canceled = true;
                return true;
            }
            return false;
        }
    };
}  // namespace socklib