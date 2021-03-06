/*
    socklib - simple socket library
    Copyright (c) 2021 on-keyday (https://github.com/on-keyday)
    Released under the MIT license
    https://opensource.org/licenses/mit-license.php
*/

#pragma once
#include "http_base.h"

namespace socklib {
    namespace v2 {

        template <class String, class Header, class Body>
        struct HttpHeaderWriter {
            using base_t = HttpBase<String, Header, Body>;
            using urlparser_t = URLParser<String, Header, Body>;
            using string_t = typename base_t::string_t;
            using request_t = typename base_t::request_t;
            using errorhandle_t = ErrorHandler<String, Header, Body>;

            static bool write_to_conn(std::shared_ptr<InetConn>& conn, string_t& towrite, request_t& req, CancelContext* cancel) {
                WriteContext w;
                w.ptr = towrite.c_str();
                w.bufsize = towrite.size();
                return errorhandle_t::write_to_conn(conn, w, req, cancel);
            }

            static bool write_header_common(string_t& towrite, Header& header, Body& body, request_t& req, bool need_len) {
                for (auto& h : header) {
                    if (auto e = base_t::is_valid_field(h, req); e < 0) {
                        return false;
                    }
                    else if (e == 0) {
                        continue;
                    }
                    towrite += h.first;
                    towrite += ": ";
                    towrite += h.second;
                    towrite += "\r\n";
                }
                if (body.size() || need_len) {
                    if (any(req.flag & RequestFlag::header_is_small)) {
                        towrite += "content-length: ";
                    }
                    else {
                        towrite += "Content-Length: ";
                    }
                    towrite += std::to_string(body.size()).c_str();
                    towrite += "\r\n\r\n";
                    towrite.append(body.data(), body.size());
                }
                else {
                    towrite += "\r\n";
                }
                return true;
            }

            static bool write_request(string_t& towrite, request_t& req) {
                towrite = req.method;
                towrite += ' ';
                base_t::write_path(towrite, req);
                towrite += ' ';
                towrite += "HTTP/1.1\r\n";
                if (any(req.flag & RequestFlag::header_is_small)) {
                    towrite += "host: ";
                }
                else {
                    towrite += "Host: ";
                }
                towrite += urlparser_t::host_with_port(req.parsed);
                towrite += "\r\n";
                bool need_len = any(req.flag & RequestFlag::need_len);
                return write_header_common(towrite, req.request, req.requestbody, req, need_len);
            }

            static bool write_response(string_t& towrite, request_t& req) {
                if (req.header_version == 9) {
                    towrite = string_t(req.responsebody.data(), req.responsebody.size());
                }
                else if (req.header_version == 10) {
                    towrite += "HTTP/1.0 ";
                }
                else {
                    towrite += "HTTP/1.1 ";
                }
                towrite += std::to_string(req.statuscode).c_str();
                towrite += ' ';
                towrite += reason_phrase(req.statuscode);
                towrite += "\r\n";
                bool need_len = !any(req.flag & RequestFlag::not_need_len);
                return write_header_common(towrite, req.response, req.responsebody, req, need_len);
            }
        };

        struct HttpBodyInfo {
            bool has_len = false;
            size_t size = 0;
            bool chunked = false;
            bool close_conn = false;
        };

        template <class String, class Header, class Body>
        struct HttpHeaderReader {
            using util_t = HttpUtil<String>;
            using request_t = RequestContext<String, Header, Body>;
            using string_t = String;
            using header_t = Header;
            using body_t = Body;

            template <class Buf>
            static bool parse_header(request_t& req, commonlib2::Reader<Buf>& r, HttpBodyInfo& body, header_t& header) {
                using commonlib2::str_eq, commonlib2::getline, commonlib2::split;
                while (true) {
                    auto tmp = getline<string_t>(r, false);
                    if (tmp.size() == 0) break;
                    auto f = split(tmp, ":", 1);
                    if (f.size() < 2) return false;
                    while (f[1].size() && (f[1][0] == ' ' || f[1][0] == '\t')) f[1].erase(0, 1);
                    //std::transform(f[0].begin(), f[0].end(), f[0].begin(), [](char c) { return std::tolower((unsigned char)c); });
                    if (str_eq(f[0], "host", util_t::header_cmp)) {
                        auto h = split(f[1], ":", 1);
                        if (h.size() == 0) continue;
                        req.parsed.host = h[0];
                        if (h.size() == 2) {
                            req.parsed.port = h[1];
                        }
                    }
                    else if (str_eq(f[0], "connection", util_t::header_cmp) && f[1].find("close") != ~0) {
                        body.close_conn = true;
                    }
                    else if (!body.chunked && str_eq(f[0], "transfer-encoding", util_t::header_cmp) && f[1].find("chunked") != ~0) {
                        body.chunked = true;
                    }
                    else if (!body.has_len && str_eq(f[0], "content-length", util_t::header_cmp)) {
                        body.has_len = true;
                        commonlib2::Reader(f[1]) >> body.size;
                    }
                    header.emplace(f[0], f[1]);
                }
                return true;
            }

