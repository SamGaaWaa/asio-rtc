#include "sdp.hpp"
#include "asioice/detail/string_utils.hpp"
#include "samlog.hpp"

#include "ctre.hpp"

#include <algorithm>
#include <ranges>
#include <charconv>
#include <cstdint>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <format>
#include <vector>
#if defined(__cpp_lib_spanstream) && __cpp_lib_spanstream >= 202106L
#include <spanstream>
#define HAS_SPANSTREAM
#else
#include <sstream>
#endif

#define REPORT_ERROR(LL)                                                       \
    [](std::string_view line) -> std::optional<session_description> {          \
        SAMLOG_WARN(auto sink) {                                               \
            char buf[256];                                                     \
            sink({buf, sizeof(buf)}, "parse \"{}\" failed\n", (line));         \
        };                                                                     \
        return {};                                                             \
    }(LL)

namespace asiortc {

constexpr bool parse_line_type_value(std::string_view line, char &type,
                                     std::string_view &value) {
    if (line.size() < 2 || line[1] != '=') {
        return false;
    }
    type = line[0];
    value = line.substr(2);
    return true;
}

constexpr std::string_view trim(std::string_view s) {
    auto pos = s.find_first_not_of(" \t\n\r\f\v");
    if (pos == std::string_view::npos)
        return {};
    s.remove_prefix(pos);
    pos = s.find_last_not_of(" \t\n\r\f\v");
    s.remove_suffix(s.size() - (pos + 1));
    return s;
}

constexpr std::string_view first_token(std::string_view s) {
    auto sp = s.find(' ');
    return sp != std::string_view::npos ? s.substr(0, sp) : s;
}

using splited_result = std::vector<std::string_view>;

static void split_lines(splited_result &lines, std::string_view s) {
    lines.clear();
    while (!s.empty()) {
        auto nl = s.find('\n');
        auto line = s.substr(0, nl);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (nl == std::string_view::npos) {
            if (!line.empty())
                lines.push_back(line);
            break;
        }
        lines.push_back(line);
        s.remove_prefix(nl + 1);
    }
}

static std::pair<std::string_view, std::string_view>
split_attr(std::string_view attr) {
    auto colon = attr.find(':');
    if (colon == std::string_view::npos)
        return {attr, {}};
    return {attr.substr(0, colon), attr.substr(colon + 1)};
}

static void split_whitespace(splited_result &parts, std::string_view s) {
    parts.clear();
    auto start = s.find_first_not_of(' ');
    if (start == std::string_view::npos)
        return;
    s.remove_prefix(start);
    while (!s.empty()) {
        auto end = s.find(' ');
        if (end == std::string_view::npos) {
            parts.push_back(s);
            break;
        }
        parts.push_back(s.substr(0, end));
        auto next = s.find_first_not_of(' ', end);
        if (next == std::string_view::npos)
            break;
        s.remove_prefix(next);
    }
}

static sdp_origin parse_origin(std::string_view s) {
    sdp_origin o;
#ifdef HAS_SPANSTREAM
    std::ispanstream is{s};
#else
    std::istringstream is{std::string(s)};
#endif
    if (is >> o.username >> o.session_id >> o.session_version >> o.nettype >>
        o.addrtype >> o.addr)
        return o;
    return {};
}

static std::optional<uint8_t> parse_uint8(std::string_view s) {
    uint8_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == std::end(s))
        return v;
    return std::nullopt;
}

static std::optional<uint16_t> parse_uint16(std::string_view s) {
    uint16_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == std::end(s))
        return v;
    return std::nullopt;
}

static std::optional<uint32_t> parse_uint32(std::string_view s) {
    uint32_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == std::end(s))
        return v;
    return std::nullopt;
}

static std::optional<uint64_t> parse_uint64(std::string_view s) {
    uint64_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == std::end(s))
        return v;
    return std::nullopt;
}

static std::optional<double> parse_double(std::string_view s) {
    double v = 0.;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == std::end(s))
        return v;
    return std::nullopt;
}

static std::string_view to_string(sdp_media_type t) noexcept {
    switch (t) {
    case sdp_media_type::audio:
        return "audio";
    case sdp_media_type::video:
        return "video";
    case sdp_media_type::application:
        return "application";
    case sdp_media_type::text:
        return "text";
    case sdp_media_type::message:
        return "message";
    }
    std::unreachable();
}

static std::optional<sdp_media_type>
parse_sdp_media_type(std::string_view s) noexcept {
    if (asioice::utils::nceq(s, "audio"))
        return sdp_media_type::audio;
    if (asioice::utils::nceq(s, "video"))
        return sdp_media_type::video;
    if (asioice::utils::nceq(s, "application"))
        return sdp_media_type::application;
    if (asioice::utils::nceq(s, "text"))
        return sdp_media_type::text;
    if (asioice::utils::nceq(s, "message"))
        return sdp_media_type::message;
    return std::nullopt;
}

static std::string_view to_string(sdp_proto p) noexcept {
    switch (p) {
    case sdp_proto::UDP:
        return "UDP";
    case sdp_proto::RTP_AVP:
        return "RTP/AVP";
    case sdp_proto::RTP_SAVP:
        return "RTP/SAVP";
    case sdp_proto::RTP_SAVPF:
        return "RTP/SAVPF";
    case sdp_proto::UDP_TLS_RTP_SAVPF:
        return "UDP/TLS/RTP/SAVPF";
    case sdp_proto::DTLS_SCTP:
        return "DTLS/SCTP";
    case sdp_proto::UDP_DTLS_SCTP:
        return "UDP/DTLS/SCTP";
    case sdp_proto::TCP_DTLS_SCTP:
        return "TCP/DTLS/SCTP";
    }
    std::unreachable();
}

static std::optional<sdp_proto> parse_sdp_proto(std::string_view s) noexcept {
    if (asioice::utils::nceq(s, "UDP"))
        return sdp_proto::UDP;
    if (asioice::utils::nceq(s, "RTP/AVP"))
        return sdp_proto::RTP_AVP;
    if (asioice::utils::nceq(s, "RTP/SAVP"))
        return sdp_proto::RTP_SAVP;
    if (asioice::utils::nceq(s, "RTP/SAVPF"))
        return sdp_proto::RTP_SAVPF;
    if (asioice::utils::nceq(s, "UDP/TLS/RTP/SAVPF"))
        return sdp_proto::UDP_TLS_RTP_SAVPF;
    if (asioice::utils::nceq(s, "DTLS/SCTP"))
        return sdp_proto::DTLS_SCTP;
    if (asioice::utils::nceq(s, "UDP/DTLS/SCTP"))
        return sdp_proto::UDP_DTLS_SCTP;
    if (asioice::utils::nceq(s, "TCP/DTLS/SCTP"))
        return sdp_proto::TCP_DTLS_SCTP;
    return std::nullopt;
}

