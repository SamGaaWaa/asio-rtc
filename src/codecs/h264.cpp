#include "h264.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h> // 用于 av_rescale_q
}

#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>
#include <algorithm>

#include "asiortc/media_track.hpp"
#include "asiortc/codecs/base.hpp"

namespace asiortc::codecs {

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    // 使用 av_rescale_q 避免溢出并提高精度
    return static_cast<uint32_t>(av_rescale_q(pts, time_base, {1, 90000}));
}

namespace {

static constexpr int PACKET_MAX = 1300;

static const uint8_t ANNEXB_START_4[] = {0x00, 0x00, 0x00, 0x01};
static const uint8_t ANNEXB_START_3[] = {0x00, 0x00, 0x01};

// FFmpeg 编码器输出的 Annex-B 数据包含防止仿效字节 (0x00 0x00 0x03)。
// RTP 传输 (RFC 6184) 不允许这些字节，必须在打包前去除。
static std::size_t remove_emulation_prevention(uint8_t *data,
                                               size_t size) noexcept {
    if (size < 3)
        return size;

    size_t read_pos = 0;
    size_t write_pos = 0;

    while (read_pos < size) {
        // 检查是否是 0x00 0x00 0x03
        if (read_pos + 2 < size && data[read_pos] == 0x00 &&
            data[read_pos + 1] == 0x00 && data[read_pos + 2] == 0x03) {

            // 严格标准：只有当 03 后面的字节是 00, 01, 02, 03
            // 或者处于流末尾时，03 才是防竞争字节
            bool is_emulation_byte =
                (read_pos + 3 == size) || (data[read_pos + 3] <= 0x03);

            if (is_emulation_byte) {
                // 写入 0x00 0x00
                data[write_pos++] = 0x00;
                data[write_pos++] = 0x00;
                // 跳过 0x03
                read_pos += 3;
                continue;
            }
        }

        // 常规字节直接拷贝
        data[write_pos++] = data[read_pos++];
    }

    return write_pos; // 返回新长度
}

static std::vector<std::vector<uint8_t>> _annexb_to_nalus(const uint8_t *data,
                                                          size_t len) {
    std::vector<std::vector<uint8_t>> nalus;
    size_t pos = 0;

    while (pos < len) {
        // 1. 跳过起始码前的 0x00
        while (pos < len && data[pos] == 0x00)
            ++pos;
        if (pos >= len)
            break;

        // 2. 检查起始码 0x00 0x00 0x01 (3-byte) 或 0x00 0x00 0x00 0x01 (4-byte)
        // 此时 pos 指向 0x00
        bool has_start_code = false;
        if (pos + 2 < len && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            data[pos + 2] == 0x01) {
            pos += 3;
            has_start_code = true;
        } else if (pos + 3 < len && data[pos] == 0x00 &&
                   data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
                   data[pos + 3] == 0x01) {
            pos += 4;
            has_start_code = true;
        }

        if (!has_start_code) {
            // 没有找到起始码，可能是数据损坏或格式不对，停止解析
            // 或者如果这是第一个NALU且没有起始码（某些特殊情况），也可以处理，
            // 但标准AnnexB一定有起始码。
            break;
        }

        size_t start = pos;
        size_t end = start;

        // 3. 查找下一个起始码
        // 这里需要一直查到 len - 4，防止越界
        while (end < len) {
            // 提前检查是否存在 00 00 00 01
            if (end + 3 < len && data[end] == 0x00 && data[end + 1] == 0x00 &&
                data[end + 2] == 0x00 && data[end + 3] == 0x01) {
                break;
            }
            // 提前检查是否存在 00 00 01
            if (end + 2 < len && data[end] == 0x00 && data[end + 1] == 0x00 &&
                data[end + 2] == 0x01) {
                break;
            }
            ++end;
        }

        if (end > start) {
            nalus.emplace_back(data + start, data + end);
        }

        // pos 会被移到 end，下一次循环开始时跳过 0x00
        pos = end;
    }
    return nalus;
}

class H264EncoderImpl : public encoder {
  public:
    H264EncoderImpl(const encoder_params &p)
        : _bitrate(p.bitrate.value_or(1'000'000)),
          _max_framerate(p.max_framerate.value_or(30)) {}

