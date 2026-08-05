#include "sdp.hpp"
#include "samlog.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asiortc {

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

std::pair<std::string_view, std::string_view>
split_attr(std::string_view attr) {
    auto colon = attr.find(':');
    if (colon == std::string_view::npos)
        return {attr, {}};
    return {attr.substr(0, colon), attr.substr(colon + 1)};
}

void split_whitespace(splited_result &parts, std::string_view s) {
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

std::optional<uint64_t> parse_uint64(std::string_view s) {
    uint64_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{})
        return v;
    return std::nullopt;
}

std::optional<uint8_t> parse_uint8(std::string_view s) {
    uint64_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && v <= 255)
        return static_cast<uint8_t>(v);
    return std::nullopt;
}

std::optional<uint16_t> parse_uint16(std::string_view s) {
    uint64_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && v <= 65535)
        return static_cast<uint16_t>(v);
    return std::nullopt;
}

std::optional<sdp_codec> parse_rtpmap(std::string_view value) {
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return std::nullopt;
    auto pt_sv = value.substr(0, space);
    auto rest = value.substr(space + 1);

    auto pt = parse_uint8(pt_sv);
    if (!pt)
        return std::nullopt;

    auto slash1 = rest.find('/');
    if (slash1 == std::string_view::npos)
        return std::nullopt;
    auto name = rest.substr(0, slash1);
    auto after_name = rest.substr(slash1 + 1);

    auto slash2 = after_name.find('/');
    std::string_view clock_sv;
    std::string_view params_sv;
    if (slash2 == std::string_view::npos) {
        clock_sv = after_name;
    } else {
        clock_sv = after_name.substr(0, slash2);
        params_sv = after_name.substr(slash2 + 1);
    }

    auto clock = parse_uint64(clock_sv);
    if (!clock || *clock > UINT32_MAX)
        return std::nullopt;

    sdp_codec c;
    c.payload_type = *pt;
    c.name = std::string(name);
    c.clock_rate = static_cast<uint32_t>(*clock);
    c.encoding_params = std::string(params_sv);
    return c;
}

std::optional<sdp_extmap> parse_extmap(std::string_view value) {
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return std::nullopt;
    auto id_sv = value.substr(0, space);
    auto uri = value.substr(space + 1);

    if (uri.empty())
        return std::nullopt;

    auto slash = id_sv.find('/');
    if (slash != std::string_view::npos)
        id_sv = id_sv.substr(0, slash);

    auto id = parse_uint8(id_sv);
    if (!id || *id == 0)
        return std::nullopt;

    sdp_extmap e;
    e.id = *id;
    e.uri = std::string(uri);
    return e;
}

std::optional<sdp_rtcp_fb> parse_rtcpfb(std::string_view value) {
    auto space = value.find(' ');
    if (space == std::string_view::npos)
        return std::nullopt;
    auto pt_sv = value.substr(0, space);
    auto rest = value.substr(space + 1);

    auto pt = parse_uint8(pt_sv);
    if (!pt)
        return std::nullopt;

    auto space2 = rest.find(' ');
    std::string_view type_sv = rest;
    std::string_view subtype_sv;
    if (space2 != std::string_view::npos) {
        type_sv = rest.substr(0, space2);
        subtype_sv = rest.substr(space2 + 1);
    }

    sdp_rtcp_fb fb;
    fb.payload_type = *pt;
    fb.type = std::string(type_sv);
    fb.subtype = std::string(subtype_sv);
    return fb;
}

static bool apply_session_attr(session_description &session,
                               std::string_view name, std::string_view value) {
    std::string vstr(value);
    splited_result parts;
    if (name == "group") {
        split_whitespace(parts, value);
        if (parts.empty())
            return false;
        if (parts.front() == "BUNDLE") {
            std::vector<std::string> mids;
            for (std::size_t i = 1; i < parts.size(); ++i)
                mids.emplace_back(parts[i]);
            session.groups.emplace_back("BUNDLE", std::move(mids));
        } else if (parts.front() == "LS") {
            std::vector<std::string> mids;
            for (std::size_t i = 1; i < parts.size(); ++i)
                mids.emplace_back(parts[i]);
            session.groups.emplace_back("LS", std::move(mids));
        }
    } else if (name == "ice-ufrag") {
        session.ice_ufrag = vstr;
    } else if (name == "ice-pwd") {
        session.ice_pwd = vstr;
    } else if (name == "fingerprint") {
        session.fingerprint = vstr;
    } else if (name == "setup") {
        session.setup = vstr;
    } else if (name == "mid") {
        session.mid = vstr;
    } else if (name == "candidate") {
        session.candidates.push_back("candidate:" + vstr);
    } else if (name == "msid-semantic") {
        split_whitespace(parts, value);
        if (!parts.empty()) {
            session.msid_semantic = std::string(parts[0]);
            for (std::size_t i = 1; i < parts.size(); ++i)
                session.msid_tokens.emplace_back(parts[i]);
        }
    } else {
        session.attributes.emplace_back(std::string(name), vstr);
    }

    return true;
}

