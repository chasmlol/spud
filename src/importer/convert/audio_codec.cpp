#include "importer/convert/audio_codec.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"

#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>

#ifdef _WIN32
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace spud::importer {

namespace {

constexpr uint32_t riff_id(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

struct XwmaFile {
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t bytes_per_second = 0;
    uint16_t block_align = 0;
    uint16_t bits = 0;
    std::vector<uint32_t> dpds; // cumulative decoded PCM bytes after each packet
    std::span<const std::byte> data;
};

XwmaFile parse_xwma(std::span<const std::byte> file) {
    BinaryReader r(file);
    if (r.read<uint32_t>() != riff_id("RIFF")) {
        throw ParseError("not a RIFF file");
    }
    r.skip(4);
    if (r.read<uint32_t>() != riff_id("XWMA")) {
        throw ParseError("not an xWMA file");
    }
    XwmaFile x;
    bool have_fmt = false;
    while (r.tell() + 8 <= r.size()) {
        const auto id = r.read<uint32_t>();
        const auto size = r.read<uint32_t>();
        const auto body = r.take(std::min<size_t>(size, r.size() - r.tell()));
        if (id == riff_id("fmt ")) {
            BinaryReader f(body);
            x.format_tag = f.read<uint16_t>();
            x.channels = f.read<uint16_t>();
            x.sample_rate = f.read<uint32_t>();
            x.bytes_per_second = f.read<uint32_t>();
            x.block_align = f.read<uint16_t>();
            x.bits = f.read<uint16_t>();
            have_fmt = true;
        } else if (id == riff_id("dpds")) {
            x.dpds = BinaryReader(body).read_array<uint32_t>(body.size() / 4);
        } else if (id == riff_id("data")) {
            x.data = body;
        }
        if ((size & 1) && r.tell() < r.size()) {
            r.skip(1);
        }
    }
    if (!have_fmt || x.data.empty()) {
        throw ParseError("xWMA file without fmt or data");
    }
    if (x.channels == 0 || x.sample_rate == 0 || x.block_align == 0) {
        throw ParseError("xWMA file with an invalid fmt chunk");
    }
    return x;
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw Error(std::format("xWMA decode: {} failed (0x{:08X})", what, static_cast<uint32_t>(hr)));
    }
}

void media_foundation_startup() {
    static std::once_flag once;
    static HRESULT result = S_OK;
    std::call_once(once, [] { result = MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    check(result, "MFStartup");
}

// One COM apartment per calling thread for the duration of a decode.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

// Pulls every pending output buffer out of the decoder into `pcm`.
void drain_output(IMFTransform* mft, uint32_t buffer_size, std::vector<int16_t>& pcm) {
    for (;;) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateSample(&sample), "MFCreateSample");
        check(MFCreateMemoryBuffer(buffer_size, &buffer), "MFCreateMemoryBuffer");
        check(sample->AddBuffer(buffer.Get()), "AddBuffer");
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample.Get();
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) {
            out.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return;
        }
        check(hr, "ProcessOutput");
        ComPtr<IMFMediaBuffer> contiguous;
        check(sample->ConvertToContiguousBuffer(&contiguous), "ConvertToContiguousBuffer");
        BYTE* bytes = nullptr;
        DWORD length = 0;
        check(contiguous->Lock(&bytes, nullptr, &length), "Lock");
        const size_t count = length / sizeof(int16_t);
        const size_t at = pcm.size();
        pcm.resize(at + count);
        std::memcpy(pcm.data() + at, bytes, count * sizeof(int16_t));
        contiguous->Unlock();
    }
}

PcmClip decode_with_media_foundation(const XwmaFile& x) {
    media_foundation_startup();
    ComScope com;
    ComPtr<IMFTransform> mft;
    check(CoCreateInstance(CLSID_CWMADecMediaObject, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)),
          "creating the WMA decoder");

    ComPtr<IMFMediaType> in;
    check(MFCreateMediaType(&in), "MFCreateMediaType");
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    GUID subtype = MFAudioFormat_WMAudioV8;
    if (x.format_tag == 0x0162) {
        subtype = MFAudioFormat_WMAudioV9;
    } else if (x.format_tag != 0x0161) {
        throw Error(std::format("xWMA decode: unsupported format tag 0x{:04X}", x.format_tag));
    }
    in->SetGUID(MF_MT_SUBTYPE, subtype);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, x.channels);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, x.sample_rate);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, x.bytes_per_second);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, x.block_align);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    // xWMA carries no codec-private data. WMA v2 streams need the WMAUDIO2WAVEFORMAT extension; xWMA always
    // encodes with the v2 options 0x001F (exponent VLC, bit reservoir, variable block length, ...), which is
    // also what FFmpeg's xwma demuxer supplies. The decode is checked against FFmpeg's in
    if (x.format_tag == 0x0161) {
        uint8_t extra[10] = {};
        extra[4] = 0x1F;
        in->SetBlob(MF_MT_USER_DATA, extra, sizeof(extra));
    }
    check(mft->SetInputType(0, in.Get(), 0), "SetInputType");

    ComPtr<IMFMediaType> out;
    check(MFCreateMediaType(&out), "MFCreateMediaType");
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, x.channels);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, x.sample_rate);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, x.channels * 2);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, x.channels * 2 * x.sample_rate);
    check(mft->SetOutputType(0, out.Get(), 0), "SetOutputType");

    MFT_OUTPUT_STREAM_INFO info{};
    check(mft->GetOutputStreamInfo(0, &info), "GetOutputStreamInfo");
    const uint32_t buffer_size = std::max<uint32_t>(info.cbSize, 1u << 16);

    check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "BEGIN_STREAMING");
    PcmClip clip;
    clip.channels = x.channels;
    clip.sample_rate = x.sample_rate;
    const size_t packets = x.data.size() / x.block_align;
    LONGLONG time = 0;
    const LONGLONG packet_duration = 10'000'000LL * x.block_align / std::max<uint32_t>(x.bytes_per_second, 1);
    for (size_t p = 0; p < packets; ++p) {
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(x.block_align, &buffer), "MFCreateMemoryBuffer");
        BYTE* bytes = nullptr;
        check(buffer->Lock(&bytes, nullptr, nullptr), "Lock");
        std::memcpy(bytes, x.data.data() + p * x.block_align, x.block_align);
        buffer->Unlock();
        buffer->SetCurrentLength(x.block_align);
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "MFCreateSample");
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(time);
        sample->SetSampleDuration(packet_duration);
        time += packet_duration;
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            drain_output(mft.Get(), buffer_size, clip.samples);
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        check(hr, "ProcessInput");
        drain_output(mft.Get(), buffer_size, clip.samples);
    }
    check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "END_OF_STREAM");
    check(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "DRAIN");
    drain_output(mft.Get(), buffer_size, clip.samples);
    return clip;
}

