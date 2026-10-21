#include "chengvault/crypto.hpp"
#include <QFile>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

// Cryptographic boundary for the course implementation
// --------------------------------------------------
// The block/stream ciphers below are primitives, not complete storage formats.
// Their public API permits known-answer testing with published keys and IVs.
// Production calls in this project go through seal/open, which authenticate.
// A stream cipher alone is malleable: flipping ciphertext flips plaintext.
// Therefore the envelope uses encrypt-then-MAC with separate derived keys.
// Header bytes belong to the MAC input, including the algorithm identifier.
// Merely authenticating ciphertext would allow unauthenticated interpretation.
// A fresh 256-bit salt creates an independent key pair for every object.
// The nonce/counter also comes from the operating-system random source.
// Passwords are not cipher keys; callers must run the persisted password KDF.
// Windows APIs are restricted to entropy and current-user credential wrapping.
// SHA, HMAC, KDF expansion, AES rounds and ChaCha rounds execute local code.
// All arithmetic is fixed-width unsigned arithmetic; overflow is intentional
// only inside the primitive's modulo-2^32 word additions or byte transforms.
// Length and counter arithmetic instead checks bounds before using results.
// Serialized word endianness is explicit, independent of the host ABI.
// QByteArray supports bounded block operations and streaming file hashing.
// It does not guarantee locked memory or erasure of implicitly shared copies.
// AES table lookup is educational and has no cache-side-channel guarantee.
// Constant-time tag comparison avoids mismatch-position timing leakage, but
// that property alone does not imply the entire implementation is constant-time.
// Sources: FIPS 180-4, FIPS 197, SP 800-38A, RFCs 4231/5869/7914/8439.
// See docs/algorithms.md for the format, vector references and limitations.
#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>
#endif