static std::string_view to_string(sdp_direction d) noexcept {
    switch (d) {
    case sdp_direction::inactive:
        return "inactive";
    case sdp_direction::sendonly:
        return "sendonly";
    case sdp_direction::recvonly:
        return "recvonly";
    case sdp_direction::sendrecv:
        return "sendrecv";
    }
    std::unreachable();
}

static std::optional<sdp_direction>
parse_sdp_direction(std::string_view s) noexcept {
    if (asioice::utils::nceq(s, "inactive"))
        return sdp_direction::inactive;
    if (asioice::utils::nceq(s, "sendonly"))
        return sdp_direction::sendonly;
    if (asioice::utils::nceq(s, "recvonly"))
        return sdp_direction::recvonly;
    if (asioice::utils::nceq(s, "sendrecv"))
        return sdp_direction::sendrecv;
    return std::nullopt;
}

static std::string_view to_string(sdp_setup_role r) noexcept {
    switch (r) {
    case sdp_setup_role::active:
        return "active";
    case sdp_setup_role::passive:
        return "passive";
    case sdp_setup_role::actpass:
        return "actpass";
    case sdp_setup_role::holdconn:
        return "holdconn";
    }
    std::unreachable();
}

static std::optional<sdp_setup_role>
parse_setup_role(std::string_view s) noexcept {
    if (asioice::utils::nceq(s, "active"))
        return sdp_setup_role::active;
    if (asioice::utils::nceq(s, "passive"))
        return sdp_setup_role::passive;
    if (asioice::utils::nceq(s, "actpass"))
        return sdp_setup_role::actpass;
    if (asioice::utils::nceq(s, "holdconn"))
        return sdp_setup_role::holdconn;
    return std::nullopt;
}

static std::string_view to_string(sdp_rid_direction d) noexcept {
    switch (d) {
    case sdp_rid_direction::send:
        return "send";
    case sdp_rid_direction::recv:
        return "recv";
    }
    std::unreachable();
}

static std::optional<sdp_rid_direction>
parse_sdp_rid_direction(std::string_view s) noexcept {
    if (asioice::utils::nceq(s, "send"))
        return sdp_rid_direction::send;
    if (asioice::utils::nceq(s, "recv"))
        return sdp_rid_direction::recv;
    return std::nullopt;
}

static std::optional<sdp_rtpmap> parse_rtpmap(std::string_view value) {
    sdp_rtpmap res;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    auto rest = trim(value.substr(space + 1));

    if (auto pt = parse_uint8(value.substr(0, space)))
        res.payload_type = *pt;
    else
        return {};

    auto slash1 = rest.find('/');
    if (slash1 == std::string_view::npos)
        return {};
    res.name = rest.substr(0, slash1);
    auto after_name = rest.substr(slash1 + 1);
    auto slash2 = after_name.find('/');
    if (slash2 == std::string_view::npos) {
        if (auto cr = parse_uint32(after_name))
            res.clock_rate = *cr;
        else
            return {};
    } else {
        if (auto cr = parse_uint32(after_name.substr(0, slash2)))
            res.clock_rate = *cr;
        else
            return {};
        if (auto ch = parse_uint8(trim(after_name.substr(slash2 + 1))))
            res.channels = *ch;
        else
            return {};
    }
    return res;
}

std::optional<sdp_extmap> parse_extmap(std::string_view value) {
    sdp_extmap attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    auto id_sv = value.substr(0, space);
    auto uri = value.substr(space + 1);

    if (uri.empty())
        return {};

    auto slash = id_sv.find('/');
    if (slash != std::string_view::npos) {
        if (auto id = parse_uint16(id_sv.substr(0, slash)))
            attr.id = *id;
        else
            return {};
        if (auto d = parse_sdp_direction(id_sv.substr(slash + 1)))
            attr.direction = *d;
        else
            return {};
    } else {
        if (auto id = parse_uint16(id_sv))
            attr.id = *id;
    }

    auto sp2 = uri.find(' ');
    if (sp2 == std::string_view::npos)
        attr.uri = std::string(uri);
    else {
        attr.uri = std::string(uri.substr(0, sp2));
        auto ext = trim(uri.substr(sp2 + 1));
        if (!ext.empty()) {
            attr.attributes = std::string(ext);
        }
    }
    return attr;
}

std::optional<sdp_rtcp_fb> parse_rtcpfb(std::string_view value) {
    sdp_rtcp_fb attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    auto pt_str = value.substr(0, space);
    if (pt_str == "*") {

    } else if (auto pt = parse_uint8(pt_str); pt)
        attr.payload_type = pt;
    else
        return {};
    auto rest = trim(value.substr(space + 1));
    auto sp2 = rest.find(' ');
    if (sp2 == std::string_view::npos)
        attr.type = std::string(rest);
    else {
        attr.type = std::string(rest.substr(0, sp2));
        attr.subtype = std::string(trim(rest.substr(sp2 + 1)));
    }
    return attr;
}

static std::optional<sdp_ssrc> parse_ssrc(std::string_view value) {
    sdp_ssrc attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    auto ssrc_sv = value.substr(0, space);
    if (auto ssrc = parse_uint32(ssrc_sv))
        attr.ssrc = *ssrc;
    else
        return {};
    auto rest = value.substr(space + 1);
    auto colon = rest.find(':');
    if (colon == std::string_view::npos)
        attr.attribute = std::string(rest);
    else {
        attr.attribute = std::string(rest.substr(0, colon));
        attr.value = std::string(rest.substr(colon + 1));
    }
    return attr;
}

static std::optional<sdp_ssrc_group> parse_ssrc_group(std::string_view value) {
    sdp_ssrc_group attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    attr.semantics = std::string(value.substr(0, space));
    auto rest = value.substr(space + 1);

    splited_result parts;
    split_whitespace(parts, rest);
    for (const auto &p : parts) {
        if (auto ssrc = parse_uint32(p))
            attr.ssrcs.push_back(*ssrc);
        else
            return {};
    }

    return attr;
}

static std::optional<sdp_fingerprint>
parse_fingerprint(std::string_view value) {
    sdp_fingerprint attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    attr.algorithm = std::string(value.substr(0, space));
    attr.value = std::string(trim(value.substr(space + 1)));
    return attr;
}

