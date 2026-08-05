#include "asiortc.hpp"
#include "asiortc/queue_track.hpp"

#include "asioice/detail/scope_guard.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
namespace asioice {
namespace net = boost::asio;
}
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
#else
#error "Requires Boost.Asio"
#endif

#include "json.hpp"

#include <chrono>
#include <exec/start_detached.hpp>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8087;

// ── WebSocket helpers ──────────────────────────────────────────

static task<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] = co_await ws.async_write(net::buffer(d),
                                           net::as_tuple(utils::use_sender));
    if (ec)
        std::cerr << "ws send err: " << ec.message() << '\n';
}

static task<nlohmann::json> ws_recv(ws_t &ws) {
    beast::flat_buffer buf;
    auto [ec, n] =
        co_await ws.async_read(buf, net::as_tuple(utils::use_sender));
    if (ec)
        throw std::runtime_error("ws recv: " + ec.message());
    auto j = nlohmann::json::parse(beast::buffers_to_string(buf.data()));
    buf.clear();
    co_return j;
}

// ── Common negotiation helpers ─────────────────────────────────

static task<void> wait_until_ice_gather(peer_connection &conn,
                                        net::steady_timer &timer,
                                        int timeout_s = 10) {
    for (int i = 0; i < timeout_s && conn.ice_gathering_state() !=
                                         ice_gathering_state_t::complete;
         ++i) {
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(utils::use_sender);
    }
    if (conn.ice_gathering_state() != ice_gathering_state_t::complete)
        throw std::runtime_error{"C++ gathering timeout"};
}

static task<bool> wait_until_connected(peer_connection &conn,
                                       net::steady_timer &timer,
                                       int timeout_s = 15) {
    for (int i = 0; i < timeout_s; ++i) {
        if (conn.connection_state() == connection_state_t::connected)
            co_return true;
        if (conn.connection_state() == connection_state_t::failed)
            co_return false;
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(utils::use_sender);
    }
    co_return conn.connection_state() == connection_state_t::connected;
}

// ── Test cases (one function per case) ─────────────────────────

static task<nlohmann::json> test_case_1_basic_video(net::io_context &ctx,
                                                    ws_t &ws) {
    nlohmann::json r, cpp;
    r["case"] = 1;
    try {
        peer_connection conn(ctx.get_executor());
        net::steady_timer timer(ctx);

        auto msg = co_await ws_recv(ws);
        if (msg["type"] != "offer")
            throw std::runtime_error("expected offer");
        auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer").value();

        auto tr = conn.add_transceiver(
            media_kind::video,
            {.direction = sdp_direction::sendrecv, .streams = {"test-video"}});
        tr.sender().set_track(std::make_shared<queue_track>(media_kind::video));

        co_await conn.set_remote_description(std::move(offer));
        co_await conn.set_local_description(co_await conn.create_answer());

        co_await wait_until_ice_gather(conn, timer);
        co_await ws_send(ws, {{"type", "answer"},
                              {"sdp", conn.local_description()->to_string()}});

        bool ok = co_await wait_until_connected(conn, timer);

        cpp["mid"] = tr.mid();
        cpp["codec_count"] = tr.codecs().size();
        cpp["ssrc"] = tr.sender().ssrc(0);
        cpp["direction"] = static_cast<int>(tr.direction());
        bool pass = ok && !tr.mid().empty() && tr.codecs().size() > 0;
        cpp["pass"] = pass;
        cpp["connection_state"] = ok ? "connected" : "not_connected";
        r["pass"] = pass;
        r["details"] = {{"cpp", cpp}};
        std::cout << "[test:1] verify: mid=" << tr.mid()
                  << " codecs=" << tr.codecs().size()
                  << " ssrc=" << tr.sender().ssrc(0) << " pass=" << pass
                  << '\n';
    } catch (const std::exception &e) {
        r["pass"] = false;
        r["details"] = {{"cpp", {{"error", e.what()}}}};
    }
    co_return r;
}