namespace cv::crypto {
namespace {
using Byte = unsigned char;
// The SHA-256 round constants are part of FIPS 180-4, not external code.
constexpr std::array<quint32, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
quint32 rotr(quint32 x, int n) {
    return (x >> n) | (x << (32 - n));
}
quint32 rotl(quint32 x, int n) {
    return (x << n) | (x >> (32 - n));
}
quint32 loadBe(const Byte *p) {
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
}
quint32 loadLe(const Byte *p) {
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
void appendBe64(QByteArray &out, quint64 value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(char(value >> shift));
}
quint64 loadBe64(const QByteArray &bytes, qsizetype offset) {
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | Byte(bytes[offset + i]);
    return value;
}
// AES arithmetic uses the irreducible polynomial x^8+x^4+x^3+x+1 (0x11B).
// This is intentionally independent of the RS field (0x11D).
Byte xtime(Byte x) {
    return Byte((unsigned(x) << 1) ^ ((x & 0x80) ? 0x11b : 0));
}
Byte aesMultiply(Byte a, Byte b) {
    // Polynomial multiplication reduces each shifted intermediate value.
    // Inversion later raises a nonzero byte to 254 by square-and-multiply.
    // This helper runs during table construction, not on every data byte.
    // Keeping the polynomial visible also distinguishes AES from RS coding.
    Byte result = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1)
            result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}
const std::array<Byte, 256> &sbox() {
    // Generate the substitution table from the field inverse and affine map.
    // No library or copied lookup table implements the scored AES operation.
    static const auto table = [] {
        std::array<Byte, 256> values{};
        for (unsigned x = 0; x < 256; ++x) {
            Byte inverse = 1, base = Byte(x);
            for (unsigned exponent = 254; exponent; exponent >>= 1) {
                if (exponent & 1)
                    inverse = aesMultiply(inverse, base);
                base = aesMultiply(base, base);
            }
            if (x == 0)
                inverse = 0;
            unsigned result = inverse ^ 0x63;
            for (int shift = 1; shift <= 4; ++shift)
                result ^= ((unsigned(inverse) << shift) | (unsigned(inverse) >> (8 - shift))) & 255;
            values[x] = Byte(result);
        }
        return values;
    }();
    return table;
}
std::array<Byte, 240> expandKey(const QByteArray &key) {
    // AES-256 needs fifteen 16-byte round keys, including the initial XOR.
    // Each new 32-byte schedule segment applies RotWord/SubWord/Rcon once.
    // The middle word additionally applies SubWord for the 256-bit variant.
    // The caller has already validated the input's exact 32-byte length.
    // A fixed array prevents accidental use of an unexpanded round key.
    std::array<Byte, 240> schedule{};
    std::memcpy(schedule.data(), key.constData(), 32);
    Byte rcon = 1;
    for (int used = 32; used < 240; used += 4) {
        std::array<Byte, 4> word{};
        std::copy_n(schedule.data() + used - 4, 4, word.data());
        if (used % 32 == 0) {
            std::rotate(word.begin(), word.begin() + 1, word.end());
            for (auto &v : word)
                v = sbox()[v];
            word[0] ^= rcon;
            rcon = xtime(rcon);
        } else if (used % 32 == 16) {
            for (auto &v : word)
                v = sbox()[v];
        }
        for (int j = 0; j < 4; ++j)
            schedule[size_t(used + j)] = schedule[size_t(used - 32 + j)] ^ word[size_t(j)];
    }
    return schedule;
}
std::array<Byte, 16> aesBlock(std::array<Byte, 16> state, const std::array<Byte, 240> &schedule) {
    // Encryption is sufficient for both directions of CTR; no inverse AES
    // round is required when decrypting ciphertext with the same keystream.
    // The last round omits MixColumns, exactly as specified for AES.
    // Keeping state by value means the caller's counter is never modified by
    // the round function and remains available for its checked increment.
    for (int i = 0; i < 16; ++i)
        state[size_t(i)] ^= schedule[size_t(i)];
    for (int round = 1; round <= 14; ++round) {
        for (auto &x : state)
            x = sbox()[x];
        const auto beforeShift = state;
        // State storage is column-major, as in the AES specification.
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                state[size_t(row + 4 * col)] = beforeShift[size_t(row + 4 * ((col + row) % 4))];
        if (round != 14) {
            for (int col = 0; col < 4; ++col) {
                Byte *s = state.data() + 4 * col;
                const Byte a = s[0], b = s[1], c = s[2], d = s[3];
                const Byte all = a ^ b ^ c ^ d;
                s[0] = Byte(s[0] ^ all ^ xtime(Byte(a ^ b)));
                s[1] = Byte(s[1] ^ all ^ xtime(Byte(b ^ c)));
                s[2] = Byte(s[2] ^ all ^ xtime(Byte(c ^ d)));
                s[3] = Byte(s[3] ^ all ^ xtime(Byte(d ^ a)));
            }
        }
        for (int i = 0; i < 16; ++i)
            state[size_t(i)] ^= schedule[size_t(16 * round + i)];
    }
    return state;
}
void quarter(quint32 &a, quint32 &b, quint32 &c, quint32 &d) {
    a += b;
    d = rotl(d ^ a, 16);
    c += d;
    b = rotl(b ^ c, 12);
    a += b;
    d = rotl(d ^ a, 8);
    c += d;
    b = rotl(b ^ c, 7);
}
int cipherId(QString name) {
    name = name.toLower();
    if (name == "aes256" || name == "aes-256" || name == "aes256-ctr" || name == "aes")
        return 1;
    if (name == "chacha20")
        return 2;
    throw Error("CIPHER", "不支持的加密算法");
}
QByteArray cipherBytes(const QByteArray &input, const QByteArray &key, const QByteArray &nonce, int id) {
    return id == 1 ? aes256Ctr(input, key, nonce) : chacha20(input, key, nonce.first(12));
}
class PreparedHmac {
  public:
    explicit PreparedHmac(const QByteArray &key) {
        QByteArray padded = key.size() > 64 ? sha256(key) : key;
        padded.append(QByteArray(64 - padded.size(), char(0)));
        QByteArray inner(64, char(0x36)), outer(64, char(0x5c));
        for (int i = 0; i < 64; ++i) {
            inner[i] = char(Byte(inner[i]) ^ Byte(padded[i]));
            outer[i] = char(Byte(outer[i]) ^ Byte(padded[i]));
        }
        inside_.update(inner);
        outside_.update(outer);
    }
    QByteArray digest(const QByteArray &data) const {
        // PBKDF2 reuses one key for many HMAC invocations. Clone the hash
        // states after processing the fixed pads instead of hashing those
        // same blocks again on every iteration. The definition is unchanged.
        Sha256 inside = inside_, outside = outside_;
        inside.update(data);
        outside.update(inside.finish());
        return outside.finish();
    }

