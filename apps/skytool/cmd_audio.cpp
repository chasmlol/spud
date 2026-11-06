#include "commands.hpp"

#include "core/binary.hpp"
#include "core/file.hpp"
#include "core/game_random.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "importer/convert/audio_codec.hpp"
#include "importer/legacy/data_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace skytool {

namespace {

// A file from the merged data view, or from disk when the path exists there.
std::vector<std::byte> load(const std::string& path) {
    if (std::filesystem::exists(path)) {
        return spud::read_file(path);
    }
    spud::legacy::DataSource data(open_install());
    auto bytes = data.read(spud::path::normalize(path));
    if (!bytes) {
        throw spud::Error("not found in the data: " + path);
    }
    return std::move(*bytes);
}

std::vector<std::byte> wav_bytes(const spud::importer::PcmClip& clip) {
    spud::BinaryWriter w;
    const uint32_t data_size = static_cast<uint32_t>(clip.samples.size() * 2);
    w.write_string("RIFF");
    w.write<uint32_t>(36 + data_size);
    w.write_string("WAVEfmt ");
    w.write<uint32_t>(16);
    w.write<uint16_t>(1);
    w.write<uint16_t>(static_cast<uint16_t>(clip.channels));
    w.write<uint32_t>(clip.sample_rate);
    w.write<uint32_t>(clip.sample_rate * clip.channels * 2);
    w.write<uint16_t>(static_cast<uint16_t>(clip.channels * 2));
    w.write<uint16_t>(16);
    w.write_string("data");
    w.write<uint32_t>(data_size);
    w.write_bytes(std::as_bytes(std::span(clip.samples)));
    return w.take();
}

// `audio rng check <rngwatch.bin>`: every watched change of the game's generator (the game's
// ap_rngwatch: before and after states), against GameRandom's twist of the before state. A plain twist must
// come out word for word. A racing one either moves the upper 21 bits as a plain twist would and differs only
// in the low bits (the threads in step), or moves them otherwise.
int rng_check(const std::string& file) {
    const auto bytes = spud::read_file(file);
    constexpr size_t record = 24 + 2 * 624 * 4;
    size_t exact = 0, last_word = 0, in_step = 0, other = 0;
    for (size_t off = 0; off + record <= bytes.size(); off += record) {
        spud::GameRandom::State before{}, after{};
        std::memcpy(before.words.data(), bytes.data() + off + 24, 624 * 4);
        std::memcpy(after.words.data(), bytes.data() + off + 24 + 624 * 4, 624 * 4);
        spud::GameRandom g(0);
        g.set_state(before);
        g.twist();
        const auto mine = g.state();
        bool upper_same = true;
        for (uint32_t i = 0; i < 624; ++i) {
            const uint32_t d = (mine.words[i] ^ after.words[i]) >> 11;
            upper_same &= d == 0 || d == (0x9908B0DFu >> 11);
        }
        if (mine.words == after.words) {
            ++exact;
        } else if (std::equal(mine.words.begin(), mine.words.end() - 1, after.words.begin())) {
            ++last_word; // the last step reads the index field, which another thread's draw had moved
        } else if (upper_same) {
            ++in_step;
        } else {
            ++other;
        }
    }
    spud::log::info(
        "{}: {} changes: {} plain twists reproduced word for word, {} all but the last word (the index "
        "field it reads had moved), {} racing in step (low bits differ), {} racing otherwise or partial",
        file, exact + last_word + in_step + other, exact, last_word, in_step, other);
    return 0;
}

// `audio rng sessions <n> [lead s] [length s]`: the region one-shot rule at Riverwood at 23:00 (six
// one-shots, chance x 0.5 in list order, at most one per pass, a pass every 0.5 s) for n sessions of the
// given length, drawing from GameRandom with the
// rest of the game's draws stood in for (advance), each session seeded differently and run for the lead
// first.
int rng_sessions(int n, double lead, double length) {
    const float chances[] = {0.03f, 0.03f, 0.07f, 0.10f, 0.09f, 0.05f};
    std::vector<int> counts;
    for (int k = 0; k < n; ++k) {
        spud::GameRandom g(1762200000u + static_cast<uint32_t>(k) * 7919u);
        g.advance(lead);
        int starts = 0;
        for (int pass = 0; pass < static_cast<int>(length / 0.5 + 0.5); ++pass) {
            g.advance(0.5);
            for (const float c : chances) {
                if (c * 0.5f >= g.next_unit()) {
                    ++starts;
                    break;
                }
            }
        }
        counts.push_back(starts);
        std::printf("session %d: %d one-shots\n", k, starts);
    }
    std::sort(counts.begin(), counts.end());
    auto at = [&](double q) { return counts[static_cast<size_t>(q * (counts.size() - 1) + 0.5)]; };
    std::printf(
        "%d sessions of %.0f s after %.0f s: median %d, quartiles %d-%d, 10-90 %% %d-%d, min %d, max %d\n", n,
        length, lead, at(0.5), at(0.25), at(0.75), at(0.1), at(0.9), counts.front(), counts.back());
    return 0;
}

} // namespace