static std::optional<sdp_bandwidth_type>
parse_bandwidth_type(std::string_view s) {
    if (asioice::utils::nceq(s, "CT"))
        return sdp_bandwidth_type::CT;
    if (asioice::utils::nceq(s, "AS"))
        return sdp_bandwidth_type::AS;
    if (asioice::utils::nceq(s, "TIAS"))
        return sdp_bandwidth_type::TIAS;
    if (asioice::utils::nceq(s, "RS"))
        return sdp_bandwidth_type::RS;
    if (asioice::utils::nceq(s, "RR"))
        return sdp_bandwidth_type::RR;
    return std::nullopt;
}

static std::optional<sdp_bandwidth> parse_bandwidth(std::string_view value) {
    auto colon = value.find(':');
    if (colon == std::string_view::npos)
        return {};
    auto type_str = value.substr(0, colon);
    auto bw_str = value.substr(colon + 1);
    auto bt = parse_bandwidth_type(type_str);
    auto bw = parse_uint32(bw_str);
    if (!bt || !bw)
        return {};
    return sdp_bandwidth{*bt, *bw};
}

static std::optional<sdp_crypto> parse_crypto(std::string_view value) {
    // 正则解释：
    // ^(\d+)               -> 第1组：Tag (数字)
    // \s+                  -> 空格分隔符
    // (\S+)                -> 第2组：Suite (非空字符，如
    // AES_CM_128_HMAC_SHA1_80) \s+                  -> 空格分隔符
    // (\S+)                -> 第3组：Key Params (非空字符，包含密钥信息)
    // (?:\s+(.*))?         -> 第4组(可选)：Session Params
    // (空格后的其余所有内容) $                    -> 行尾

    if (auto match =
            ctre::match<R"(^(\d+)\s+(\S+)\s+(\S+)(?:\s+(.*))?$)">(value)) {
        sdp_crypto crypto;

        // 1. 解析 Tag (uint32)
        if (auto tag = parse_uint32(match.get<1>())) {
            crypto.tag = *tag;
        } else {
            return {}; // Tag 解析失败
        }

        // 2. 解析 Suite
        crypto.suite = match.get<2>().to_string();

        // 3. 解析 Key Params
        crypto.key_params = match.get<3>().to_string();

        // 4. 解析 Session Params (可选)
        // match.get<4>() 如果没有匹配到会返回空 view，to_string() 则为空字符串
        crypto.session_params = match.get<4>().to_string();

        return crypto;
    }

    return {};
}

static std::optional<sdp_rtcp> parse_rtcp(std::string_view value) {
    // 正则解释：
    // ^(\d+)               -> 第1组：Port (必选)
    // (?:\s+(\S+)          -> 第2组：Nettype (可选，如 IN)
    // \s+(\S+)             -> 第3组：Addrtype (可选，如 IP4)
    // \s+(\S+))?           -> 第4组：Addr (可选，如 192.168.1.1)
    // $                    -> 结尾

    // 注意：RFC 3605 规定如果出现地址，必须是完整的 nettype addrtype addr 序列
    // 所以这里将这三者作为一个整体进行可选匹配
    if (auto match =
            ctre::match<R"(^(\d+)(?:\s+(\S+)\s+(\S+)\s+(\S+))?$)">(value)) {
        sdp_rtcp rtcp;

        // 1. 解析端口 (必选)
        if (auto port = parse_uint16(match.get<1>())) {
            rtcp.port = *port;
        } else {
            return {}; // 端口解析失败或溢出
        }

        // 2. 解析地址信息 (可选)
        // match.get<N>() 如果没有匹配到内容，其 bool 转换值为 false
        if (match.get<2>() && match.get<3>() && match.get<4>()) {
            rtcp.nettype = match.get<2>().to_string();
            rtcp.addrtype = match.get<3>().to_string();
            rtcp.addr = match.get<4>().to_string();
        }

        return rtcp;
    }

    return {};
}

static std::optional<sdp_msid> parse_msid(std::string_view value) {
    sdp_msid attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos) {
        attr.stream_id = std::string(value);
        return attr;
    }
    attr.stream_id = std::string(value.substr(0, space));
    auto track = trim(value.substr(space + 1));
    if (!track.empty())
        attr.track_id = std::string(track);
    return attr;
}

static bool check_msid(const sdp_media &m) {
    if (m.msids.empty())
        return true;
    const auto &id = m.msids.front().track_id;
    return std::none_of(
        m.msids.begin() + 1, m.msids.end(),
        [&id](const auto &msid) { return msid.track_id != id; });
}

static std::optional<sdp_msid_semantic>
parse_msid_semantic(std::string_view value) {
    sdp_msid_semantic attr;
    splited_result parts;
    split_whitespace(parts, value);
    attr.semantic = std::string(parts.front());
    for (int i = 1; i < parts.size(); ++i)
        attr.stream_ids.emplace_back(parts[i]);
    return attr;
}

static std::optional<sdp_group> parse_group(std::string_view value) {
    sdp_group attr;
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return {};
    attr.semantic = std::string(value.substr(0, space));
    auto mids = trim(value.substr(space + 1));
    if (!mids.empty()) {
        splited_result parts;
        split_whitespace(parts, mids);
        for (const auto &p : parts) {
            attr.items.emplace_back(p);
        }
    }
    return attr;
}

static std::vector<sdp_simulcast_layer>
parse_simulcast_layers(std::string_view str) {
    std::vector<sdp_simulcast_layer> result;
    while (!str.empty()) {
        auto semi = str.find(';');
        auto group = semi != std::string_view::npos ? str.substr(0, semi) : str;
        std::vector<sdp_simulcast_stream> alts;
        while (!group.empty()) {
            auto comma = group.find(',');
            auto token = comma != std::string_view::npos
                             ? group.substr(0, comma)
                             : group;
            token = trim(token);
            if (!token.empty()) {
                sdp_simulcast_stream s;
                if (token[0] == '~') {
                    s.paused = true;
                    token.remove_prefix(1);
                }
                s.rid = std::string(token);
                alts.push_back(std::move(s));
            }
            if (comma == std::string_view::npos)
                break;
            group = group.substr(comma + 1);
        }
        if (!alts.empty()) {
            result.emplace_back(std::move(alts));
        }
        if (semi == std::string_view::npos)
            break;
        str = str.substr(semi + 1);
    }
    return result;
}