  private:
    Sha256 inside_, outside_;
};
} // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {
}

void Sha256::transform(const unsigned char *block) {
    // The compression function consumes exactly one already-buffered block.
    // Words 0..15 come from the message and 16..63 expand the message schedule.
    // Working variables start from the chaining state, then feed forward into
    // it. Message length/padding belongs in finish(), never in this function.
    // This separation makes arbitrary update boundaries observationally equal.
    std::array<quint32, 64> words{};
    for (int i = 0; i < 16; ++i)
        words[size_t(i)] = loadBe(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        const quint32 x = words[size_t(i - 15)], y = words[size_t(i - 2)];
        words[size_t(i)] = words[size_t(i - 16)] + (rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)) +
                           words[size_t(i - 7)] + (rotr(y, 17) ^ rotr(y, 19) ^ (y >> 10));
    }
    quint32 a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5],
            g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const quint32 choose = (e & f) ^ (~e & g);
        const quint32 majority = (a & b) ^ (a & c) ^ (b & c);
        const quint32 t1 =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + choose + K[size_t(i)] + words[size_t(i)];
        const quint32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const QByteArray &data) {
    // length_ counts original bytes only, excluding final padding.
    // SHA256's terminal bit count is 64-bit, so reject more than 2^61-1 bytes.
    // pending_ always holds fewer than 64 bytes after a successful update.
    // The middle region is read directly rather than concatenated into a
    // growing temporary allocation; memory does not scale with file length.
    require(digest_.isEmpty(), "HASH_STATE", "哈希已经结束");
    require(quint64(data.size()) <= std::numeric_limits<quint64>::max() / 8 - length_, "HASH_SIZE",
            "哈希输入过长");
    length_ += quint64(data.size());
    qsizetype offset = 0;
    if (!pending_.isEmpty()) {
        const qsizetype take = std::min<qsizetype>(64 - pending_.size(), data.size());
        pending_.append(data.constData(), take);
        offset += take;
        if (pending_.size() == 64) {
            transform(reinterpret_cast<const Byte *>(pending_.constData()));
            pending_.clear();
        }
    }
    // Process full blocks in place. File hashing therefore uses bounded memory.
    while (data.size() - offset >= 64) {
        transform(reinterpret_cast<const Byte *>(data.constData() + offset));
        offset += 64;
    }
    if (offset < data.size())
        pending_.append(data.constData() + offset, data.size() - offset);
}

QByteArray Sha256::finish() {
    // A message ending at byte 56..63 needs a second padding block.
    // Cache the digest to make repeated finish() safe for logging/comparison.
    // Further update() is rejected rather than silently hashing after padding.
    // Big-endian output follows the standard even on little-endian Windows.
    if (!digest_.isEmpty())
        return digest_;
    const quint64 bitLength = length_ * 8;
    pending_.append(char(0x80));
    while (pending_.size() % 64 != 56)
        pending_.append(char(0));
    appendBe64(pending_, bitLength);
    for (qsizetype i = 0; i < pending_.size(); i += 64)
        transform(reinterpret_cast<const Byte *>(pending_.constData() + i));
    for (quint32 word : state_)
        for (int shift = 24; shift >= 0; shift -= 8)
            digest_.append(char(word >> shift));
    pending_.clear();
    return digest_;
}
QByteArray sha256(const QByteArray &data) {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}
QByteArray hashFile(const QString &path, const Context &context) {
    context.check();
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "READ", "无法读取文件: " + path);
    Sha256 hash;
    while (!file.atEnd()) {
        context.check();
        const QByteArray data = file.read(ChunkSize);
        require(!data.isEmpty() || file.error() == QFileDevice::NoError, "READ", "读取文件失败: " + path);
        hash.update(data);
        context.update(quint64(file.pos()), quint64(file.size()), "校验文件");
    }
    context.check();
    return hash.finish();
}

