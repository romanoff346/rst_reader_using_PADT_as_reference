// deflate.cpp — минимальный DEFLATE-декодер (RFC 1951) + zlib-обёртка (RFC 1950)
//
// Реализация "каноническая" табличная, без динамического аллокатора на символ.
// Используется для чтения СЖАТЫХ записей ANSYS RST (ANSCOMP формат).
#include "rst_reader.h"

namespace rst_reader {
namespace deflate {
namespace {

// ============================================================================
// BitReader — побитовое чтение из буфера (LSB-first, как в DEFLATE)
// ============================================================================
struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;

    explicit BitReader(const uint8_t* s, size_t len) : p(s), n(len) {}

    bool get_bits(int cnt, uint32_t& out) {
        while (bitcnt < cnt) {
            if (pos >= n) return false;
            bitbuf |= static_cast<uint32_t>(p[pos++]) << bitcnt;
            bitcnt += 8;
        }
        out = bitbuf & ((1u << cnt) - 1);
        bitbuf >>= cnt;
        bitcnt -= cnt;
        return true;
    }
};

// ============================================================================
// Huffman — канонический Huffman-кодер (алгоритм из RFC 1951)
// ============================================================================
struct Huff {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
    int max_len = 0;

    bool build(const uint8_t* lengths, int nsym) {
        for (int i = 0; i <= 15; ++i) counts[i] = 0;
        max_len = 0;
        for (int i = 0; i < nsym; ++i) {
            if (lengths[i] > 15) return false;
            ++counts[lengths[i]];
            if (lengths[i] > max_len) max_len = lengths[i];
        }
        if (counts[0] == nsym) return true;  // все нулевые — дерево пустое
        int idx = 0;
        for (int l = 1; l <= 15; ++l)
            for (int s = 0; s < nsym; ++s)
                if (lengths[s] == l) symbols[idx++] = static_cast<uint16_t>(s);
        return true;
    }

    bool decode(BitReader& br, uint16_t& out) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= max_len; ++len) {
            uint32_t b;
            if (!br.get_bits(1, b)) return false;
            code |= static_cast<int>(b);
            int count = counts[len];
            if (code - first < count) {
                out = symbols[index + (code - first)];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }
};

// ============================================================================
// inflate_block — декодирование одного Huffman-блока
// ============================================================================

// Таблицы длин и дистанций (из RFC 1951)
static const uint16_t lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,
    193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577
};
static const uint8_t dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
    9,9,10,10,11,11,12,12,13,13
};

bool inflate_block(BitReader& br, const Huff& lit, const Huff& dist,
                   std::vector<uint8_t>& dst) {
    for (;;) {
        uint16_t sym;
        if (!lit.decode(br, sym)) return false;
        if (sym < 256) {
            dst.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;  // end of block
        } else {
            int li = sym - 257;
            if (li >= 29) return false;
            uint32_t extra;
            if (!br.get_bits(lext[li], extra)) return false;
            size_t length = lbase[li] + extra;
            uint16_t dsym;
            if (!dist.decode(br, dsym)) return false;
            if (dsym >= 30) return false;
            if (!br.get_bits(dext[dsym], extra)) return false;
            size_t distance = dbase[dsym] + extra;
            if (distance > dst.size()) return false;
            size_t start = dst.size() - distance;
            for (size_t k = 0; k < length; ++k)
                dst.push_back(dst[start + k]);
        }
    }
}

} // anonymous namespace

// ============================================================================
// inflate_raw — распаковка raw deflate потока (без zlib заголовка)
// ============================================================================
bool inflate_raw(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst) {
    dst.clear();
    BitReader br(src, src_len);
    for (;;) {
        uint32_t bfinal, btype;
        if (!br.get_bits(1, bfinal) || !br.get_bits(2, btype)) return false;
        
        if (btype == 0) {
            // Stored block (без сжатия)
            br.bitbuf = 0; br.bitcnt = 0;
            if (br.pos + 4 > br.n) return false;
            uint16_t len = static_cast<uint16_t>(src[br.pos] | (src[br.pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(src[br.pos + 2] | (src[br.pos + 3] << 8));
            if (static_cast<uint16_t>(~nlen & 0xFFFF) != len) return false;
            br.pos += 4;
            if (br.pos + len > br.n) return false;
            dst.insert(dst.end(), src + br.pos, src + br.pos + len);
            br.pos += len;
        } else if (btype == 1) {
            // Fixed Huffman tables
            uint8_t lens[288];
            for (int i = 0; i < 144; ++i) lens[i] = 8;
            for (int i = 144; i < 256; ++i) lens[i] = 9;
            for (int i = 256; i < 280; ++i) lens[i] = 7;
            for (int i = 280; i < 288; ++i) lens[i] = 8;
            Huff lit, dist;
            if (!lit.build(lens, 288)) return false;
            uint8_t dlens[30]; for (int i = 0; i < 30; ++i) dlens[i] = 5;
            if (!dist.build(dlens, 30)) return false;
            if (!inflate_block(br, lit, dist, dst)) return false;
        } else if (btype == 2) {
            // Dynamic Huffman tables
            uint32_t hlit, hdist, hclen;
            if (!br.get_bits(5, hlit) || !br.get_bits(5, hdist) || !br.get_bits(4, hclen))
                return false;
            hlit += 257; hdist += 1; hclen += 4;
            static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t clens[19] = {};
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t v; if (!br.get_bits(3, v)) return false;
                clens[order[i]] = static_cast<uint8_t>(v);
            }
            Huff cl;
            if (!cl.build(clens, 19)) return false;
            std::vector<uint8_t> lens(hlit + hdist);
            for (size_t i = 0; i < lens.size();) {
                uint16_t sym;
                if (!cl.decode(br, sym)) return false;
                if (sym < 16) { lens[i++] = static_cast<uint8_t>(sym); }
                else if (sym == 16) {
                    uint32_t rep; if (!br.get_bits(2, rep)) return false;
                    if (i == 0 || i + rep + 3 > lens.size()) return false;
                    uint8_t prev = lens[i - 1];
                    for (rep += 3; rep; --rep) lens[i++] = prev;
                } else if (sym == 17) {
                    uint32_t rep; if (!br.get_bits(3, rep)) return false;
                    if (i + rep + 3 > lens.size()) return false;
                    for (rep += 3; rep; --rep) lens[i++] = 0;
                } else {
                    uint32_t rep; if (!br.get_bits(7, rep)) return false;
                    if (i + rep + 11 > lens.size()) return false;
                    for (rep += 11; rep; --rep) lens[i++] = 0;
                }
            }
            Huff lit, dist;
            if (!lit.build(lens.data(), static_cast<int>(hlit))) return false;
            if (!dist.build(lens.data() + hlit, static_cast<int>(hdist))) return false;
            if (!inflate_block(br, lit, dist, dst)) return false;
        } else {
            return false;
        }
        if (bfinal) break;
    }
    return true;
}

// ============================================================================
// inflate_zlib — распаковка zlib-обёрнутого потока (CMF+FLG + deflate + adler32)
// ============================================================================
bool inflate_zlib(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst) {
    if (src_len < 3) return false;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) return false;                     // метод = deflate
    if (((cmf << 8) | flg) % 31 != 0) return false;          // контрольная сумма заголовка
    size_t skip = 2 + ((flg & 0x20) ? 4 : 0);               // FLEVEL/FDICT
    if (src_len <= skip) return false;
    return inflate_raw(src + skip, src_len - skip - 4, dst); // отбрасываем adler32
}

} // namespace deflate
} // namespace rst_reader
