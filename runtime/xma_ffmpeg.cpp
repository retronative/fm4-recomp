// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// One XMA frame at a time through FFmpeg's "xmaframes" decoder (patches/ffmpeg/0001, Xenia's raw-frame decoder),
// as Xenia xenia-canary 67f16c4e315f apu/xma_context.cc:635-671 feeds it: one byte of bit padding sizes
// (3 bits before the frame, 3 after, 2 unused; xma_context.cc:643-644), then the frame from bit 8, padded to a byte.
// The frame here always starts at bit 8 (padding before 0). The decoder holds back its very first frame (WMA Pro's
// skip_frame: no samples, avcodec_receive_frame gives EAGAIN); that frame writes nothing (apu::XmaPass), so the
// hardware's own start-up latency is APPROXIMATE. Every other FFmpeg result than a whole frame is an error.
#include "xma_ffmpeg.h"
#include "stops.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <cstdio>
#include <cstring>

namespace runtime::xma {
namespace {
class FfmpegDecoder final : public apu::XmaFrameDecoder
{
public:
    ~FfmpegDecoder() override
    {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&context_);
    }

    bool Open(uint32_t rate, uint32_t channels, std::string& error)
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        context_ = codec ? avcodec_alloc_context3(codec) : nullptr;
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!context_ || !packet_ || !frame_)
            return error = "FFmpeg allocation failed", false;
        context_->sample_rate = int(rate);
        av_channel_layout_default(&context_->ch_layout, int(channels));
        if (const int r = avcodec_open2(context_, codec, nullptr); r < 0)
            return error = "avcodec_open2: " + Text(r), false;
        channels_ = channels;
        return true;
    }

    bool Decode(const uint8_t* bits, uint32_t bitCount, std::vector<float> (&channels)[2], uint32_t& samples, std::string& error) override
    {
        samples = 0;
        const int size = int(1 + (bitCount + 7) / 8);
        if (const int r = av_new_packet(packet_, size); r < 0)  // zeroed padding after `size`
            return error = "av_new_packet: " + Text(r), false;
        std::memset(packet_->data, 0, size_t(size));
        const uint32_t paddingEnd = uint32_t(size) * 8 - (8 + bitCount);
        packet_->data[0] = uint8_t((paddingEnd & 7) << 2);  // padding before: 0
        std::memcpy(packet_->data + 1, bits, (bitCount + 7) / 8);
        if (bitCount % 8)
            packet_->data[size - 1] &= uint8_t(0xFF << (8 - bitCount % 8));  // the frame's bits only
        int r = avcodec_send_packet(context_, packet_);
        av_packet_unref(packet_);
        if (r < 0)
            return error = "avcodec_send_packet: " + Text(r), false;
        r = avcodec_receive_frame(context_, frame_);
        if (r == AVERROR(EAGAIN))
            return true;  // the decoder's first frame: no samples
        if (r < 0)
            return error = "avcodec_receive_frame: " + Text(r), false;
        if (frame_->format != AV_SAMPLE_FMT_FLTP || uint32_t(frame_->ch_layout.nb_channels) != channels_)
            return error = "unexpected frame format or channel count", false;
        samples = uint32_t(frame_->nb_samples);
        for (uint32_t c = 0; c < channels_; c++)
        {
            const float* plane = reinterpret_cast<const float*>(frame_->extended_data[c]);
            channels[c].assign(plane, plane + samples);
        }
        av_frame_unref(frame_);
        return true;
    }

private:
    static std::string Text(int code)
    {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(code, text, sizeof text);
        return text;
    }
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    uint32_t channels_ = 0;
};
}  // namespace

apu::XmaDecoderFactory FfmpegDecoderFactory()
{
    const char* license = avcodec_license();
    if (avcodec_version() != LIBAVCODEC_VERSION_INT || std::strncmp(license, "LGPL", 4) != 0 ||
        avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) == nullptr)
        Stop(StopCode::Setup, "XMA: the linked libavcodec (%u, %s) is not the pinned LGPL xmaframes build (tools/ffmpeg-xma)",
             avcodec_version(), license);
    std::fprintf(stderr, "[xma] FFmpeg libavcodec %u.%u.%u, %s, decoder xmaframes\n", avcodec_version() >> 16, (avcodec_version() >> 8) & 0xFF,
                 avcodec_version() & 0xFF, license);
    return [](uint32_t rate, uint32_t channels, std::string& error) -> std::unique_ptr<apu::XmaFrameDecoder> {
        auto decoder = std::make_unique<FfmpegDecoder>();
        if (!decoder->Open(rate, channels, error))
            return nullptr;
        return decoder;
    };
}

}  // namespace runtime::xma