void apply_media_attr(sdp_media &media, std::string_view name,
                      std::string_view value) {
    std::string vstr(value);
    if (name == "mid") {
        media.mid = vstr;
    } else if (name == "sendrecv") {
        media.direction = sdp_direction::sendrecv;
    } else if (name == "sendonly") {
        media.direction = sdp_direction::sendonly;
    } else if (name == "recvonly") {
        media.direction = sdp_direction::recvonly;
    } else if (name == "inactive") {
        media.direction = sdp_direction::inactive;
    } else if (name == "rtcp-mux") {
        media.rtcp_mux = true;
    } else if (name == "ice-ufrag") {
        media.ice_ufrag = vstr;
    } else if (name == "ice-pwd") {
        media.ice_pwd = vstr;
    } else if (name == "fingerprint") {
        media.fingerprint = vstr;
    } else if (name == "setup") {
        media.setup = vstr;
    } else if (name == "candidate") {
        media.candidates.push_back("candidate:" + vstr);
    } else if (name == "rtpmap") {
        auto c = parse_rtpmap(value);
        if (c)
            media.rtpmaps.push_back(*c);
        else
            media.attributes.emplace_back("rtpmap", vstr);
    } else if (name == "fmtp") {
        media.fmtps.push_back(vstr);
    } else if (name == "extmap") {
        auto e = parse_extmap(value);
        if (e)
            media.extmaps.push_back(*e);
        else
            media.attributes.emplace_back("extmap", vstr);
    } else if (name == "ssrc") {
        media.ssrcs.push_back(vstr);
    } else if (name == "msid") {
        auto sp = value.find(' ');
        if (sp == std::string_view::npos)
            media.msids.push_back({std::string(value), {}});
        else
            media.msids.push_back({std::string(value.substr(0, sp)),
                                   std::string(value.substr(sp + 1))});
    } else if (name == "rid") {
        media.rids.push_back(vstr);
    } else if (name == "simulcast") {
        media.simulcast = vstr;
    } else if (name == "rtcp-fb") {
        auto fb = parse_rtcpfb(value);
        if (fb)
            media.rtcp_fbs.push_back(*fb);
        else
            media.attributes.emplace_back("rtcp-fb", vstr);
    } else if (name == "sctpmap") {
        media.sctpmap = vstr;
    } else if (name == "sctp-port") {
        auto p = parse_uint16(value);
        if (p)
            media.sctp_port = *p;
    } else if (name == "max-message-size") {
        media.attributes.emplace_back("max-message-size", vstr);
    } else {
        media.attributes.emplace_back(std::string(name), vstr);
    }
}

#define REPORT_ERROR(LL)                                                       \
    [](std::string_view line) -> std::optional<session_description> {          \
        SAMLOG_WARN(auto sink) {                                               \
            char buf[256];                                                     \
            sink({buf, sizeof(buf)}, "parse \"{}\" failed\n", (line));         \
        };                                                                     \
        return {};                                                             \
    }(LL)