static std::optional<sdp_simulcast> parse_simulcast(std::string_view value) {
    sdp_simulcast attr;
    auto v = trim(value);
    while (!v.empty()) {
        auto token = first_token(v);
        auto rest = token.size() == v.size() ? std::string_view{}
                                             : trim(v.substr(token.size() + 1));
        if (token == "send" && !rest.empty()) {
            auto nextKeyword = rest.find(" recv");
            auto streams = nextKeyword != std::string_view::npos
                               ? rest.substr(0, nextKeyword)
                               : rest;
            attr.send_layers = parse_simulcast_layers(trim(streams));
            if (nextKeyword != std::string_view::npos) {
                v = rest.substr(nextKeyword + 1);
            } else {
                break;
            }
        } else if (token == "recv" && !rest.empty()) {
            auto nextKeyword = rest.find(" send");
            auto streams = nextKeyword != std::string_view::npos
                               ? rest.substr(0, nextKeyword)
                               : rest;
            attr.recv_layers = parse_simulcast_layers(trim(streams));
            if (nextKeyword != std::string_view::npos) {
                v = rest.substr(nextKeyword + 1);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return attr;
}

static std::optional<sdp_rid> parse_rid(std::string_view value) {
    // 1. 匹配 RID 头部：id 和 direction
    // 格式：id direction [params]
    auto head_match =
        ctre::match<R"(^(\S+)\s+(send|recv)(?:\s+(.*))?$)">(value);
    if (!head_match) {
        return std::nullopt;
    }

    sdp_rid rid;
    rid.id = head_match.get<1>().to_string();

    // 解析方向
    auto dir_str = head_match.get<2>().to_view();
    rid.direction =
        (dir_str == "send") ? sdp_rid_direction::send : sdp_rid_direction::recv;

    auto params_str = head_match.get<3>().to_view();
    if (params_str.empty()) {
        return rid;
    }

    // 2. 解析参数部分
    // RFC 8851 允许 pt 列表使用分号分隔 (pt=96;97)，而参数之间也用分号分隔。
    // 逻辑：
    // - 遇到 key=value 对，检查是否为 'pt'。若是，标记进入 pt 列表上下文。
    // - 若遇到单纯的数字（无 key），且当前处于 pt 上下文，则视为 pt
    // 列表的延续。
    // - 否则视为普通参数。

    bool in_pt_list = false;

    // 使用 search_all 匹配非分号序列
    for (auto token_match : ctre::search_all<R"([^;]+)">(params_str)) {
        auto token = trim(token_match.to_view());

        // 尝试匹配 key=value 格式
        if (auto kv_match = ctre::match<R"(^([^=]+)=(.+)$)">(token)) {
            auto key = trim(kv_match.get<1>().to_view());
            auto val = trim(kv_match.get<2>().to_view());

            if (key == "pt") {
                in_pt_list = true; // 进入 pt 上下文
                // 处理 pt 值，可能包含逗号分隔的多个值 (pt=96,97)
                // 此时使用 search_all 匹配数字序列
                for (auto pt_match : ctre::search_all<R"(\d+)">(val)) {
                    if (auto pt = parse_uint8(pt_match.to_view())) {
                        rid.payload_types.push_back(*pt);
                    }
                }
            } else {
                in_pt_list = false; // 遇到其他参数，重置上下文
                rid.params.emplace(key, val);
            }
        } else {
            // 不是 key=value 格式，判断是否为 pt 列表的延续 (如 pt=96;97 中的
            // ";97") 只有当之前紧邻 pt 参数时才处理
            if (in_pt_list) {
                if (auto pt = parse_uint8(token)) {
                    rid.payload_types.push_back(*pt);
                }
            }
        }
    }

    return rid;
}

static std::optional<sdp_media> parse_media_line(std::string_view value) {
    sdp_media md;
#ifdef HAS_SPANSTREAM
    std::ispanstream iss{value};
#else
    std::istringstream iss{std::string(value)};
#endif
    std::string media_type_str, port_str, proto_str;
    if (!(iss >> media_type_str >> port_str >> proto_str)) {
        return {};
    }
    if (auto mt = parse_sdp_media_type(media_type_str))
        md.media_type = *mt;
    else
        return {};

    auto slash = port_str.find('/');
    if (slash != std::string::npos) {
        if (auto p =
                parse_uint16(std::string_view(port_str).substr(0, slash))) {
            md.port = *p;
        }
    } else {
        if (auto p = parse_uint16(port_str)) {
            md.port = *p;
        } else
            return {};
    }

    if (auto tp = parse_sdp_proto(proto_str)) {
        md.proto = *tp;
    }

    std::string fmt;
    bool is_sctp = md.media_type == sdp_media_type::application;
    if (is_sctp) {
        while (iss >> fmt) {
            md.add_format(fmt);
        }
    } else {
        // auto& pts = md.formats.emplace<std::vector<uint8_t>>();
        // while (iss >> fmt) {
        //     auto pt = parse_uint8(fmt);
        //     if (!pt)
        //         return {};
        //     pts.push_back(*pt);
        // }
    }

    return md;
}

static bool apply_session_attr(session_description &session,
                               std::string_view name, std::string_view value) {
    if (name == "group") {
        auto group = parse_group(value);
        if (!group)
            return false;
        session.groups.push_back(std::move(*group));
    } else if (name == "ice-ufrag") {
        session.ice_ufrag = std::string(trim(value));
    } else if (name == "ice-pwd") {
        session.ice_pwd = std::string(trim(value));
    } else if (name == "ice-options") {
        splited_result parts;
        split_whitespace(parts, value);
        for (const auto &p : parts) {
            session.ice_options.emplace_back(p);
        }
    } else if (name == "ice-lite") {
        session.ice_lite = true;
    } else if (name == "candidate") {
        session.candidates.emplace_back(std::format("candidate:{}", value));
    } else if (name == "fingerprint") {
        auto fp = parse_fingerprint(value);
        if (!fp)
            return false;
        session.fingerprints.push_back(std::move(*fp));
    } else if (name == "setup") {
        auto role = parse_setup_role(value);
        if (!role)
            return false;
        session.setup = *role;
    } else if (name == "extmap-allow-mixed") {
        session.extmap_allow_mixed = true;
    } else if (name == "msid-semantic") {
        auto ms = parse_msid_semantic(value);
        if (!ms)
            return false;
        session.msid_semantic = std::move(*ms);
    } else if (auto d = parse_sdp_direction(name)) {
        session.direction = *d;
    } else {
        session.attributes.emplace_back(std::string(name), std::string(value));
    }
    return true;
}

static bool apply_media_attr(sdp_media &media, std::string_view name,
                             std::string_view value) {
    if (name == "rtpmap") {
        auto rtpmap = parse_rtpmap(value);
        if (!rtpmap)
            return false;
        media.add_rtpmap(std::move(*rtpmap));
    } else if (name == "fmtp") {
        auto sp = value.find(' ');
        if (sp == std::string_view::npos)
            return false;
        auto pt = parse_uint8(value.substr(0, sp));
        if (!pt)
            return false;
        if (auto *rtpmap = media.find_rtpmap(*pt); !rtpmap)
            return false;
        else
            rtpmap->set_params_string(std::string(trim(value.substr(sp + 1))));
    } else if (name == "rtcp-fb") {
        auto fb = parse_rtcpfb(value);
        if (!fb)
            return false;
        media.add_feedback(std::move(*fb));
    } else if (name == "extmap") {
        auto extmap = parse_extmap(value);
        if (!extmap)
            return false;
        media.extmaps.push_back(std::move(*extmap));
    } else if (name == "ssrc") {
        auto ssrc = parse_ssrc(value);
        if (!ssrc)
            return false;
        media.ssrcs.push_back(std::move(*ssrc));
    } else if (name == "ssrc-group") {
        auto ssrc_group = parse_ssrc_group(value);
        if (!ssrc_group)
            return false;
        media.ssrc_groups.push_back(std::move(*ssrc_group));
    } else if (name == "rtcp") {
        auto rtcp = parse_rtcp(value);
        if (!rtcp)
            return false;
        media.rtcp = std::move(*rtcp);
    } else if (name == "mid") {
        auto mid = trim(value);
        if (!mid.empty())
            media.mid = std::string(mid);
        else
            return false;
    } else if (name == "msid") {
        auto msid = parse_msid(value);
        if (!msid)
            return false;
        media.msids.push_back(std::move(*msid));
    } else if (name == "ice-ufrag") {
        auto ufrag = trim(value);
        if (!ufrag.empty())
            media.ice_ufrag = std::string(ufrag);
        else
            return false;
    } else if (name == "ice-pwd") {
        auto pwd = trim(value);
        if (!pwd.empty())
            media.ice_pwd = std::string(pwd);
        else
            return false;
    } else if (name == "ice-options") {
        auto opts = trim(value);
        if (opts.empty())
            return false;
        splited_result parts;
        split_whitespace(parts, opts);
        for (const auto &p : parts) {
            media.ice_options.emplace_back(p);
        }
    } else if (name == "candidate") {
        media.candidates.emplace_back(std::format("candidate:{}", value));
    } else if (name == "end-of-candidates") {
        media.end_of_candidates = true;
    } else if (name == "fingerprint") {
        auto fp = parse_fingerprint(value);
        if (!fp)
            return false;
        media.fingerprints.push_back(std::move(*fp));
    } else if (name == "setup") {
        auto role = parse_setup_role(value);
        if (!role)
            return false;
        media.setup = *role;
    } else if (name == "crypto") {
        auto crypto = parse_crypto(value);
        if (!crypto)
            return false;
        media.cryptos.push_back(std::move(*crypto));
    } else if (name == "rtcp-mux") {
        media.rtcp_mux = true;
    } else if (name == "rtcp-mux-only") {
        media.rtcp_mux_only = true;
    } else if (name == "rtcp-rsize") {
        media.rtcp_rsize = true;
    } else if (name == "extmap-allow-mixed") {
        media.extmap_allow_mixed = true;
    } else if (name == "sctp-port") {
        auto port = parse_uint16(value);
        if (!port)
            return false;
        media.sctp_port = *port;
    } else if (name == "max-message-size") {
        auto size = parse_uint32(value);
        if (!size)
            return false;
        media.max_message_size = *size;
    } else if (name == "simulcast") {
        auto simulcast = parse_simulcast(value);
        if (!simulcast)
            return false;
        media.simulcast = std::move(*simulcast);
    } else if (name == "rid") {
        auto rid = parse_rid(value);
        if (!rid)
            return false;
        media.rids.push_back(std::move(*rid));
    } else if (auto d = parse_sdp_direction(name)) {
        media.direction = *d;
    } else {
        media.attributes.emplace_back(std::string(name), std::string(value));
    }

    return true;
}

static std::optional<session_description> do_parse(std::string_view sdp,
                                                   std::string_view type) {
    session_description session;
    session.sdp_type = std::string(type);

    splited_result lines;
    split_lines(lines, sdp);
    if (lines.empty()) {
        return REPORT_ERROR(sdp);
    }

    bool has_version = false;
    bool has_origin = false;
    bool has_session_name = false;
    bool has_timing = false;
    bool in_media = false;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::size_t lineNum = i + 1;
        auto line = lines[i];
        line = trim(line);

        if (line.empty()) {
            continue;
        }

        char type;
        std::string_view val;
        if (!parse_line_type_value(line, type, val)) {
            return REPORT_ERROR(line);
        }

        if (type == 'm') {
            // check last mline
            if (!session.medias.empty()) {
                const auto &last_m = session.medias.back();
                if (!check_msid(last_m))
                    return REPORT_ERROR("msid with different track id");
                // TODO
            }
            auto m = parse_media_line(val);
            if (!m)
                return REPORT_ERROR(line);
            session.medias.push_back(std::move(*m));
            in_media = true;
            continue;
        }

        if (in_media) {
            auto &media = session.medias.back();
            switch (type) {
            case 'c': {
                // TODO
                break;
            }
            case 'b':
                // TODO
                break;
            case 'k':
                // TODO
                break;
            case 'a': {
                auto [name, value] = split_attr(val);
                if (!apply_media_attr(media, name, value))
                    return REPORT_ERROR(line);
                break;
            }
            default:
                break;
            }
        } else {
            switch (type) {
            case 'v':
                has_version = true;
                if (auto v = parse_uint8(val)) {
                    session.version = *v;
                }
                break;
            case 'o':
                has_origin = true;
                session.origin = parse_origin(val);
                break;
            case 's':
                has_session_name = true;
                session.session_name = std::string(val);
                break;
            case 'i':
                // TODO
                break;
            case 'u':
                session.uri = std::string(val);
                break;
            case 'e':
                session.email = std::string(val);
                break;
            case 'p':
                session.phone_number = std::string(val);
                break;
            case 'c': {
                // TODO:
                break;
            }
            case 'b':
                // TODO:
                if (auto bw = parse_bandwidth(val))
                    session.bandwidths.push_back(*bw);
                else
                    return REPORT_ERROR(line);
                break;
            case 't': {
                // TODO:
                break;
            }
            case 'r': {
                // TODO:
                break;
            }
            case 'z': {
                // TODO:
                break;
            }
            case 'k':
                // TODO:
                break;
            case 'a': {
                auto [name, value] = split_attr(val);
                if (!apply_session_attr(session, name, value))
                    return REPORT_ERROR(line);
                break;
            }
            default:
                SAMLOG_WARN(auto sink) {
                    sink("unknow type of sdp line: {}\n", line);
                };
                break;
            }
        }
    }

    return session;
}

template <class T>
    requires(std::integral<T> || std::floating_point<T>)
static void to_string(std::string &s, T v) {
    char buf[128];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec == std::errc{})
        s.append(buf, res.ptr);
}

template <class... Args>
void format_to_str(std::string &res, std::format_string<Args...> fmt,
                   Args &&...args) {
    const auto fmt_size = std::formatted_size(fmt, std::forward<Args>(args)...);
    const auto old_size = res.size();
    res.resize_and_overwrite(
        old_size + fmt_size, [&](char *p, std::size_t) -> std::size_t {
            std::format_to(p + old_size, fmt, std::forward<Args>(args)...);
            return old_size + fmt_size;
        });
}

static void to_string(std::string &s, const sdp_origin &o) {
    format_to_str(s, "o={} {} {} {} {} {}", o.username, o.session_id,
                  o.session_version, o.nettype, o.addrtype, o.addr);
}

static void to_string(std::string &s, const sdp_rtpmap &attr) {
    format_to_str(s, "a=rtpmap:{} {}/{}", (int)attr.payload_type, attr.name,
                  attr.clock_rate);
    if (attr.channels)
        format_to_str(s, "/{}", (int)*attr.channels);
}

// static void to_string(std::string& s, const sdp_fmtp& attr)
// {
//     format_to_str(s,
//         "a=fmtp:{} {}",
//         (int)attr.payload_type,
//         attr.params
//     );
// }

static void to_string(std::string &s, const sdp_rtcp_fb &attr) {
    if (attr.payload_type)
        format_to_str(s, "a=rtcp-fb:{} {}", (int)*attr.payload_type, attr.type);
    else
        format_to_str(s, "a=rtcp-fb:* {}", attr.type);
    if (!attr.subtype.empty()) {
        s += ' ';
        s += attr.subtype;
    }
}

static void to_string(std::string &s, const sdp_extmap &attr) {
    format_to_str(s, "a=extmap:{}", attr.id);
    if (attr.direction)
        format_to_str(s, "/{}", to_string(*attr.direction));
    format_to_str(s, " {}", attr.uri);
    if (!attr.attributes.empty())
        format_to_str(s, " {}", attr.attributes);
}

static void to_string(std::string &s, const sdp_ssrc &attr) {
    format_to_str(s, "a=ssrc:{} {}", attr.ssrc, attr.attribute);
    if (!attr.value.empty())
        format_to_str(s, ":{}", attr.value);
}

static void to_string(std::string &s, const sdp_ssrc_group &attr) {
    format_to_str(s, "a=ssrc-group:{}", attr.semantics);
    for (auto ssrc : attr.ssrcs)
        format_to_str(s, " {}", ssrc);
}

static void to_string(std::string &s, const sdp_rtcp &attr) {
    format_to_str(s, "a=rtcp:{}", attr.port);
    if (!attr.nettype.empty() && !attr.addrtype.empty() && !attr.addr.empty())
        format_to_str(s, " {} {} {}", attr.nettype, attr.addrtype, attr.addr);
}

static void to_string(std::string &s, const sdp_fingerprint &attr) {
    format_to_str(s, "a=fingerprint:{} {}", attr.algorithm, attr.value);
}

static void to_string(std::string &s, const sdp_crypto &attr) {
    format_to_str(s, "a=crypto:{} {} {}", attr.tag, attr.suite,
                  attr.key_params);
    if (!attr.session_params.empty())
        format_to_str(s, " {}", attr.session_params);
}

static void to_string(std::string &s, const sdp_msid &attr) {
    format_to_str(s, "a=msid:{}", attr.stream_id);
    if (!attr.track_id.empty())
        format_to_str(s, " {}", attr.track_id);
}

static void to_string(std::string &s, const sdp_group &attr) {
    format_to_str(s, "a=group:{}", attr.semantic);
    for (const auto &item : attr.items)
        format_to_str(s, " {}", item);
}

static void to_string(std::string &s, const sdp_msid_semantic &attr) {
    format_to_str(s, "a=msid-semantic:{}", attr.semantic);
    for (const auto &stream_id : attr.stream_ids)
        format_to_str(s, " {}", stream_id);
}

static void to_string(std::string &s,
                      const std::vector<sdp_simulcast_layer> &attr) {
    auto old_size = s.size();
    for (const auto &layer : attr) {
        if (layer.streams.empty())
            continue;
        for (const auto &stream : layer.streams) {
            if (stream.paused)
                s += '~';
            s += stream.rid;
            s += ',';
        }
        s.back() = ';';
    }
    if (s.size() > old_size)
        s.pop_back();
}

static void to_string(std::string &s, const sdp_simulcast &attr) {
    if (attr.send_layers.empty() && attr.recv_layers.empty())
        return;
    s += "a=simulcast:";
    if (!attr.send_layers.empty()) {
        s += "send ";
        to_string(s, attr.send_layers);
    }
    if (!attr.recv_layers.empty()) {
        s += " recv ";
        to_string(s, attr.recv_layers);
    }
}

static void to_string(std::string &result, const sdp_rid &attr) {
    format_to_str(result, "a=rid:{} {}", attr.id, to_string(attr.direction));
    std::vector<std::string> parts;
    if (!attr.payload_types.empty()) {
        std::string pt = "pt=";
        for (std::size_t i = 0; i < attr.payload_types.size(); ++i) {
            if (i > 0)
                pt += ',';
            to_string(pt, (int)attr.payload_types[i]);
        }
        parts.push_back(std::move(pt));
    }

    if (!attr.params.empty()) {
        std::vector<std::string> keys;
        keys.reserve(attr.params.size());
        for (const auto &kv : attr.params) {
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys) {
            parts.push_back(key + "=" + attr.params.at(key));
        }
    }

    if (!parts.empty()) {
        result += " ";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0)
                result += ';';
            result += parts[i];
        }
    }
}