    ~H264EncoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe) override {
        if (!_ctx || _width != frame.width || _height != frame.height) {
            _width = frame.width;
            _height = frame.height;
            _init_context();
        }

        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });
        if (!f)
            throw std::runtime_error{"av_frame_alloc failed"};

        f->format = _ctx->pix_fmt;
        f->width = _ctx->width;
        f->height = _ctx->height;

        // 确保数据对齐正确，虽然通常 libavutil 分配的 buffer 是对齐的
        if (av_image_fill_arrays(f->data, f->linesize, frame.data.data(),
                                 static_cast<AVPixelFormat>(f->format),
                                 f->width, f->height, 1) < 0)
            throw std::runtime_error{"av_image_fill_arrays failed"};

        f->pts = frame.timestamp;

        if (force_keyframe)
            f->pict_type = AV_PICTURE_TYPE_I;

        int ret = avcodec_send_frame(_ctx, f.get());
        f.reset(); // 尽快释放 frame 引用
        if (ret < 0)
            return {{}, 0};

        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });

        std::vector<uint8_t> encoded;
        int64_t last_pts = AV_NOPTS_VALUE;

        while (true) {
            ret = avcodec_receive_packet(_ctx, pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            last_pts = pkt->pts;
            // 合并数据到 encoded buffer
            encoded.insert(encoded.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt.get());
        }

        if (encoded.empty())
            return {{}, 0};

        // 将 PTS 转换为 RTP 时间戳
        uint32_t rtp_ts = to_rtp_timestamp(last_pts, _ctx->time_base);

        // 切分 NALU 并打包
        auto payloads = _packetize(encoded);
        return {std::move(payloads), rtp_ts};
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data,
         uint32_t timestamp) override {
        auto payloads = _packetize(encoded_data);
        return {payloads, timestamp};
    }

    void set_parameters(const encoder_params &p) override {
        if (p.bitrate) {
            _bitrate = *p.bitrate;
            if (_ctx) {
                _ctx->bit_rate = _bitrate;
                // 某些编码器可能需要重新打开或应用标志位才能立即生效
                av_opt_set_int(_ctx->priv_data, "b", _bitrate, 0);
            }
        }
        if (p.max_framerate && _ctx &&
            (_ctx->framerate.num != *p.max_framerate ||
             _ctx->framerate.den != 1)) {
            _max_framerate = *p.max_framerate;
            _ctx->framerate = {_max_framerate, 1};
            _ctx->gop_size = _max_framerate * 2; // 建议 GOP 设为帧率的 2 倍左右
        }
    }

  private:
    void _init_context() {
        if (_ctx)
            avcodec_free_context(&_ctx);

        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec)
            throw std::runtime_error{"Codec H264 not found"};

        _ctx = avcodec_alloc_context3(codec);
        _ctx->width = _width;
        _ctx->height = _height;
        _ctx->bit_rate = _bitrate;
        _ctx->pix_fmt = AV_PIX_FMT_YUV420P;

        // 设置时间基为 90kHz 以匹配 RTP
        _ctx->time_base = {1, 90000};
        _ctx->framerate = {_max_framerate, 1};
        _ctx->gop_size = _max_framerate * 2;

        // 使用 Constrained Baseline 以确保最大兼容性 (WebRTC 标准)
        _ctx->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        _ctx->level = 31; // Level 3.1 支持 720p 30fps

        // 设置低延迟选项
        av_opt_set(_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(_ctx->priv_data, "tune", "zerolatency", 0);

        // 关键：告诉编码器不要在输出中包含全局头
        // (AV_CODEC_FLAG_GLOBAL_HEADER)， 因为我们需要带内参数集 (SPS/PPS)
        // 在每个关键帧发送，以适应 WebRTC 场景 _ctx->flags |=
        // AV_CODEC_FLAG_GLOBAL_HEADER;

        int ret = avcodec_open2(_ctx, codec, nullptr);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            throw std::runtime_error{"avcodec_open2 failed: " +
                                     std::string(errbuf)};
        }
    }

    std::vector<std::vector<uint8_t>>
    _packetize(const std::vector<uint8_t> &buf) {
        // 1. 将 Annex-B 转换为 NALU 列表
        auto nalus = _annexb_to_nalus(buf.data(), buf.size());
        std::vector<std::vector<uint8_t>> payloads;

        for (auto &nal : nalus) {
            if (nal.empty())
                continue;

            // 2. 关键修复：去除防止仿效字节 (0x00 0x00 0x03)
            // RTP 不允许这些字节存在
            nal.resize(remove_emulation_prevention(nal.data(), nal.size()));

            if (nal.empty())
                continue;
            uint8_t nalu_type = nal[0] & 0x1F;

            // 3. 打包逻辑
            if (nal.size() <= static_cast<size_t>(PACKET_MAX)) {
                // 单一 NALU 单元包
                payloads.push_back(std::move(nal));
            } else {
                // FU-A 分片
                size_t off = 1; // 跳过 NALU header
                while (off < nal.size()) {
                    // 计算 payload 大小，减去 FU-A header (2 bytes)
                    size_t chunk_size = std::min(
                        nal.size() - off, static_cast<size_t>(PACKET_MAX - 2));

                    bool start = (off == 1);
                    bool end = (off + chunk_size >= nal.size());

                    std::vector<uint8_t> fu;
                    fu.reserve(2 + chunk_size);

                    // FU Indicator: 原始 NALU header 的高3位 + Type 28
                    fu.push_back(static_cast<uint8_t>((nal[0] & 0xE0) | 28));

                    // FU Header: Start bit + End bit + 原始 NALU Type
                    fu.push_back(static_cast<uint8_t>((start ? 0x80 : 0) |
                                                      (end ? 0x40 : 0) |
                                                      (nalu_type & 0x1F)));

                    // Payload
                    fu.insert(fu.end(), nal.begin() + off,
                              nal.begin() + off + chunk_size);

                    payloads.push_back(std::move(fu));
                    off += chunk_size;
                }
            }
        }
        return payloads;
    }

    AVCodecContext *_ctx = nullptr;
    int _width = 0, _height = 0;
    int _bitrate = 0;
    int _max_framerate = 0;
};