            template <class Buf>
            static bool parse_request(request_t& req, commonlib2::Reader<Buf>& r, HttpBodyInfo& body) {
                using commonlib2::str_eq, commonlib2::getline, commonlib2::split;
                auto status = split(getline(r, false), " ");
                if (status.size() < 2) return false;
                req.method = status[0];
                auto h = split(status[1], "?", 1);
                req.parsed.path = h[0];
                if (h.size() == 2) {
                    req.parsed.query = "?";
                    req.parsed.query += h[1];
                }
                if (status.size() == 3) {
                    if (status[2] == "HTTP/1.0") {
                        req.header_version = 10;
                    }
                    else if (status[2] == "HTTP/1.1") {
                        req.header_version = 11;
                    }
                }
                else {
                    if (!any(req.flag & RequestFlag::allow_http09)) {
                        req.err = HttpError::invalid_request_format;
                        return false;
                    }
                    req.header_version = 9;
                    return true;
                }
                return parse_header(req, r, body, req.request);
            }

            template <class Buf>
            static bool parse_response(request_t& req, commonlib2::Reader<Buf>& r, HttpBodyInfo& body) {
                using commonlib2::str_eq, commonlib2::getline, commonlib2::split;
                auto status = split(getline(r, false), " ", 2);
                if (status.size() < 2) return false;
                if (status[0] == "HTTP/1.1") {
                    req.header_version = 11;
                }
                else if (status[0] == "HTTP/1.0") {
                    req.header_version = 10;
                }
                commonlib2::Reader(status[1]) >> req.statuscode;
                return parse_header(req, r, body, req.response);
            }

            static bool read_body(request_t& req, HttpBodyInfo& bodyinfo, string_t& rawdata, body_t& body) {
                commonlib2::Reader<string_t&> r(rawdata);
                if (bodyinfo.chunked) {
                    while (rawdata.size()) {
                        r.expect("\r\n");
                        using commonlib2::getline;
                        bool noext = false;
                        string_t num = "0x";
                        getline(r, num, false, &noext);
                        if (noext) {
                            return true;
                        }
                        size_t chunksize = 0;
                        commonlib2::Reader(num) >> chunksize;
                        if (chunksize == 0) {
                            r.expect("\r\n");
                            rawdata.erase(0, r.readpos());
                            req.phase = RequestPhase::body_recved;
                            return false;
                        }
                        if (r.readable() < chunksize) {
                            return true;
                        }
                        size_t nowsize = body.size();
                        body.resize(nowsize + chunksize);
                        if (r.read_byte(body.data() + nowsize, chunksize, commonlib2::translate_byte_as_is, true) < chunksize) {
                            req.err = HttpError::read_body;
                            req.phase = RequestPhase::error;
                            return false;
                        }
                        r.expect("\r\n");
                        rawdata.erase(0, r.readpos());
                        r.seek(0);
                    }
                }
                else if (bodyinfo.has_len) {
                    if (rawdata.size() >= bodyinfo.size) {
                        body.resize(bodyinfo.size);
                        if (r.read_byte(body.data(), bodyinfo.size, commonlib2::translate_byte_as_is, true) < bodyinfo.size) {
                            req.err = HttpError::read_body;
                            req.phase = RequestPhase::error;
                        }
                        else {
                            req.phase = RequestPhase::body_recved;
                        }
                        rawdata.erase(0, bodyinfo.size);
                        return false;
                    }
                }
                else {
                    if (any(RequestFlag::no_read_body & req.flag)) {
                        req.phase = RequestPhase::body_recved;
                        return false;
                    }
                    if (rawdata.size() == 0) {
                        req.phase = RequestPhase::body_recved;
                        return false;
                    }
                    for (size_t i = 0; i < rawdata.size(); i++) {
                        body.push_back(rawdata[i]);
                    }
                    rawdata.clear();
                }
                return true;
            }
        };

        template <class String, class Header, class Body>
        struct Http1ReadContext : IReadContext {
            using base_t = HttpBase<String, Header, Body>;
            using request_t = typename base_t::request_t;
            using string_t = typename base_t::string_t;
            using httpparser_t = HttpHeaderReader<String, Header, Body>;
            using errhandle_t = ErrorHandler<String, Header, Body>;

            Http1ReadContext(request_t& r)
                : req(r) {}

            bool nolen = false;
            request_t& req;
            bool eos = false;
            HttpBodyInfo bodyinfo;
            string_t rawdata;
            bool server = false;

            virtual bool require() override {
                return !eos;
            }

            virtual void on_error(std::int64_t errorcode, CancelContext* cancel, const char* msg) override {
                errhandle_t::on_error(req, errorcode, cancel, msg);
            }