constexpr std::string_view to_string(sdp_bandwidth_type type) {
    switch (type) {
    case sdp_bandwidth_type::CT:
        return "CT";
    case sdp_bandwidth_type::AS:
        return "AS";
    case sdp_bandwidth_type::TIAS:
        return "TIAS";
    case sdp_bandwidth_type::RS:
        return "RS";
    case sdp_bandwidth_type::RR:
        return "RR";
    }
    return "CT";
}

static void to_string(std::string &out, const sdp_bandwidth &b) {
    format_to_str(out, "b={}:{}", to_string(b.type), b.bandwidth);
}

static void to_string(std::string &out, const sdp_media &m) {
    using ::asiortc::to_string;
    format_to_str(out, "m={} {} {}", to_string(m.media_type), m.port,
                  to_string(m.proto));
    if (auto pts = m.payload_types(); !pts.empty()) {
        for (auto pt : pts)
            format_to_str(out, " {}", (int)pt);
    } else if (auto fmts = m.formats(); !fmts.empty()) {
        for (const auto &fmt : fmts) {
            out += ' ';
            out += fmt;
        }
    }
    out += "\r\n";

    if (!m.conn_nettype.empty()) {
        out += "c=";
        out += m.conn_nettype;
        out += " ";
        out += m.conn_addrtype;
        out += " ";
        out += m.conn_addr;
        out += "\r\n";
    }

    format_to_str(out, "a={}\r\n", to_string(m.direction));

    if (!m.mid.empty())
        format_to_str(out, "a=mid:{}\r\n", m.mid);
    for (const auto &msid : m.msids) {
        to_string(out, msid);
        out += "\r\n";
    }

    if (!m.ice_ufrag.empty())
        format_to_str(out, "a=ice-ufrag:{}\r\n", m.ice_ufrag);
    if (!m.ice_pwd.empty())
        format_to_str(out, "a=ice-pwd:{}\r\n", m.ice_pwd);
    if (!m.ice_options.empty()) {
        out += "a=ice-options:";
        for (std::size_t i = 0; i < m.ice_options.size(); ++i) {
            if (i > 0)
                out += ' ';
            out += m.ice_options[i];
        }
        out += "\r\n";
    }
    for (const auto &c : m.candidates) {
        format_to_str(out, "a={}\r\n", c);
    }

    for (const auto &fp : m.fingerprints) {
        to_string(out, fp);
        out += "\r\n";
    }
    if (m.setup) {
        format_to_str(out, "a=setup:{}\r\n", to_string(*m.setup));
    }

    if (m.extmap_allow_mixed)
        out += "a=extmap-allow-mixed\r\n";
    if (m.rtcp_mux)
        out += "a=rtcp-mux\r\n";
    if (m.rtcp_mux_only)
        out += "a=rtcp-mux-only\r\n";
    if (m.rtcp_rsize)
        out += "a=rtcp-rsize\r\n";
    if (m.rtcp) {
        to_string(out, *m.rtcp);
        out += "\r\n";
    }

    for (const auto &em : m.extmaps) {
        to_string(out, em);
        out += "\r\n";
    }

    if (auto pts = m.payload_types(); !pts.empty()) {
        for (const auto &pt : pts) {
            const auto *rtpmap = m.find_rtpmap(pt);
            if (!rtpmap) {
                SAMLOG_WARN(auto sink) {
                    sink("No rtpmap with payload type {}\n", (int)pt);
                };
                continue;
            }
            to_string(out, *rtpmap);
            out += "\r\n";
            auto param = rtpmap->params_string();
            if (!param.empty())
                format_to_str(out, "a=fmtp:{} {}\r\n", pt, param);
            for (const auto &fb : m.rtcp_fbs())
                if (fb.payload_type == pt) {
                    to_string(out, fb);
                    out += "\r\n";
                }
        }
    }

    for (const auto &fb : m.rtcp_fbs())
        if (!fb.payload_type) {
            to_string(out, fb);
            out += "\r\n";
        }

    for (const auto &ssrc : m.ssrcs) {
        to_string(out, ssrc);
        out += "\r\n";
    }
    for (const auto &sg : m.ssrc_groups) {
        to_string(out, sg);
        out += "\r\n";
    }

    for (const auto &c : m.cryptos) {
        to_string(out, c);
        out += "\r\n";
    }

    if (m.sctp_port)
        format_to_str(out, "a=sctp-port:{}\r\n", *m.sctp_port);
    if (m.max_message_size)
        format_to_str(out, "a=max-message-size:{}\r\n", *m.max_message_size);

    to_string(out, m.simulcast);
    if (!m.simulcast.send_layers.empty() || !m.simulcast.recv_layers.empty())
        out += "\r\n";
    for (const auto &rid : m.rids) {
        to_string(out, rid);
        out += "\r\n";
    }

    if (m.end_of_candidates)
        out += "a=end-of-candidates\r\n";

    for (const auto &[name, value] : m.attributes) {
        out += "a=" + name;
        if (!value.empty())
            out += ":" + value;
        out += "\r\n";
    }
}