static task<nlohmann::json> test_case_2_audio_video(net::io_context &ctx,
                                                    ws_t &ws) {
    nlohmann::json r, cpp;
    r["case"] = 2;
    try {
        peer_connection conn(ctx.get_executor());
        net::steady_timer timer(ctx);

        auto msg = co_await ws_recv(ws);
        if (msg["type"] != "offer")
            throw std::runtime_error("expected offer");
        auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer").value();

        std::vector<rtp_transceiver_interface> trs;
        trs.push_back(conn.add_transceiver(
            media_kind::audio,
            {.direction = sdp_direction::sendrecv, .streams = {"test-audio"}}));
        trs.push_back(conn.add_transceiver(
            media_kind::video,
            {.direction = sdp_direction::sendrecv, .streams = {"test-video"}}));
        trs[0].sender().set_track(
            std::make_shared<queue_track>(media_kind::audio));
        trs[1].sender().set_track(
            std::make_shared<queue_track>(media_kind::video));

        co_await conn.set_remote_description(std::move(offer));
        co_await conn.set_local_description(co_await conn.create_answer());

        co_await wait_until_ice_gather(conn, timer);
        co_await ws_send(ws, {{"type", "answer"},
                              {"sdp", conn.local_description()->to_string()}});

        bool ok = co_await wait_until_connected(conn, timer);

        bool pass = ok && trs.size() == 2;
        for (size_t i = 0; i < trs.size(); i++) {
            nlohmann::json t;
            t["mid"] = trs[i].mid();
            t["codec_count"] = trs[i].codecs().size();
            cpp["transceivers"].push_back(t);
            if (trs[i].mid().empty())
                pass = false;
        }
        cpp["connection_state"] = ok ? "connected" : "not_connected";
        cpp["pass"] = pass;
        r["pass"] = pass;
        r["details"] = {{"cpp", cpp}};
        std::cout << "[test:2] verify: " << trs.size()
                  << " transceivers, pass=" << pass << '\n';
    } catch (const std::exception &e) {
        r["pass"] = false;
        r["details"] = {{"cpp", {{"error", e.what()}}}};
    }
    co_return r;
}

static task<nlohmann::json> test_case_3_dc_echo(net::io_context &ctx,
                                                ws_t &ws) {
    nlohmann::json r, cpp;
    r["case"] = 3;
    try {
        peer_connection conn(ctx.get_executor());
        net::steady_timer timer(ctx);

        data_channel_interface remote_dc;
        bool dc_received = false;
        conn.on_data_channel([&](data_channel_interface dc) {
            remote_dc = dc;
            dc_received = true;
            std::cout << "[test:3] on_data_channel\n";
        });

        auto msg = co_await ws_recv(ws);
        if (msg["type"] != "offer")
            throw std::runtime_error("expected offer");
        auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer").value();

        co_await conn.set_remote_description(std::move(offer));
        {
            auto answer = co_await conn.create_answer();
            auto str = answer.to_string();
            std::cout << "\n[ANSWER]:\n" << str << "\n\n";
            co_await conn.set_local_description(std::move(answer));
        }
        co_await wait_until_ice_gather(conn, timer);
        co_await ws_send(ws, {{"type", "answer"},
                              {"sdp", conn.local_description()->to_string()}});

        bool ok = co_await wait_until_connected(conn, timer);
        if (!ok) {
            cpp["connection_state"] = "not_connected";
            cpp["pass"] = false;
            r["pass"] = false;
            r["details"] = {{"cpp", cpp}};
            co_return r;
        }

        for (int i = 0; i < 20 && !dc_received; ++i) {
            timer.expires_after(std::chrono::milliseconds(500));
            co_await timer.async_wait(utils::use_sender);
        }
        if (!dc_received) {
            cpp["dc_received"] = false;
            cpp["pass"] = false;
            r["pass"] = false;
            r["details"] = {{"cpp", cpp}};
            co_return r;
        }

        auto dc_msg = co_await remote_dc.read();
        std::string text{dc_msg.text_data()};
        cpp["dc_recv_text"] = text;

        co_await remote_dc.send(text);
        cpp["dc_echo_sent"] = true;

        cpp["connection_state"] = "connected";
        cpp["pass"] = true;
        r["pass"] = true;
        r["details"] = {{"cpp", cpp}};
        std::cout << "[test:3] DC echo: recv='" << text
                  << "' echo_sent pass=true\n";
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(utils::use_sender);
    } catch (const std::exception &e) {
        r["pass"] = false;
        r["details"] = {{"cpp", {{"error", e.what()}}}};
    }
    co_return r;
}