#endif

} // namespace

FuzParts split_fuz(std::span<const std::byte> fuz) {
    BinaryReader r(fuz);
    if (r.read<uint32_t>() != riff_id("FUZE")) {
        throw ParseError("not a FUZ file");
    }
    r.skip(4); // version
    const auto lip_size = r.read<uint32_t>();
    FuzParts parts;
    parts.lip = r.take(lip_size);
    parts.audio = fuz.subspan(r.tell());
    return parts;
}

osv::LipTrack decode_lip(std::span<const std::byte> lip) {
    // The game and the game.
    BinaryReader r(lip);
    const auto version = r.read<uint32_t>();
    const auto size_field = r.read<uint32_t>();
    auto flags = r.read<uint32_t>();
    if (version != 1) {
        throw ParseError(std::format("lip header version {} (only 1 ships and is supported)", version));
    }
    if (!(flags & 0x8)) {
        flags |= 0x4; // without an explicit channel count every frame carries the modifiers
    }
    std::vector<std::byte> payload;
    if (flags & 0x1) {
        // Zero-run coding: a nonzero byte is itself; a zero byte is followed by a u16 count of zero bytes.
        payload.reserve(size_field);
        while (r.tell() < r.size()) {
            const auto b = r.read<std::byte>();
            if (b != std::byte{0}) {
                payload.push_back(b);
            } else {
                payload.insert(payload.end(), r.read<uint16_t>(), std::byte{0});
            }
            if (payload.size() > size_field) {
                throw ParseError("lip: zero-run data longer than the header says");
            }
        }
    } else {
        auto raw = r.take(std::min<size_t>(size_field, r.size() - r.tell()));
        payload.assign(raw.begin(), raw.end());
    }
    BinaryReader p(payload);
    const auto frames = p.read<uint32_t>();
    if (frames < 1 || frames > 1000000) {
        throw ParseError(std::format("lip: frame count {} out of range", frames));
    }
    osv::LipTrack t;
    t.start_frame = p.read<int32_t>();
    t.phoneme_count = (flags & 0x8) ? p.read<uint32_t>() : 16;
    t.modifier_count = (flags & 0x4) ? 17 : 0;
    const size_t row = size_t(t.phoneme_count) + t.modifier_count;
    t.frames = p.read_array<float>(size_t(frames) * row);
    return t;
}

