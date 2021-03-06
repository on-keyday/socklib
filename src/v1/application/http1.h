/*
    socklib - simple socket library
    Copyright (c) 2021 on-keyday (https://github.com/on-keyday)
    Released under the MIT license
    https://opensource.org/licenses/mit-license.php
*/

#pragma once

#include "../transport/tcp.h"
#include "../transport/conn_struct_base.h"
#include <net_helper.h>

#include <thread>
#include <atomic>

namespace socklib {

    constexpr const char* reason_phrase(unsigned short status, bool dav = false) {
        switch (status) {
            case 100:
                return "Continue";
            case 101:
                return "Switching Protocols";
            case 103:
                return "Early Hints";
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 202:
                return "Accepted";
            case 203:
                return "Non-Authoritative Information";
            case 204:
                return "No Content";
            case 205:
                return "Reset Content";
            case 206:
                return "Partial Content";
            case 300:
                return "Multiple Choices";
            case 301:
                return "Moved Permently";
            case 302:
                return "Found";
            case 303:
                return "See Other";
            case 304:
                return "Not Modified";
            case 307:
                return "Temporary Redirect";
            case 308:
                return "Permanent Redirect";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 402:
                return "Payment Required";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 406:
                return "Not Acceptable";
            case 407:
                return "Proxy Authentication Required";
            case 408:
                return "Request Timeout";
            case 409:
                return "Conflict";
            case 410:
                return "Gone";
            case 411:
                return "Length Required";
            case 412:
                return "Precondition Failed";
            case 413:
                return "Payload Too Large";
            case 414:
                return "URI Too Long";
            case 415:
                return "Unsupported Media Type";
            case 416:
                return "Range Not Satisfiable";
            case 417:
                return "Expectation Failed";
            case 418:
                return "I'm a teapot";
            case 421:
                return "Misdirected Request";
            case 425:
                return "Too Early";
            case 426:
                return "Upgrade Required";
            case 429:
                return "Too Many Requests";
            case 431:
                return "Request Header Fields Too Large";
            case 451:
                return "Unavailable For Legal Reasons";
            case 500:
                return "Internal Server Error";
            case 501:
                return "Not Implemented";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            case 504:
                return "Gateway Timeout";
            case 505:
                return "HTTP Version Not Supported";
            case 506:
                return "Variant Also Negotiates";
            case 510:
                return "Not Extended";
            case 511:
                return "Network Authentication Required";
            default:
                break;
        }
        if (dav) {
            switch (status) {
                case 102:
                    return "Processing";
                case 207:
                    return "Multi-Status";
                case 208:
                    return "Already Reported";
                case 226:
                    return "IM Used";
                case 422:
                    return "Unprocessable Entity";
                case 423:
                    return "Locked";
                case 424:
                    return "Failed Dependency";
                case 507:
                    return "Insufficient Storage";
                case 508:
                    return "Loop Detected";
                default:
                    break;
            }
        }
        return "Unknown";
    }

    struct HttpConn : public AppLayer {
        friend struct Http1;
        friend struct HttpClient;
        using Header = std::multimap<std::string, std::string>;

       protected:
        Header header;
        std::string host;
        std::string path_;
        std::string query_;
        bool done = false;
        bool recving = false;
        std::uint32_t waiting = 0;
        std::string tmpbuffer;
        HttpConn(std::shared_ptr<Conn>&& in, std::string&& hostname, std::string&& path, std::string&& query)
            : AppLayer(std::move(in)), host(hostname), path_(path), query_(query) {}

       protected:
        bool send_detail(std::string& res, const Header& header, const char* body, size_t bodylen, bool mustlen) {
            if (!conn) return false;
            if ((body && bodylen) || mustlen) {
                res += "content-length: ";
                res += std::to_string(bodylen);
                res += "\r\n";
            }
            for (auto& h : header) {
                std::string tmp;
                tmp.resize(h.first.size());
                std::transform(h.first.begin(), h.first.end(), tmp.begin(), [](auto c) { return std::tolower((unsigned char)c); });
                if (tmp.find("content-length") != ~0 || tmp.find("host") != ~0) {
                    continue;
                }
                if (h.first.find("\r") != ~0 || h.first.find("\n") != ~0 || h.second.find("\r") != ~0 || h.second.find("\n") != ~0) {
                    continue;
                }
                res += h.first;
                res += ": ";
                res += h.second;
                res += "\r\n";
            }
            res += "\r\n";
            if (body && bodylen) {
                res.append(body, bodylen);
            }
            return conn->write(res);
        }

       public:
        bool wait() const {
            return waiting;
        }

        const std::string& path() const {
            return path_;
        }

        const std::string& query() const {
            return query_;
        }

        std::string url() const {
            if (!host.size()) return path_ + query_;
            return (conn->get_ssl() ? "https://" : "http://") + host + path_ + query_;
        }
    };