std::string session_description::to_string() const {
    using ::asiortc::to_string;

    const auto &sdp = *this;
    std::string out;
    out.reserve(128);

    format_to_str(out, "v={}\r\n", (int)sdp.version);

    to_string(out, sdp.origin);
    out += "\r\n";

    out += "s=";
    out += sdp.session_name;
    out += "\r\n";

    if (!sdp.uri.empty()) {
        out += "u=";
        out += sdp.uri;
        out += "\r\n";
    }

    if (!sdp.email.empty()) {
        out += "e=";
        out += sdp.email;
        out += "\r\n";
    }

    if (!sdp.phone_number.empty()) {
        out += "p=";
        out += sdp.phone_number;
        out += "\r\n";
    }

    if (!sdp.conn_nettype.empty()) {
        out += "c=";
        out += sdp.conn_nettype;
        out += " ";
        out += sdp.conn_addrtype;
        out += " ";
        out += sdp.conn_addr;
        out += "\r\n";
    }

    for (const auto &bw : sdp.bandwidths) {
        to_string(out, bw);
        out += "\r\n";
    }

    out += "t=0 0\r\n";

    if (sdp.direction) {
        out += "a=";
        out += to_string(*sdp.direction);
        out += "\r\n";
    }

    if (!sdp.ice_ufrag.empty()) {
        out += "a=ice-ufrag:";
        out += sdp.ice_ufrag;
        out += "\r\n";
    }
    if (!sdp.ice_pwd.empty()) {
        out += "a=ice-pwd:";
        out += sdp.ice_pwd;
        out += "\r\n";
    }
    if (!sdp.ice_options.empty()) {
        out += "a=ice-options:";
        for (std::size_t i = 0; i < sdp.ice_options.size(); ++i) {
            if (i > 0)
                out += ' ';
            out += sdp.ice_options[i];
        }
        out += "\r\n";
    }
    if (sdp.ice_lite) {
        out += "a=ice-lite\r\n";
    }

    for (const auto &fp : sdp.fingerprints) {
        to_string(out, fp);
        out += "\r\n";
    }
    if (sdp.setup) {
        out += "a=setup:";
        out += to_string(*sdp.setup);
        out += "\r\n";
    }

    if (sdp.extmap_allow_mixed) {
        out += "a=extmap-allow-mixed\r\n";
    }

    if (!sdp.msid_semantic.stream_ids.empty()) {
        out += "a=msid-semantic:";
        out += sdp.msid_semantic.semantic;
        for (const auto &id : sdp.msid_semantic.stream_ids) {
            out += " ";
            out += id;
        }
        out += "\r\n";
    }

    for (const auto &group : sdp.groups) {
        to_string(out, group);
        out += "\r\n";
    }

    for (const auto &[name, value] : sdp.attributes) {
        out += "a=";
        out += name;
        if (!value.empty()) {
            out += ":";
            out += value;
        }
        out += "\r\n";
    }

    for (const auto &c : sdp.candidates) {
        format_to_str(out, "a={}\r\n", c);
    }

    for (const auto &m : sdp.medias) {
        to_string(out, m);
    }

    return out;
}