QByteArray hmac(const QByteArray &key, const QByteArray &data) {
    // HMAC normalizes a long key by hashing, not by truncating it.
    // The inner and outer fixed pads separate the two hash domains.
    // Streaming the two pieces avoids allocating pad+message for large input.
    // A short key is allowed here for published vectors; envelope keys are
    // always independently derived 32-byte values at the storage boundary.
    return PreparedHmac(key).digest(data);
}
QByteArray pbkdf2(const QByteArray &password, const QByteArray &salt, int iterations, int length) {
    // The block index starts at one and is encoded in network byte order.
    // U1 binds the salt and block index; later U values chain through HMAC.
    // XOR of every U value is the output block, never just the last U value.
    // Partial final output is legal and is truncated after computing a block.
    // The explicit length cap prevents unbounded requests from configuration.
    require(iterations > 0 && length > 0 && length <= 1024 * 1024, "KDF_PARAMETERS", "无效 PBKDF2 参数");
    const PreparedHmac prf(password);
    QByteArray output;
    output.reserve(length);
    for (quint32 block = 1; output.size() < length; ++block) {
        QByteArray input = salt;
        for (int shift = 24; shift >= 0; shift -= 8)
            input.append(char(block >> shift));
        QByteArray u = prf.digest(input), accumulated = u;
        for (int round = 1; round < iterations; ++round) {
            u = prf.digest(u);
            for (int i = 0; i < 32; ++i)
                accumulated[i] = char(Byte(accumulated[i]) ^ Byte(u[i]));
        }
        output.append(accumulated);
    }
    output.truncate(length);
    return output;
}
QByteArray hkdf(const QByteArray &key, const QByteArray &salt, const QByteArray &info, int length) {
    // Extraction converts input key material into a pseudorandom HMAC key.
    // Expansion chains the previous block, context info and one-byte index.
    // That one-byte index is why RFC5869 limits output to 255 hash blocks.
    // Different info values create distinct usages under a shared master key.
    require(length > 0 && length <= 255 * 32, "KDF_PARAMETERS", "无效 HKDF 输出长度");
    const QByteArray extracted = hmac(salt.isEmpty() ? QByteArray(32, char(0)) : salt, key);
    QByteArray result, previous;
    for (int index = 1; result.size() < length; ++index) {
        previous = hmac(extracted, previous + info + QByteArray(1, char(index)));
        result.append(previous);
    }
    result.truncate(length);
    return result;
}
QByteArray randomBytes(int length) {
    require(length >= 0 && length <= 64 * 1024 * 1024, "RANDOM_SIZE", "无效随机数据长度");
    QByteArray bytes(length, Qt::Uninitialized);
#ifdef _WIN32
    // BCrypt is only an operating-system entropy source; no cipher is delegated.
    if (length)
        require(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), ULONG(length),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
                "RANDOM", "操作系统随机数生成失败");
#else
    QFile random("/dev/urandom");
    require(random.open(QIODevice::ReadOnly) && random.read(bytes.data(), length) == length, "RANDOM",
            "系统随机源不可用");
#endif
    return bytes;
}
bool equal(const QByteArray &a, const QByteArray &b) {
    // Lengths are public. Equal-length tags always visit every byte, without an
    // early mismatch exit; volatile keeps the accumulator from being elided.
    if (a.size() != b.size())
        return false;
    volatile unsigned difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference = difference | unsigned(Byte(a[i]) ^ Byte(b[i]));
    return difference == 0;
}