int cmd_audio(Args args) {
    auto pos = positionals(args);
    if (pos.size() >= 3 && pos[0] == "rng" && pos[1] == "check") {
        return rng_check(pos[2]);
    }
    if (pos.size() >= 3 && pos[0] == "rng" && pos[1] == "sessions") {
        return rng_sessions(std::stoi(pos[2]), pos.size() >= 4 ? std::stod(pos[3]) : 60.0,
                            pos.size() >= 5 ? std::stod(pos[4]) : 180.0);
    }
    if (pos.size() < 3) {
        std::puts(
            "audio decode <file or data path> <out.wav>   xWMA / FUZ audio to 16-bit PCM WAV\n"
            "audio encode <file or data path> <out.ogg>   the importer's conversion (xWMA/FUZ -> Ogg "
            "Vorbis)\n"
            "audio lip <file or data path> <out.lip>      the lip-sync part of a FUZ\n"
            "audio rng check <rngwatch.bin>                the game generator's plain twists, reproduced\n"
            "audio rng sessions <n> [lead s] [length s]    one-shot counts per session (default 180 s)");
        return 1;
    }
    using namespace spud::importer;
    auto bytes = load(pos[1]);
    std::span<const std::byte> audio = bytes;
    std::span<const std::byte> lip;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "FUZE", 4) == 0) {
        auto parts = split_fuz(bytes);
        audio = parts.audio;
        lip = parts.lip;
    }
    if (pos[0] == "lip") {
        spud::write_file(pos[2], lip);
        spud::log::info("wrote {} bytes of lip data", lip.size());
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    auto clip = decode_xwma(audio);
    const double decode_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    spud::log::info("decoded {} frames, {} ch, {} Hz ({:.3f} s of audio in {:.3f} s)", clip.frames(), clip.channels,
                    clip.sample_rate, double(clip.frames()) / clip.sample_rate, decode_s);
    if (pos[0] == "decode") {
        spud::write_file(pos[2], wav_bytes(clip));
        return 0;
    }
    if (pos[0] == "encode") {
        const uint32_t source_bps = spud::BinaryReader(audio.subspan(28, 4)).read<uint32_t>();
        const float quality = vorbis_quality_for(clip.channels, source_bps);
        auto ogg = encode_vorbis(clip, quality);
        spud::write_file(pos[2], ogg);
        const double seconds = double(clip.frames()) / clip.sample_rate;
        spud::log::info("vorbis q{:.1f}: {} bytes ({:.1f} kbps; source {:.1f} kbps)", quality * 10, ogg.size(),
                        ogg.size() * 8 / seconds / 1000, source_bps * 8 / 1000.0);
        return 0;
    }
    spud::log::error("unknown audio subcommand");
    return 1;
}

} // namespace skytool