bool is_xwma(std::span<const std::byte> file) {
    return file.size() >= 12 && std::memcmp(file.data(), "RIFF", 4) == 0 &&
           std::memcmp(file.data() + 8, "XWMA", 4) == 0;
}

PcmClip decode_xwma(std::span<const std::byte> file) {
    const auto x = parse_xwma(file);
#ifdef _WIN32
    auto clip = decode_with_media_foundation(x);
#else
    throw Error("xWMA decoding needs Windows Media Foundation");
#endif
    // The last dpds entry is the stream's decoded length in bytes of 16-bit PCM; the decoder pads the final
    // packet past it.
    if (!x.dpds.empty()) {
        const size_t want = x.dpds.back() / 2;
        if (clip.samples.size() > want) {
            clip.samples.resize(want);
        }
    }
    return clip;
}

std::vector<std::byte> encode_vorbis(const PcmClip& clip, float quality) {
    if (clip.channels == 0 || clip.sample_rate == 0) {
        throw Error("vorbis encode: empty clip");
    }
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, static_cast<long>(clip.channels), static_cast<long>(clip.sample_rate),
                               quality) != 0) {
        vorbis_info_clear(&vi);
        throw Error(
            std::format("vorbis encode: unsupported format ({} ch, {} Hz)", clip.channels, clip.sample_rate));
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "spud importer");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, 0x4F534B59); // fixed serial: the same input always gives the same file

    std::vector<std::byte> out;
    auto append_page = [&out](const ogg_page& page) {
        const auto* h = reinterpret_cast<const std::byte*>(page.header);
        const auto* b = reinterpret_cast<const std::byte*>(page.body);
        out.insert(out.end(), h, h + page.header_len);
        out.insert(out.end(), b, b + page.body_len);
    };
    {
        ogg_packet header, comments, codebooks;
        vorbis_analysis_headerout(&vd, &vc, &header, &comments, &codebooks);
        ogg_stream_packetin(&os, &header);
        ogg_stream_packetin(&os, &comments);
        ogg_stream_packetin(&os, &codebooks);
        ogg_page page;
        while (ogg_stream_flush(&os, &page) != 0) {
            append_page(page);
        }
    }

    const size_t frames = clip.frames();
    constexpr size_t k_chunk = 4096;
    size_t done = 0;
    bool eos = false;
    while (!eos) {
        const size_t n = std::min(k_chunk, frames - done);
        if (n == 0) {
            vorbis_analysis_wrote(&vd, 0);
        } else {
            float** buffer = vorbis_analysis_buffer(&vd, static_cast<int>(n));
            for (size_t i = 0; i < n; ++i) {
                for (uint32_t c = 0; c < clip.channels; ++c) {
                    buffer[c][i] = clip.samples[(done + i) * clip.channels + c] / 32768.0f;
                }
            }
            vorbis_analysis_wrote(&vd, static_cast<int>(n));
            done += n;
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&vd, &packet)) {
                ogg_stream_packetin(&os, &packet);
                ogg_page page;
                while (ogg_stream_pageout(&os, &page) != 0) {
                    append_page(page);
                    if (ogg_page_eos(&page)) {
                        eos = true;
                    }
                }
            }
        }
        if (n == 0) {
            ogg_page page;
            while (ogg_stream_flush(&os, &page) != 0) {
                append_page(page);
            }
            eos = true;
        }
    }
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return out;
}

float vorbis_quality_for(uint32_t channels, uint32_t source_bytes_per_second) {
    // libvorbis' nominal bit rates for 44.1 kHz stereo at quality -0.1 .. 1.0 (vorbisenc's setup tables).
    // Mono streams at the same quality land above half of these, so halving them per channel is conservative.
    static constexpr float k_stereo_kbps[] = {45, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 500};
    const float target_per_channel = 2.0f * source_bytes_per_second * 8.0f / 1000.0f / std::max(channels, 1u);
    for (int i = 0; i < 12; ++i) {
        if (k_stereo_kbps[i] / 2.0f >= target_per_channel) {
            return -0.1f + 0.1f * i;
        }
    }
    return 1.0f;
}

} // namespace spud::importer