    struct HttpClientConn : HttpConn {
        std::string _method;

       public:
        HttpClientConn(std::shared_ptr<Conn>&& in, std::string&& hostname, std::string&& path, std::string&& query)
            : HttpConn(std::move(in), std::move(hostname), std::move(path), std::move(query)) {}

        std::string& remain_buffer() {
            return tmpbuffer;
        }

        Header& response() {
            return header;
        }

        const std::string& method() {
            return _method;
        }

        bool send(const char* method, const Header& header = Header(), const char* body = nullptr, size_t bodylen = 0, bool mustlen = false) {
            if (!method) return false;
            std::string res = method;
            res += " ";
            res += path_;
            res += query_;
            res += " HTTP/1.1\r\nhost: ";
            res += host;
            res += "\r\n";
            done = send_detail(res, header, body, bodylen, mustlen);
            if (done) _method = method;
            return done;
        }

        bool recv(bool igbody = false, CancelContext* cancel = nullptr) {
            if (!done || recving) return false;
            recving = true;
            commonlib2::Reader<SockReader> r(SockReader(conn, cancel));
            if (tmpbuffer.size()) {
                r.ref().ref() = std::move(tmpbuffer);
            }
            struct {
                decltype(r)& r;
                decltype(header)& h;
                bool first = false;
                size_t prevpos = 0;
            } data{r, header};
            r.ref().setcallback(
                [](void* ctx, bool oneof) {
                    auto d = (decltype(&data))ctx;
                },
                &data);
            header.clear();
            if (!commonlib2::parse_httpresponse(r, header, igbody)) {
                return false;
            }
            if (r.readable() > 1) {
                tmpbuffer = r.ref().ref().substr(r.readpos());
            }
            recving = false;
            return header.size() != 0;
        }

        template <class F, class... Args>
        void recv(F&& f, bool igbody, Args&&... args) {
            std::shared_ptr<HttpClientConn> self(
                this, +[](HttpClientConn*) {});
            f(self, recv(igbody), std::forward<Args>(args)...);
        }

        template <class F, class... Args>
        bool recv_async(F&& f, bool igbody, Args&&... args) {
            if (waiting) return false;
            waiting++;
            std::thread([&, this]() {
                std::shared_ptr<HttpClientConn> self(
                    this, +[](HttpClientConn*) {});
                f(self, recv(igbody), std::forward<Args>(args)...);
                waiting--;
            }).detach();
            return true;
        }

        ~HttpClientConn() {
            while (waiting)
                Sleep(5);
        }
    };

    struct HttpServerConn : HttpConn {
        HttpServerConn(std::shared_ptr<Conn>&& in)
            : HttpConn(std::move(in), std::string(), std::string(), std::string()) {}

        Header& request() {
            return header;
        }

        bool recv(CancelContext* cancel = nullptr) {
            if (recving) return false;
            recving = true;
            commonlib2::Reader<SockReader> r(SockReader(conn, cancel));
            header.clear();
            if (!commonlib2::parse_httprequest(r, header)) {
                recving = false;
                return false;
            }
            if (auto found = header.find(":path"); found != header.end()) {
                if (auto ch = commonlib2::split(found->second, "?", 1); ch.size() == 2) {
                    found->second = ch[0];
                    header.emplace(":query", "?" + ch[1]);
                    path_ = ch[0];
                    query_ = "?" + ch[1];
                }
                else {
                    path_ = ch[0];
                }
            }
            recving = false;
            done = true;
            return true;
        }

        bool send(unsigned short statuscode = 200, const char* phrase = "OK", const Header& header = Header(), const char* body = nullptr, size_t bodylen = 0) {
            if (!done) return false;
            if (statuscode < 100 && statuscode > 999) return false;
            if (!phrase || phrase[0] == 0 || std::string(phrase).find("\r") != ~0 || std::string(phrase).find("\n") != ~0) return false;
            std::string res = "HTTP/1.1 ";
            res += std::to_string(statuscode);
            res += " ";
            res += phrase;
            res += "\r\n";
            return send_detail(res, header, body, bodylen, true);
        }
    };

    struct HttpOpenContext {
        const char* url = nullptr;
        const char* cacert = nullptr;
        IPMode ipmode = IPMode::both;
        bool urlencoded = false;
        CancelContext* cancel = nullptr;
        const char* proxy = nullptr;
        unsigned short proxy_port = 0;
        OpenErr err = true;
    };

    struct HttpRequestContext {
        commonlib2::URLContext<std::string> url;
        unsigned short port = 0;
        std::string path, query;
        std::string host_with_port() const {
            return url.host + (url.port.size() ? ":" + url.port : "");
        }
    };

    struct Http1 {
        friend struct WebSocket;
        friend struct Http2;
        friend struct HttpClient;
        friend struct HttpServer;