class H264DecoderImpl : public decoder {
  public:
    H264DecoderImpl() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec)
            throw std::runtime_error{"Decoder not found"};

        _ctx = avcodec_alloc_context3(codec);

        // 关键修复：设置输入包的时间基为 90kHz，帮助解码器计算 PTS
        _ctx->pkt_timebase = {1, 90000};
        _ctx->framerate = {30, 1}; // 默认值，实际会从码流中更新

        // 尝试开启低延迟或快速解码选项
        // av_opt_set(_ctx->priv_data, "flags", "low_delay", 0);

        int ret = avcodec_open2(_ctx, codec, nullptr);
        if (ret < 0) {
            throw std::runtime_error{"avcodec_open2 failed for decoder"};
        }
    }

    ~H264DecoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::vector<media_frame> decode(const std::vector<uint8_t> &rtp_payload,
                                    uint32_t timestamp) override {
        if (rtp_payload.empty())
            return {};

        std::vector<uint8_t> nal;
        uint8_t first_byte = rtp_payload[0];
        uint8_t nalu_type = first_byte & 0x1F;

        if (nalu_type == 28) {
            // FU-A 处理
            if (rtp_payload.size() < 2)
                return {};
            uint8_t fu_header = rtp_payload[1];
            bool start = (fu_header & 0x80) != 0;
            bool end = (fu_header & 0x40) != 0;

            if (start) {
                _fu_buffer.clear();
                // 重组 NALU Header: FU Indicator 的高3位 + FU Header 的低5位
                uint8_t reconstructed_nal = static_cast<uint8_t>(
                    (first_byte & 0xE0) | (fu_header & 0x1F));
                _fu_buffer.push_back(reconstructed_nal);
            }

            _fu_buffer.insert(_fu_buffer.end(), rtp_payload.begin() + 2,
                              rtp_payload.end());

            if (!end) {
                return {}; // 等待后续分片
            }

            nal = std::move(_fu_buffer);
            _fu_buffer.clear();
        } else if (nalu_type >= 1 && nalu_type <= 23) {
            // 单一 NALU
            nal = rtp_payload;
        } else {
            // 其他类型 (STAP-A 等) 暂不支持直接处理，但可以尝试直接送入解码器
            // 简单处理：直接视为 NALU
            nal = rtp_payload;
        }

        // 添加 Annex-B 起始码
        std::vector<uint8_t> annexb_data;
        annexb_data.reserve(4 + nal.size());
        annexb_data.insert(annexb_data.end(), ANNEXB_START_4,
                           ANNEXB_START_4 + 4);
        annexb_data.insert(annexb_data.end(), nal.begin(), nal.end());

        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });

        // 注意：这里不能直接把 annexb_data.data() 赋值给 pkt->data
        // 因为 pkt 必须管理 buffer 生命周期，或者使用 av_packet_from_data
        // 简单起见，我们复制数据
        if (av_new_packet(pkt.get(), annexb_data.size()) < 0) {
            return {};
        }
        memcpy(pkt->data, annexb_data.data(), annexb_data.size());

        // 设置 PTS。RTP timestamp 是 90kHz。
        // 之前设置了 _ctx->pkt_timebase = {1, 90000}，所以这里的 PTS 就是 RTP
        // 值
        pkt->pts = timestamp;
        pkt->dts = timestamp; // 简化处理，DTS 通常等于 PTS 或由解码器推算

        int ret = avcodec_send_packet(_ctx, pkt.get());
        if (ret < 0) {
            // 解码器正在刷新或出错
            return {};
        }

        std::vector<media_frame> frames;
        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });

        while (true) {
            ret = avcodec_receive_frame(_ctx, f.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            media_frame mf;
            mf.kind = media_kind::video;
            mf.format = media_format::yuv420p;

            // 关键修复：将 PTS 转换回 90kHz
            // 编码器可能是其他 time_base 输出的，所以必须 rescale
            mf.timestamp = static_cast<uint32_t>(
                av_rescale_q(f->pts, _ctx->time_base, {1, 90000}));

            mf.width = f->width;
            mf.height = f->height;

            int buf_size = av_image_get_buffer_size(
                static_cast<AVPixelFormat>(f->format), f->width, f->height, 1);
            mf.data.resize(buf_size);

            av_image_copy_to_buffer(
                mf.data.data(), buf_size, f->data, f->linesize,
                static_cast<AVPixelFormat>(f->format), f->width, f->height, 1);

            frames.push_back(std::move(mf));
            av_frame_unref(f.get());
        }
        return frames;
    }

  private:
    AVCodecContext *_ctx = nullptr;
    std::vector<uint8_t> _fu_buffer;
};

} // namespace

std::shared_ptr<encoder> make_h264_encoder(const encoder_params &p) {
    return std::make_shared<H264EncoderImpl>(p);
}

std::shared_ptr<decoder> make_h264_decoder() {
    return std::make_shared<H264DecoderImpl>();
}

} // namespace asiortc::codecs