            virtual void append(const char* read, size_t size) override {
                commonlib2::Reader<string_t&> r(rawdata);
                size_t last = rawdata.size();
                rawdata.append(read, size);
                if (req.phase == RequestPhase::request_recving || req.phase == RequestPhase::response_recving) {
                    r.seek(last);
                    auto parse_header = [&, this] {
                        r.seek(0);
                        if (server) {
                            if (!httpparser_t::parse_request(req, r, bodyinfo)) {
                                eos = true;
                                req.phase = RequestPhase::error;
                                return false;
                            }
                            if (req.header_version == 9) {
                                eos = true;
                                req.phase = RequestPhase::body_recved;
                            }
                            else {
                                req.phase = RequestPhase::request_recved;
                            }
                        }
                        else {
                            if (!httpparser_t::parse_response(req, r, bodyinfo)) {
                                eos = true;
                                req.phase = RequestPhase::error;
                                return false;
                            }
                            req.phase = RequestPhase::response_recved;
                        }
                        rawdata.erase(0, r.readpos());
                        nolen = !bodyinfo.chunked && !bodyinfo.has_len;
                        return true;
                    };
                    while (!r.ceof()) {
                        if (r.achar() == '\r' || r.achar() == '\n') {
                            if (r.expect("\r\n\r\n") || r.expect("\n\n")) {
                                if (!parse_header()) {
                                    return;
                                }
                                break;
                            }
                        }
                        r.increment();
                    }
                }
                if (req.phase == RequestPhase::request_recved) {
                    if (!httpparser_t::read_body(req, bodyinfo, rawdata, req.requestbody)) {
                        eos = true;
                    }
                }
                else if (req.phase == RequestPhase::response_recved) {
                    if (!httpparser_t::read_body(req, bodyinfo, rawdata, req.responsebody)) {
                        eos = true;
                    }
                }
            }
        };

        template <class String, class Header, class Body>
        struct Http1Client {
            using base_t = HttpBase<String, Header, Body>;
            using string_t = typename base_t::string_t;
            using request_t = typename base_t::request_t;
            using urlparser_t = typename base_t::urlparser_t;
            using headerwriter_t = HttpHeaderWriter<String, Header, Body>;
            using readcontext_t = Http1ReadContext<String, Header, Body>;

            static bool request(std::shared_ptr<InetConn>& conn, request_t& req, CancelContext* cancel = nullptr) {
                if (!conn) return false;
                if (req.resolved_version != 1) {
                    req.err = HttpError::not_accept_version;
                    return false;
                }
                if (req.method.size() == 0) {
                    req.method = "GET";
                }
                string_t towrite;
                if (!headerwriter_t::write_request(towrite, req)) {
                    req.phase = RequestPhase::error;
                    return false;
                }
                if (!headerwriter_t::write_to_conn(conn, towrite, req, cancel)) {
                    req.phase = RequestPhase::error;
                }
                req.phase = RequestPhase::request_sent;
                return true;
            }

            static bool response(std::shared_ptr<InetConn>& conn, readcontext_t& read, CancelContext* cancel = nullptr) {
                if (!conn) return false;
                if (read.req.phase == RequestPhase::idle) {
                    read.req.err = HttpError::invalid_phase;
                    return false;
                }
                if (read.req.phase == RequestPhase::request_sent) {
                    read.req.phase = RequestPhase::response_recving;
                }
                read.nolen = false;
                read.server = false;
                if (!conn->read(read, cancel)) {
                    return read.nolen;
                }
                return true;
            }
        };

        template <class String, class Header, class Body>
        struct Http1Server {
            using base_t = HttpBase<String, Header, Body>;
            using request_t = typename base_t::request_t;
            using httpwriter_t = HttpHeaderWriter<String, Header, Body>;
            using readcontext_t = Http1ReadContext<String, Header, Body>;
            using string_t = String;

            static bool request(std::shared_ptr<InetConn>& conn, readcontext_t& read, CancelContext* cancel = nullptr) {
                if (!conn) return false;
                if (read.req.phase == RequestPhase::idle) {
                    read.req.phase = RequestPhase::request_recving;
                }
                read.nolen = false;
                read.server = true;
                if (!conn->read(read, cancel)) {
                    return read.nolen;
                }
                ConnStat stat;
                conn->stat(stat);
                if (any(stat.status & ConnStatus::secure)) {
                    read.req.parsed.scheme = "https";
                }
                else {
                    read.req.parsed.scheme = "http";
                }
                return true;
            }

            static bool response(std::shared_ptr<InetConn>& conn, request_t& req, CancelContext* cancel = nullptr) {
                if (!conn) return false;
                if (req.statuscode < 100 || req.statuscode > 599) {
                    req.statuscode = 500;
                }
                if (req.phase != RequestPhase::body_recved) {
                    req.err = HttpError::invalid_phase;
                    return false;
                }
                string_t towrite;
                if (!httpwriter_t::write_response(towrite, req)) {
                    req.phase = RequestPhase::error;
                    return false;
                }
                if (!httpwriter_t::write_to_conn(conn, towrite, req, cancel)) {
                    req.phase = RequestPhase::error;
                }
                req.phase = RequestPhase::idle;
                return false;
            }
        };

    }  // namespace v2
}  // namespace socklib