static task<nlohmann::json> test_case_4_cxx_offerer(net::io_context &ctx,
                                                    ws_t &ws) {
    nlohmann::json r, cpp;
    r["case"] = 4;
    try {
        peer_connection conn(ctx.get_executor());
        net::steady_timer timer(ctx);

        auto tr = conn.add_transceiver(media_kind::video,
                                       {.direction = sdp_direction::sendrecv,
                                        .streams = {"cxx-offerer-stream"}});
        tr.sender().set_track(std::make_shared<queue_track>(media_kind::video));

        int on_track_call = 0;
        conn.on_track([&](auto &&...) { ++on_track_call; });

        auto req = co_await ws_recv(ws);
        if (req["type"] != "request_offer")
            throw std::runtime_error("expected request_offer");

        auto offer = co_await conn.create_offer();
        co_await conn.set_local_description(std::move(offer));
        co_await wait_until_ice_gather(conn, timer);
        auto local_desc = conn.local_description()->to_string();
        std::cout << "[test:4] C++ offer:\n" << local_desc << '\n';
        co_await ws_send(ws,
                         {{"type", "offer"}, {"sdp", std::move(local_desc)}});

        auto ans = co_await ws_recv(ws);
        if (ans["type"] != "answer")
            throw std::runtime_error("expected answer");
        std::cout << "[test:4] JS answer:\n"
                  << ans["sdp"].get<std::string>() << '\n';
        co_await conn.set_remote_description(
            parse_sdp(ans["sdp"].get<std::string>(), "answer").value());
        if (on_track_call != 1) {
            throw std::runtime_error{"on_track should invoke 2 times"};
        }
        bool ok = co_await wait_until_connected(conn, timer);

        bool pass = ok && !tr.mid().empty();
        cpp["connection_state"] = ok ? "connected" : "not_connected";
        cpp["mid"] = tr.mid();
        cpp["ssrc"] = tr.sender().ssrc(0);
        cpp["pass"] = pass;
        r["pass"] = pass;
        r["details"] = {{"cpp", cpp}};
        std::cout << "[test:4] C++ offerer verify: mid=" << tr.mid()
                  << " pass=" << pass << '\n';
    } catch (const std::exception &e) {
        r["pass"] = false;
        r["details"] = {{"cpp", {{"error", e.what()}}}};
    }
    co_return r;
}

static task<nlohmann::json> test_case_5_multi_transceiver(net::io_context &ctx,
                                                          ws_t &ws) {
    nlohmann::json r, cpp;
    r["case"] = 5;
    try {
        peer_connection conn(ctx.get_executor());
        net::steady_timer timer(ctx);
        bool pass = false;

        auto msg = co_await ws_recv(ws);
        if (msg["type"] != "offer")
            throw std::runtime_error("expected offer");
        auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer").value();

        std::vector<rtp_transceiver_interface> trs;
        trs.push_back(conn.add_transceiver(
            media_kind::video,
            {.direction = sdp_direction::sendrecv, .streams = {"cpp_stream"}}));
        trs.push_back(conn.add_transceiver(
            media_kind::audio,
            {.direction = sdp_direction::sendrecv, .streams = {"cpp_stream"}}));
        trs[0].sender().set_track(
            std::make_shared<queue_track>(media_kind::video));
        trs[1].sender().set_track(
            std::make_shared<queue_track>(media_kind::audio));

        std::vector<std::string> streams;
        int on_track_call = 0;
        conn.on_track([&](rtp_receiver_interface receiver,
                          std::shared_ptr<media_track> track,
                          std::vector<std::string> msids,
                          rtp_transceiver_interface tr) {
            std::move(msids.begin(), msids.end(), std::back_inserter(streams));
            ++on_track_call;
        });

        co_await conn.set_remote_description(std::move(offer));
        {
            if (on_track_call != 2)
                throw std::runtime_error(
                    "on_track should have been called twice");
            if (streams.empty())
                throw std::runtime_error("on_track msids empty");
            std::sort(streams.begin(), streams.end());
            streams.erase(std::unique(streams.begin(), streams.end()),
                          streams.end());
            for (const auto &stream : streams)
                std::cout << "[test:5] on_track stream: " << stream << '\n';
            if (streams.size() != 1)
                throw std::runtime_error(
                    "on_track msids should have one unique msid");
        }
        co_await conn.set_local_description(co_await conn.create_answer());

        co_await wait_until_ice_gather(conn, timer);
        co_await ws_send(ws, {{"type", "answer"},
                              {"sdp", conn.local_description()->to_string()}});

        bool ok = co_await wait_until_connected(conn, timer);

        pass = ok && trs.size() == 2;
        std::set<std::string> mids;
        for (size_t i = 0; i < trs.size(); i++) {
            nlohmann::json t;
            t["mid"] = trs[i].mid();
            t["ssrc"] = trs[i].sender().ssrc(0);
            mids.insert(trs[i].mid());
            cpp["transceivers"].push_back(t);
            if (trs[i].mid().empty())
                pass = false;
        }
        pass = pass && mids.size() == 2;
        cpp["connection_state"] = ok ? "connected" : "not_connected";
        cpp["pass"] = pass;
        cpp["streams"] = streams.front();
        r["pass"] = pass;
        r["details"] = {{"cpp", cpp}};
        std::cout << "[test:5] multi transceiver verify: " << trs.size()
                  << " trs, unique_mids=" << mids.size() << " pass=" << pass
                  << '\n';
    } catch (const std::exception &e) {
        r["pass"] = false;
        r["details"] = {{"cpp", {{"error", e.what()}}}};
    }
    co_return r;
}

