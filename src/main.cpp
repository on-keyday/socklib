
#include <iostream>

#include "http.h"

#include <reader.h>
#include <net_helper.h>

#include <coroutine>
#include <mutex>

#include <deque>

/*
template <class T>
struct task {
   private:
    struct promise_type {
        T value;

        auto get_return_object() {
            return task{*this};
        };

        auto initial_suspend() {
            return std::suspend_always{};
        }
        auto final_suspend() {
            return std::suspend_always{};
        }
        auto yield_value(T v) {
            value = v;
            return std::suspend_always{};
        }
        void return_void() {}
        void unhandled_exception() {
            std::terminate();
        }
    };
    using coro_handle = std::coroutine_handle<promise_type>;
    coro_handle co;
    task(promise_type& in)
        : co(coro_handle::from_promise(co)) {}

    task(task&& rhs)
        : coro_(std::exchange(rhs.co, nullptr)) {}

    ~task() {
        if (co) co.destroy();
    }
};
*/
using namespace commonlib2;

void httprecv(std::shared_ptr<socklib::HttpClientConn>& conn, bool res, const char* cacert, void (*callback)(decltype(conn))) {
    if (!res) {
        std::cout << "failed to recv\n";
        return;
    }
    unsigned short statuscode = 0;
    Reader<std::string>(conn->response().find(":status")->second) >> statuscode;
    std::cout << conn->url() << "\n";
    std::cout << conn->ipaddress() << "\n";
    std::cout << statuscode << " " << conn->response().find(":phrase")->second << "\n";
    if (statuscode >= 301 && statuscode <= 308) {
        if (auto found = conn->response().find("location"); found != conn->response().end()) {
            if (socklib::Http::reopen(conn, found->second.c_str(), true, cacert)) {
                std::cout << "redirect\n";
                conn->send(conn->method().c_str());
                conn->recv(httprecv, false, cacert, callback);
                return;
            }
        }
    }
    callback(conn);
}

void client_test() {
    auto cacert = "D:/CommonLib/netsoft/cacert.pem";
    auto conn = socklib::Http::open("gmail.com", false, cacert);
    if (!conn) {
        std::cout << "connection failed\n";
        std::cout << "last error:" << WSAGetLastError();
        return;
    }
    const char payload[] = "Hello World";
    conn->send("GET");

    conn->recv_async(httprecv, false, cacert, [](auto& conn) {
        std::cout << conn->response().find(":body")->second;
    });

    while (conn->wait()) {
        Sleep(5);
    }
    conn->close();
}

std::mutex mut;

std::deque<std::shared_ptr<socklib::HttpServerConn>> que;

int main(int, char**) {
    auto maxth = std::thread::hardware_concurrency();
    if (maxth == 0) {
        maxth = 4;
    }
    uint32_t i = 0;
    for (i = 0; i < maxth; i++) {
        try {
            std::thread(
                []() {
                    auto id = std::this_thread::get_id();
                    while (true) {
                        std::shared_ptr<socklib::HttpServerConn> conn;
                        mut.lock();
                        if (que.size()) {
                            conn = std::move(que.front());
                            que.pop_front();
                        }
                        mut.unlock();
                        if (!conn) {
                            Sleep(10);
                            continue;
                        }
                        auto begin = std::chrono::system_clock::now();
                        auto print_time = [&](auto end) {
                            std::cout << std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
                        };
                        if (!conn->recv()) {
                            return;
                        }
                        auto rec = std::chrono::system_clock::now();
                        auto path = conn->request().find(":path")->second;
                        unsigned short status = 0;
                        auto meth = conn->request().find(":method")->second;
                        if (meth != "GET" && meth != "HEAD") {
                            const char c[] = "405 method not allowed";
                            conn->send(405, "Method Not Allowed", {}, c, sizeof(c) - 1);
                            status = 405;
                        }
                        if (!status && path != "/") {
                            const char c[] = "404 not found";
                            conn->send(404, "Not Found", {}, c, sizeof(c) - 1);
                            status = 404;
                        }
                        if (!status) {
                            if (meth == "GET") {
                                conn->send(200, "OK", {}, "It Works!", 9);
                            }
                            else {
                                conn->send();
                            }
                            status = 200;
                        }
                        std::cout << "thread-" << id;
                        std::cout << "|" << conn->ipaddress() << "|\"";
                        std::cout << path << "\"|";
                        std::cout << meth << "|";
                        std::cout << status << "|";
                        print_time(rec);
                        std::cout << "|";
                        print_time(std::chrono::system_clock::now());
                        std::cout << "|\n";
                    }
                })
                .detach();
        } catch (...) {
            std::cout << "thread make suspended\n";
            break;
        }
    }

    std::cout << "thread count:" << maxth << "\n";

    socklib::Server sv;
    while (true) {
        auto res = socklib::Http::serve(sv, 8090);
        if (!res) {
            std::cout << "error occured\n";
            continue;
        }
        mut.lock();
        que.push_back(std::move(res));
        mut.unlock();
    }
}