       private:
        static std::shared_ptr<Conn> open_tcp_conn(HttpRequestContext& ctx, HttpOpenContext& arg, const char* alpnstr = nullptr, int len = 0) {
            return TCP::open_secure(ctx.url.host.c_str(), ctx.port, ctx.url.scheme.c_str(), true,
                                    arg.cacert, ctx.url.scheme == "https", alpnstr, len, true, &arg.err, arg.cancel, arg.ipmode);
        }

        static OpenErr reopen_tcp_conn(std::shared_ptr<Conn>& conn, HttpRequestContext& ctx, HttpOpenContext& arg, const char* alpnstr = nullptr, int len = 0) {
            arg.err = TCP::reopen_secure(conn, ctx.url.host.c_str(), ctx.port, ctx.url.scheme.c_str(), true,
                                         arg.cacert, ctx.url.scheme == "https", alpnstr, len, true, arg.cancel, arg.ipmode);
            return arg.err;
        }

        static std::shared_ptr<HttpClientConn> init_object(std::shared_ptr<Conn>& conn, HttpRequestContext& ctx) {
            return std::make_shared<HttpClientConn>(std::move(conn), ctx.host_with_port(), std::move(ctx.path), std::move(ctx.query));
        }

        static bool
        setuphttp(HttpOpenContext& arg, HttpRequestContext& ctx,
                  const char* normal = "http", const char* secure = "https", const char* defaultscheme = "http", const char* defaultpath = "/") {
            using R = commonlib2::Reader<std::string>;
            R(arg.url).readwhile(commonlib2::parse_url, ctx.url);
            if (!ctx.url.succeed) return false;
            if (!ctx.url.scheme.size()) {
                ctx.url.scheme = defaultscheme;
            }
            else {
                if (ctx.url.scheme != normal && ctx.url.scheme != secure) {
                    return false;
                }
            }
            if (!ctx.url.path.size()) {
                ctx.url.path = defaultpath;
            }
            if (!arg.urlencoded) {
                commonlib2::URLEncodingContext<std::string> encctx;
                encctx.no_escape = {':'};  //temporary solusion
                encctx.path = true;
                R(ctx.url.path).readwhile(ctx.path, commonlib2::url_encode, &encctx);
                if (encctx.failed) return false;
                encctx.query = true;
                encctx.path = false;
                R(ctx.url.query).readwhile(ctx.query, commonlib2::url_encode, &encctx);
            }
            else {
                ctx.path = ctx.url.path;
                ctx.query = ctx.url.query;
            }
            if (ctx.url.port.size()) {
                R(ctx.url.port) >> ctx.port;
            }
            return true;
        }
        static void fill_urlprefix(const std::string& host, HttpOpenContext& arg, std::string& result, const char* scheme) {
            auto r = commonlib2::Reader(arg.url);
            if (r.ahead("//")) {
                result = scheme;
                result += ":";
            }
            else if (r.achar() == '/') {
                result = scheme;
                result += "://";
                result += host;
            }
        }

        static OpenErr reopen_detail(std::shared_ptr<HttpClientConn>& conn, HttpRequestContext& ctx, HttpOpenContext& arg) {
            auto res = reopen_tcp_conn(conn->borrow(), ctx, arg, "\x08http/1.1", 9);
            if (!res && res != OpenError::needless_to_reopen) return res;
            conn->host = ctx.host_with_port();
            conn->path_ = ctx.path;
            conn->query_ = ctx.query;
            return true;
        }

       public:
        static std::shared_ptr<HttpClientConn> open(HttpOpenContext& arg) {
            HttpRequestContext ctx;
            if (!setuphttp(arg, ctx)) {
                arg.err = OpenError::parse_url;
                return nullptr;
            }
            std::shared_ptr<Conn> conn;
            conn = open_tcp_conn(ctx, arg, "\x08http/1.1", 9);
            if (!conn) return nullptr;
            return init_object(conn, ctx);
        }

        static OpenErr reopen(std::shared_ptr<HttpClientConn>& conn, HttpOpenContext& arg) {
            if (!conn || !arg.url) return false;
            std::string urlstr;
            fill_urlprefix(conn->host, arg, urlstr, conn->borrow()->get_ssl() ? "https" : "http");
            urlstr += arg.url;
            arg.url = urlstr.c_str();
            HttpRequestContext ctx;
            if (!setuphttp(arg, ctx)) {
                arg.err = OpenError::parse_url;
                return OpenError::parse_url;
            }
            return reopen_detail(conn, ctx, arg);
        }

        static std::shared_ptr<HttpServerConn> serve(Server& sv, unsigned short port = 80, size_t timeout = 10, IPMode mode = IPMode::both) {
            std::shared_ptr<Conn> conn = TCP::serve(sv, port, timeout, "http", true, mode);
            if (!conn) return nullptr;
            return std::make_shared<HttpServerConn>(std::move(conn));
        }
    };
}  // namespace socklib