static sdp_direction reverse_direction(sdp_direction d) {
    using enum sdp_direction;
    switch (d) {
    case inactive:
        return inactive;
    case sendrecv:
        return sendrecv;
    case sendonly:
        return recvonly;
    case recvonly:
        return sendonly;
    }
}

sdp_direction negotiate_direction(sdp_direction local,
                                  sdp_direction remote) noexcept {
    using enum sdp_direction;
    auto d = std::to_underlying(local) &
             std::to_underlying(reverse_direction(remote));
    return (sdp_direction)d;
}

const char *direction_str(sdp_direction d) {
    switch (d) {
    case sdp_direction::sendrecv:
        return "sendrecv";
    case sdp_direction::sendonly:
        return "sendonly";
    case sdp_direction::recvonly:
        return "recvonly";
    case sdp_direction::inactive:
        return "inactive";
    }
    return "sendrecv";
}

std::unique_ptr<session_description_interface>
parse_sdp(std::string_view sdp, std::string_view type) {
    auto res = do_parse(sdp, type);
    if (!res)
        return {};
    return std::make_unique<session_description>(std::move(*res));
}

void sdp_rtpmap::add_param(std::string_view name, std::string_view value) {
    if (_params.empty()) {
        _params += name;
        _params += '=';
        _params += value;
        return;
    }
    auto pos = _params.find(name);
    if (pos == std::string::npos) {
        _params += ';';
        _params += name;
        _params += '=';
        _params += value;
        return;
    }
    if (pos + name.size() + 1 > _params.size())
        return;
    if (_params[pos + name.size()] != '=')
        return;
    auto value_end = _params.find(';', pos + name.size() + 1);
    if (value_end == std::string::npos)
        value_end = _params.size();
    _params.replace(_params.begin() + pos + name.size() + 1,
                    _params.begin() + value_end, value);
}