std::optional<session_description> parse_sdp(std::string_view sdp_text,
                                             std::string type) {
    session_description session;
    session.type = std::move(type);

    sdp_media *current_media = nullptr;

    splited_result lines;
    lines.reserve(64);
    split_lines(lines, sdp_text);

    splited_result parts;
    for (auto line : lines) {
        if (line.empty())
            continue;

        if (line.starts_with("v=")) {
            auto v = parse_uint8(line.substr(2));
            if (!v)
                return REPORT_ERROR(line);
            session.version = *v;
        } else if (line.starts_with("o=")) {
            split_whitespace(parts, line.substr(2));
            if (parts.size() < 6)
                return REPORT_ERROR(line);
            session.origin.username = parts[0];
            if (auto sid = parse_uint64(parts[1]); sid)
                session.origin.session_id = *sid;
            else
                return REPORT_ERROR(line);
            if (auto sv = parse_uint64(parts[2]); sv)
                session.origin.session_version = *sv;
            else
                return REPORT_ERROR(line);
            session.origin.nettype = parts[3];
            session.origin.addrtype = parts[4];
            session.origin.addr = parts[5];
        } else if (line.starts_with("s=")) {
            session.session_name = line.substr(2);
        } else if (line.starts_with("t=")) {
            split_whitespace(parts, line.substr(2));
            if (parts.size() >= 2) {
                if (auto start = parse_uint64(parts[0]); start)
                    session.timing.start = *start;
                else
                    return REPORT_ERROR(line);
                if (auto stop = parse_uint64(parts[1]); stop)
                    session.timing.stop = *stop;
                else
                    return REPORT_ERROR(line);
            }
        } else if (line.starts_with("c=")) {
            split_whitespace(parts, line.substr(2));
            if (parts.size() >= 3) {
                if (current_media) {
                    current_media->conn_nettype = parts[0];
                    current_media->conn_addrtype = parts[1];
                    current_media->conn_addr = parts[2];
                } else {
                    session.conn_nettype = parts[0];
                    session.conn_addrtype = parts[1];
                    session.conn_addr = parts[2];
                }
            }
        } else if (line.starts_with("m=")) {
            split_whitespace(parts, line.substr(2));
            if (parts.size() >= 3) {
                sdp_media media;
                media.media_type = parts[0];
                if (auto port = parse_uint16(parts[1]); port)
                    media.port = *port;
                else
                    return REPORT_ERROR(line);
                media.proto = parts[2];
                for (std::size_t i = 3; i < parts.size(); ++i) {
                    media.fmts.emplace_back(parts[i]);
                    if (auto pt = parse_uint8(parts[i]); pt)
                        media.payload_types.push_back(*pt);
                }
                session.medias.push_back(std::move(media));
                current_media = &session.medias.back();
            }
        } else if (line.starts_with("a=")) {
            auto [name, value] = split_attr(line.substr(2));
            if (current_media)
                apply_media_attr(*current_media, name, value);
            else {
                if (!apply_session_attr(session, name, value))
                    return REPORT_ERROR(line);
            }
        }
    }

    return session;
}