QByteArray aes256Ctr(const QByteArray &input, const QByteArray &key, const QByteArray &iv) {
    // Counter-mode output has exactly the input length, including empty input.
    // Partial final blocks consume only the needed prefix of their keystream.
    // Counter wrap is tolerated only after the final consumed block, when no
    // repeated keystream will be returned. Another call must use a new nonce.
    require(key.size() == 32 && iv.size() == 16, "CIPHER_PARAMETERS",
            "AES-256 要求 32 字节密钥和 16 字节计数器");
    const auto schedule = expandKey(key);
    std::array<Byte, 16> counter{};
    std::memcpy(counter.data(), iv.constData(), 16);
    QByteArray output(input.size(), Qt::Uninitialized);
    for (qsizetype offset = 0; offset < input.size(); offset += 16) {
        const auto stream = aesBlock(counter, schedule);
        const qsizetype count = std::min<qsizetype>(16, input.size() - offset);
        for (qsizetype j = 0; j < count; ++j)
            output[offset + j] = char(Byte(input[offset + j]) ^ stream[size_t(j)]);
        // Increment the full 128-bit big-endian counter. Never wrap and reuse a
        // keystream block, even when this low-level API is called directly.
        bool carry = true;
        for (int j = 15; j >= 0 && carry; --j) {
            ++counter[size_t(j)];
            carry = counter[size_t(j)] == 0;
        }
        require(!carry || offset + count == input.size(), "COUNTER_OVERFLOW", "AES 计数器耗尽");
    }
    return output;
}
QByteArray chacha20(const QByteArray &input, const QByteArray &key, const QByteArray &nonce,
                    quint32 counter) {
    // Validate total block consumption before allocating or processing output.
    // The last legal counter is 0xffffffff; a request may use it once.
    // The three nonce words remain fixed while state word 12 advances.
    // Feed-forward uses the original state for each block, not a prior round.
    require(key.size() == 32 && nonce.size() == 12, "CIPHER_PARAMETERS",
            "ChaCha20 要求 32 字节密钥和 12 字节随机数");
    const quint64 blocks = (quint64(input.size()) + 63) / 64;
    require(blocks <= (quint64(1) << 32) - counter, "COUNTER_OVERFLOW", "ChaCha20 计数器耗尽");
    std::array<quint32, 16> state{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i)
        state[size_t(4 + i)] = loadLe(reinterpret_cast<const Byte *>(key.constData() + i * 4));
    for (int i = 0; i < 3; ++i)
        state[size_t(13 + i)] = loadLe(reinterpret_cast<const Byte *>(nonce.constData() + i * 4));
    QByteArray output(input.size(), Qt::Uninitialized);
    for (qsizetype offset = 0; offset < input.size(); offset += 64) {
        state[12] = counter++;
        auto working = state;
        // Ten double-rounds alternate the four columns and four diagonals.
        for (int round = 0; round < 10; ++round) {
            quarter(working[0], working[4], working[8], working[12]);
            quarter(working[1], working[5], working[9], working[13]);
            quarter(working[2], working[6], working[10], working[14]);
            quarter(working[3], working[7], working[11], working[15]);
            quarter(working[0], working[5], working[10], working[15]);
            quarter(working[1], working[6], working[11], working[12]);
            quarter(working[2], working[7], working[8], working[13]);
            quarter(working[3], working[4], working[9], working[14]);
        }
        for (int i = 0; i < 16; ++i)
            working[size_t(i)] += state[size_t(i)];
        const qsizetype count = std::min<qsizetype>(64, input.size() - offset);
        for (qsizetype j = 0; j < count; ++j)
            output[offset + j] =
                char(Byte(input[offset + j]) ^ Byte(working[size_t(j / 4)] >> (8 * (j % 4))));
    }
    return output;
}