// ── Test case registry ─────────────────────────────────────────

using test_case_fn =
    std::function<asiortc::task<nlohmann::json>(net::io_context &, ws_t &)>;

static const std::map<int, test_case_fn> test_cases = {
    {1, test_case_1_basic_video},       {2, test_case_2_audio_video},
    {3, test_case_3_dc_echo},           {4, test_case_4_cxx_offerer},
    {5, test_case_5_multi_transceiver},
};

// ── Test session (one per WebSocket) ───────────────────────────

static task<void> test_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "test_session: exited\n"; });

    std::cout << "WS connected (asiortc negotiation test)\n";

    while (true) {
        auto msg = co_await ws_recv(*ws);
        std::string type = msg["type"];

        if (type == "start_test") {
            int case_id = msg["case"].get<int>();
            std::cout << "\n=== [test:" << case_id << "] START ===\n";

            co_await ws_send(*ws, {{"type", "status"}, {"status", "ready"}});

            auto it = test_cases.find(case_id);
            nlohmann::json result;
            if (it != test_cases.end()) {
                result = co_await it->second(ctx, *ws);
            } else {
                result["case"] = case_id;
                result["pass"] = false;
                result["details"] = {{"cpp", {{"error", "unknown case"}}}};
            }
            result["type"] = "result";
            result["name"] = msg.value("name", "unknown");
            co_await ws_send(*ws, result);

            std::cout << "=== [test:" << case_id << "] DONE: pass=" << result
                      << " ===\n\n";
        } else if (type == "done") {
            break;
        }
    }
}

// ── HTTP server + WebSocket upgrade ────────────────────────────

static task<void> http_session(net::io_context &ctx,
                               net::ip::tcp::socket sock) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    auto [ec, n] = co_await http::async_read(sock, buf, req, utils::use_sender);
    if (ec) {
        std::cerr << "http read: " << ec.message() << '\n';
        co_return;
    }

    if (websocket::is_upgrade(req)) {
        auto ws = std::make_shared<ws_t>(beast::tcp_stream(std::move(sock)));
        auto [wec] =
            co_await ws->async_accept(req, net::as_tuple(utils::use_sender));
        if (wec) {
            std::cerr << "ws accept: " << wec.message() << '\n';
            co_return;
        }
        co_await test_session(ctx, std::move(ws));
    } else {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/html");
        static constexpr char html[] = {
#embed "index.html"
            , '\0'};
        res.body() = html;
        res.prepare_payload();
        auto [sec, _] = co_await http::async_write(
            sock, res, net::as_tuple(utils::use_sender));
        if (sec)
            std::cerr << "http write: " << sec.message() << '\n';
    }
}

static task<void> listener(net::io_context &ctx) {
    net::ip::tcp::acceptor acc(
        ctx, net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), PORT));
    std::cout << "asiortc negotiation test server on http://localhost:" << PORT
              << "/\n";
    while (true) {
        auto [ec, sock] =
            co_await acc.async_accept(net::as_tuple(utils::use_sender));
        if (ec)
            continue;
        exec::start_detached(http_session(ctx, std::move(sock)));
    }
}

int main() {
    std::cout << std::unitbuf;
    net::io_context ctx;
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}
