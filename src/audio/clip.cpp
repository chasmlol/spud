#include "audio/clip.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/file.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace spud::audio {

namespace {

struct Wave {
    ClipInfo info;
    std::span<const std::byte> data;
};

Wave parse_wave(std::span<const std::byte> file, const std::filesystem::path& name) {
    BinaryReader r(file);
    if (file.size() < 12 || r.read_string(4) != "RIFF") {
        throw ParseError(name.string() + ": not a RIFF file");
    }
    r.skip(4);
    if (r.read_string(4) != "WAVE") {
        throw ParseError(name.string() + ": not a WAVE file");
    }
    Wave w;
    uint16_t tag = 0, block_align = 0, bits = 0;
    while (r.tell() + 8 <= r.size()) {
        const auto id = r.read_string(4);
        const auto size = r.read<uint32_t>();
        const auto body = r.take(std::min<size_t>(size, r.size() - r.tell()));
        if (id == "fmt " && body.size() >= 16) {
            BinaryReader f(body);
            tag = f.read<uint16_t>();
            w.info.channels = f.read<uint16_t>();
            w.info.sample_rate = f.read<uint32_t>();
            f.skip(4);
            block_align = f.read<uint16_t>();
            bits = f.read<uint16_t>();
        } else if (id == "data") {
            w.data = body;
        } else if (id == "smpl" && body.size() >= 36) {
            // The game: the first sample loop's start and end, when the
            // chunk lists any loops.
            BinaryReader s(body);
            s.skip(28);
            const auto loops = s.read<uint32_t>();
            if (loops != 0 && body.size() >= 52) {
                s.seek(44);
                w.info.loop_begin = s.read<uint32_t>();
                w.info.loop_end = s.read<uint32_t>();
                w.info.has_smpl_loop = true;
            }
        }
        if ((size & 1) && r.tell() < r.size()) {
            r.skip(1);
        }
    }
    if (tag != 1 || bits != 16 || w.info.channels == 0 || block_align != w.info.channels * 2) {
        throw ParseError(
            std::format("{}: unsupported WAV format (tag {}, {} bits)", name.string(), tag, bits));
    }
    w.info.frames = w.data.size() / block_align;
    if (!w.info.has_smpl_loop) {
        w.info.loop_begin = 0;
        w.info.loop_end = static_cast<uint32_t>(w.info.frames);
    }
    return w;
}

bool is_ogg(const std::filesystem::path& file) {
    auto ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".ogg";
}

class VorbisStream final : public ClipStream {
  public:
    VorbisStream(std::vector<std::byte> bytes, const std::filesystem::path& name)
        : m_bytes(std::move(bytes)) {
        int error = 0;
        m_vorbis = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(m_bytes.data()),
                                          static_cast<int>(m_bytes.size()), &error, nullptr);
        if (!m_vorbis) {
            throw ParseError(
                std::format("{}: not a readable Ogg Vorbis file (error {})", name.string(), error));
        }
        const auto vi = stb_vorbis_get_info(m_vorbis);
        m_info.channels = static_cast<uint32_t>(vi.channels);
        m_info.sample_rate = vi.sample_rate;
        m_info.frames = stb_vorbis_stream_length_in_samples(m_vorbis);
        m_info.loop_begin = 0;
        m_info.loop_end = static_cast<uint32_t>(m_info.frames);
    }
    ~VorbisStream() override { stb_vorbis_close(m_vorbis); }

    const ClipInfo& info() const override { return m_info; }

    size_t read(int16_t* out, size_t frames) override {
        size_t done = 0;
        while (done < frames) {
            const int n = stb_vorbis_get_samples_short_interleaved(
                m_vorbis, static_cast<int>(m_info.channels), out + done * m_info.channels,
                static_cast<int>((frames - done) * m_info.channels));
            if (n <= 0) {
                break;
            }
            done += static_cast<size_t>(n);
        }
        return done;
    }

    void seek(uint64_t frame) override { stb_vorbis_seek(m_vorbis, static_cast<unsigned int>(frame)); }

  private:
    std::vector<std::byte> m_bytes;
    stb_vorbis* m_vorbis = nullptr;
    ClipInfo m_info;
};

class WaveStream final : public ClipStream {
  public:
    WaveStream(std::vector<std::byte> bytes, const std::filesystem::path& name) : m_bytes(std::move(bytes)) {
        auto w = parse_wave(m_bytes, name);
        m_info = w.info;
        m_data = w.data;
    }
    const ClipInfo& info() const override { return m_info; }
    size_t read(int16_t* out, size_t frames) override {
        const size_t n = std::min<uint64_t>(frames, m_info.frames - m_pos);
        std::memcpy(out, m_data.data() + m_pos * m_info.channels * 2, n * m_info.channels * 2);
        m_pos += n;
        return n;
    }
    void seek(uint64_t frame) override { m_pos = std::min(frame, m_info.frames); }

  private:
    std::vector<std::byte> m_bytes;
    std::span<const std::byte> m_data;
    ClipInfo m_info;
    uint64_t m_pos = 0;
};

} // namespace

std::unique_ptr<ClipStream> open_stream(const std::filesystem::path& file) {
    auto bytes = read_file(file);
    if (is_ogg(file)) {
        return std::make_unique<VorbisStream>(std::move(bytes), file);
    }
    return std::make_unique<WaveStream>(std::move(bytes), file);
}

std::shared_ptr<const Clip> load_clip(const std::filesystem::path& file) {
    auto clip = std::make_shared<Clip>();
    auto bytes = read_file(file);
    if (is_ogg(file)) {
        VorbisStream s(std::move(bytes), file);
        clip->info = s.info();
        clip->pcm.resize(clip->info.frames * clip->info.channels);
        const size_t got = s.read(clip->pcm.data(), clip->info.frames);
        clip->pcm.resize(got * clip->info.channels);
        clip->info.frames = got;
        clip->info.loop_end = static_cast<uint32_t>(got);
    } else {
        auto w = parse_wave(bytes, file);
        clip->info = w.info;
        clip->pcm.resize(w.info.frames * w.info.channels);
        std::memcpy(clip->pcm.data(), w.data.data(), clip->pcm.size() * 2);
    }
    return clip;
}

ClipInfo probe_clip(const std::filesystem::path& file) {
    return open_stream(file)->info();
}

} // namespace spud::audio