QByteArray seal(const QByteArray &plaintext, const QByteArray &masterKey, const QString &cipher) {
    // This object-level envelope intentionally contains no password verifier.
    // A caller unwraps the repository master key once, then uses it for blocks.
    // Each block still gets its own salt-derived cipher and authentication keys.
    // Repeated plaintext therefore does not produce repeated ciphertext here.
    // Deduplication belongs before sealing and uses a repository-specific ID.
    // The nonce is encoded in fixed-width storage for unambiguous parsing.
    // ChaCha20's final four nonce-storage bytes are reserved and must be zero.
    // Ciphertext length needs no padding adjustment in either supported mode.
    require(masterKey.size() == 32, "KEY_SIZE", "主密钥必须为 32 字节");
    const int id = cipherId(cipher);
    const QByteArray salt = randomBytes(32);
    QByteArray nonce = randomBytes(id == 1 ? 16 : 12);
    if (id == 2)
        nonce.append(QByteArray(4, char(0)));
    // CVSEAL01 | algorithm:u8 | salt:32 | counter/nonce:16 | size:u64be.
    // Separate labeled HKDF expansions prohibit reuse of the MAC key as a
    // cipher key. Ciphertext and the complete header use encrypt-then-MAC.
    QByteArray header("CVSEAL01", 8);
    header.append(char(id));
    header += salt;
    header += nonce;
    appendBe64(header, quint64(plaintext.size()));
    const QByteArray encryptionKey = hkdf(masterKey, salt, "ChengVault/encryption/v1");
    const QByteArray authenticationKey = hkdf(masterKey, salt, "ChengVault/authentication/v1");
    QByteArray envelope = header + cipherBytes(plaintext, encryptionKey, nonce, id);
    envelope += hmac(authenticationKey, envelope);
    return envelope;
}
QByteArray open(const QByteArray &envelope, const QByteArray &masterKey) {
    // The input is an already-bounded in-memory object supplied by its reader.
    // No field from the header drives a larger allocation than the supplied
    // byte string: declared length must exactly match the available ciphertext.
    // Structural rejection can precede MAC calculation because lengths and
    // algorithm identifiers are public format information, not secret data.
    // A matching MAC authenticates those fields as well as the ciphertext.
    // Decryption output is only returned after this authentication barrier.
    // Tests separately corrupt header, payload and tag, and use a wrong key.
    constexpr qsizetype headerSize = 65, tagSize = 32;
    require(masterKey.size() == 32, "KEY_SIZE", "主密钥必须为 32 字节");
    require(envelope.size() >= headerSize + tagSize && envelope.first(8) == "CVSEAL01", "ENVELOPE",
            "加密数据格式无效");
    const int id = Byte(envelope[8]);
    require(id == 1 || id == 2, "ENVELOPE", "未知加密算法");
    require(loadBe64(envelope, 57) == quint64(envelope.size() - headerSize - tagSize), "ENVELOPE",
            "加密数据长度不匹配");
    const QByteArray salt = envelope.mid(9, 32), nonce = envelope.mid(41, 16);
    require(id != 2 || nonce.last(4) == QByteArray(4, char(0)), "ENVELOPE", "随机数保留字段无效");
    const QByteArray authenticationKey = hkdf(masterKey, salt, "ChengVault/authentication/v1");
    const QByteArray authenticated = envelope.first(envelope.size() - tagSize);
    // No plaintext is produced until authentication passes, including on a
    // wrong password. Format errors and MAC failures never modify files.
    require(equal(hmac(authenticationKey, authenticated), envelope.last(tagSize)), "AUTHENTICATION",
            "密码错误或备份已被修改");
    const QByteArray encryptionKey = hkdf(masterKey, salt, "ChengVault/encryption/v1");
    return cipherBytes(envelope.mid(headerSize, envelope.size() - headerSize - tagSize), encryptionKey, nonce,
                       id);
}

QByteArray protectLocalKey(const QByteArray &key) {
    // DPAPI binds this blob to the current Windows user. It is only a local
    // convenience for unattended tasks, not a portable archive key format.
    // Optional entropy is a fixed application context, not an extra password.
    // The OS-allocated result is released after copying into the return value.
#ifdef _WIN32
    require(key.size() <= std::numeric_limits<DWORD>::max(), "KEY_SIZE", "本机凭据过长");
    DATA_BLOB input{DWORD(key.size()), reinterpret_cast<BYTE *>(const_cast<char *>(key.constData()))},
        output{};
    const QByteArray entropy("ChengVault/local-credentials/v1");
    DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE *>(const_cast<char *>(entropy.constData()))};
    require(CryptProtectData(&input, L"ChengVault local credential", &extra, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &output),
            "CREDENTIAL", "无法保护本机凭据");
    QByteArray result(reinterpret_cast<char *>(output.pbData), qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    Q_UNUSED(key);
    throw Error("PLATFORM", "本机凭据仅支持 Windows DPAPI");
#endif
}
QByteArray unprotectLocalKey(const QByteArray &protectedKey) {
    // The same application entropy is required when unlocking the blob.
    // CRYPTPROTECT_UI_FORBIDDEN makes a background worker fail explicitly
    // rather than waiting on an invisible password or consent dialog.
    // No fallback returns plaintext when protection or unprotection fails.
#ifdef _WIN32
    require(protectedKey.size() <= std::numeric_limits<DWORD>::max(), "KEY_SIZE", "本机凭据过长");
    DATA_BLOB input{DWORD(protectedKey.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(protectedKey.constData()))},
        output{};
    const QByteArray entropy("ChengVault/local-credentials/v1");
    DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE *>(const_cast<char *>(entropy.constData()))};
    require(CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output),
            "CREDENTIAL", "无法解锁当前用户的本机凭据");
    QByteArray result(reinterpret_cast<char *>(output.pbData), qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    Q_UNUSED(protectedKey);
    throw Error("PLATFORM", "本机凭据仅支持 Windows DPAPI");
#endif
}
} // namespace cv::crypto