void sdp_rtpmap::remove_param(std::string_view name) noexcept {
    auto pos = _params.find(name);
    if (pos == std::string::npos)
        return;
    auto end = _params.find(';', pos + name.size());
    if (end == std::string::npos)
        end = _params.size();
    else {
        ++end;
    }
    _params.erase(_params.begin() + pos, _params.begin() + end);
}

std::optional<std::string_view>
sdp_rtpmap::find_param(std::string_view name) const noexcept {
    auto pos = _params.find(name);
    if (pos == std::string::npos)
        return {};
    if (pos + name.size() + 1 > _params.size())
        return {};
    if (_params[pos + name.size()] != '=')
        return {};
    auto end = _params.find(';', pos + name.size() + 1);
    if (end == std::string::npos)
        end = _params.size();
    return std::string_view{_params.begin() + pos + name.size() + 1,
                            _params.begin() + end};
}

const sdp_rtpmap *sdp_media::find_rtpmap(uint8_t pt) const noexcept {
    auto it = std::ranges::find_if(this->_rtpmaps, [pt](const auto &rtpmap) {
        return rtpmap.payload_type == pt;
    });
    if (it == this->_rtpmaps.end())
        return nullptr;
    return &*it;
}

sdp_rtpmap *sdp_media::find_rtpmap(uint8_t pt) noexcept {
    auto it = std::ranges::find_if(this->_rtpmaps, [pt](const auto &rtpmap) {
        return rtpmap.payload_type == pt;
    });
    if (it == this->_rtpmaps.end())
        return nullptr;
    return &*it;
}

void sdp_media::add_rtpmap(sdp_rtpmap map) {
    auto pt = map.payload_type;
    if (find_rtpmap(pt))
        return;
    _rtpmaps.emplace_back(std::move(map));
    auto pts = std::get_if<std::vector<uint8_t>>(&this->_formats);
    if (!pts) {
        pts = &this->_formats.emplace<std::vector<uint8_t>>();
    }
    pts->push_back(pt);
}

void sdp_media::add_feedback(sdp_rtcp_fb fb) {
    if (fb.type.empty())
        return;
    static constexpr auto eq = [](const sdp_rtcp_fb &a, const sdp_rtcp_fb &b) {
        return a.type == b.type && a.subtype == b.subtype;
    };
    if (fb.payload_type) {
        auto it = std::ranges::find_if(this->_rtcp_fbs, [&fb](const auto &f) {
            if (!eq(f, fb))
                return false;
            if (!f.payload_type)
                return true;
            return *f.payload_type == *fb.payload_type;
        });
        if (it == this->_rtcp_fbs.end())
            this->_rtcp_fbs.emplace_back(std::move(fb));
    } else {
        std::erase_if(this->_rtcp_fbs,
                      [&fb](const auto &f) { return eq(f, fb); });
        this->_rtcp_fbs.emplace_back(std::move(fb));
    }
}

sdp_rtpmap sdp_rtpmap::from_media_description(const media_description &desc) {
    sdp_rtpmap c;
    c.clock_rate = desc.clock_rate;
    c.channels = desc.channels;
    c.set_params_string(desc.encoding_params);
    switch (desc.format) {
    case media_format::opus:
        c.name = "opus";
        c.payload_type = 111;
        break;
    case media_format::vp8:
        c.name = "vp8";
        c.payload_type = 96;
        break;
    case media_format::h264:
        c.name = "h264";
        c.payload_type = 102;
        c.add_param("profile-level-id", "42001f");
        break;
    case media_format::vp9:
        c.name = "vp9";
        c.payload_type = 98;
        break;
    default:
        throw std::invalid_argument{
            "sdp_rtpmap::from_media_description: invalid description"};
    }
    return c;
}

bool sdp_rtpmap::is_match(const sdp_rtpmap &a, const sdp_rtpmap &b) noexcept {
    if (!asioice::utils::nceq(a.name, b.name) || a.clock_rate != b.clock_rate ||
        a.channels != b.channels)
        return false;
    if (asioice::utils::nceq(a.name, "H264") &&
        a.find_param("profile-level-id") != b.find_param("profile-level-id"))
        return false;
    return true;
}

} // namespace asiortc