std::string session_description::to_string() const {
    const auto &sdp = *this;
    std::string out;
    out.reserve(128);

    out += "v=";
    out += std::to_string(sdp.version);
    out += "\r\n";

    out += "o=";
    out += sdp.origin.username;
    out += " ";
    out += std::to_string(sdp.origin.session_id);
    out += " ";
    out += std::to_string(sdp.origin.session_version);
    out += " ";
    out += sdp.origin.nettype;
    out += " ";
    out += sdp.origin.addrtype;
    out += " ";
    out += sdp.origin.addr;
    out += "\r\n";

    out += "s=";
    out += sdp.session_name;
    out += "\r\n";

    if (!sdp.conn_nettype.empty()) {
        out += "c=";
        out += sdp.conn_nettype;
        out += " ";
        out += sdp.conn_addrtype;
        out += " ";
        out += sdp.conn_addr;
        out += "\r\n";
    }

    out += "t=";
    out += std::to_string(sdp.timing.start);
    out += " ";
    out += std::to_string(sdp.timing.stop);
    out += "\r\n";

    if (!sdp.msid_semantic.empty()) {
        out += "a=msid-semantic:";
        out += sdp.msid_semantic;
        for (const auto &t : sdp.msid_tokens) {
            out += " ";
            out += t;
        }
        out += "\r\n";
    }

    for (const auto &grp : sdp.groups)
        grp.to_string(out);

    if (!sdp.ice_ufrag.empty())
        out += "a=ice-ufrag:" + sdp.ice_ufrag + "\r\n";
    if (!sdp.ice_pwd.empty())
        out += "a=ice-pwd:" + sdp.ice_pwd + "\r\n";
    if (!sdp.fingerprint.empty())
        out += "a=fingerprint:" + sdp.fingerprint + "\r\n";
    if (!sdp.setup.empty())
        out += "a=setup:" + sdp.setup + "\r\n";
    if (!sdp.mid.empty())
        out += "a=mid:" + sdp.mid + "\r\n";
    for (const auto &c : sdp.candidates)
        out += "a=" + c + "\r\n";
    for (const auto &[name, value] : sdp.attributes) {
        out += "a=" + name;
        if (!value.empty())
            out += ":" + value;
        out += "\r\n";
    }

    for (const auto &m : sdp.medias) {
        out += "m=";
        out += m.media_type;
        out += " ";
        out += std::to_string(m.port);
        out += " ";
        out += m.proto;
        if (!m.fmts.empty()) {
            for (const auto &fmt : m.fmts) {
                out += " ";
                out += fmt;
            }
        } else {
            for (auto pt : m.payload_types) {
                out += " ";
                out += std::to_string(pt);
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

        if (!m.mid.empty())
            out += "a=mid:" + m.mid + "\r\n";

        switch (m.direction) {
        case sdp_direction::sendrecv:
            out += "a=sendrecv\r\n";
            break;
        case sdp_direction::sendonly:
            out += "a=sendonly\r\n";
            break;
        case sdp_direction::recvonly:
            out += "a=recvonly\r\n";
            break;
        case sdp_direction::inactive:
            out += "a=inactive\r\n";
            break;
        }

        if (m.rtcp_mux)
            out += "a=rtcp-mux\r\n";

        for (const auto &c : m.rtpmaps) {
            out += "a=rtpmap:";
            out += std::to_string(c.payload_type);
            out += " ";
            out += c.name;
            out += "/";
            out += std::to_string(c.clock_rate);
            if (!c.encoding_params.empty()) {
                out += "/";
                out += c.encoding_params;
            }
            out += "\r\n";
        }
        for (const auto &f : m.fmtps)
            out += "a=fmtp:" + f + "\r\n";
        for (const auto &e : m.extmaps) {
            out += "a=extmap:";
            out += std::to_string(e.id);
            out += " ";
            out += e.uri;
            out += "\r\n";
        }
        for (const auto &fb : m.rtcp_fbs) {
            out += "a=rtcp-fb:";
            out += std::to_string(fb.payload_type);
            out += " ";
            out += fb.type;
            if (!fb.subtype.empty()) {
                out += " ";
                out += fb.subtype;
            }
            out += "\r\n";
        }
        for (const auto &s : m.ssrcs)
            out += "a=ssrc:" + s + "\r\n";
        for (const auto &msid : m.msids) {
            out += "a=msid:" + msid.stream_id;
            if (!msid.track_id.empty())
                out += " " + msid.track_id;
            out += "\r\n";
        }
        for (const auto &rid : m.rids)
            out += "a=rid:" + rid +
                   " send\r\n"; // TODO: handle simulcast parameters
        if (m.rids.size() > 1) {
            out += "a=simulcast:send ";
            for (const auto &rid : m.rids) {
                out += rid;
                out += ';';
            }
            out.pop_back();
            out += "\r\n";
        }

        if (!m.ice_ufrag.empty())
            out += "a=ice-ufrag:" + m.ice_ufrag + "\r\n";
        if (!m.ice_pwd.empty())
            out += "a=ice-pwd:" + m.ice_pwd + "\r\n";
        if (!m.fingerprint.empty())
            out += "a=fingerprint:" + m.fingerprint + "\r\n";
        if (!m.setup.empty())
            out += "a=setup:" + m.setup + "\r\n";
        for (const auto &c : m.candidates)
            out += "a=" + c + "\r\n";

        if (!m.sctpmap.empty())
            out += "a=sctpmap:" + m.sctpmap + "\r\n";
        if (m.sctp_port != 0)
            out += "a=sctp-port:" + std::to_string(m.sctp_port) + "\r\n";

        for (const auto &[name, value] : m.attributes) {
            out += "a=" + name;
            if (!value.empty())
                out += ":" + value;
            out += "\r\n";
        }
    }

    return out;
}

sdp_direction negotiate_direction(sdp_direction local,
                                  sdp_direction remote) noexcept {
    using enum sdp_direction;
    if (local == inactive || remote == inactive)
        return inactive;
    if (local == sendrecv)
        return remote;
    if (local == sendonly)
        return (remote == recvonly || remote == sendrecv) ? sendonly : inactive;
    if (local == recvonly)
        return (remote == sendonly || remote == sendrecv) ? recvonly : inactive;
    return inactive;
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

void sdp_group::to_string(std::string &dst) const {
    dst += "a=group:";
    dst += this->semantic;
    for (const auto &item : this->items) {
        dst += ' ';
        dst += item;
    }
    dst += "\r\n";
}

} // namespace asiortc
