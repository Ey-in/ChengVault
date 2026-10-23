#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include "chengvault/repair.hpp"
#include "test_support.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <array>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

using cv::test::check;
using cv::test::throws;
namespace {
QByteArray hex(const char *text) {
    // Qt's permissive decoder accepts odd nibbles; strict test fixtures must
    // reject transcription mistakes before diagnosing a primitive failure.
    const QByteArray source(text), decoded = QByteArray::fromHex(source);
    check(source.size() % 2 == 0 && decoded.toHex() == source.toLower(),
          "malformed published test-vector hex");
    return decoded;
}
QByteArray noise(qsizetype length, quint32 seed = 0x41a3f875) {
    // Deterministic fixture generator, deliberately unrelated to scored
    // compression/hash functions. It is never used as cryptographic entropy.
    QByteArray result(length, Qt::Uninitialized);
    for (qsizetype i = 0; i < length; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        result[i] = char(seed);
    }
    return result;
}
void flip(const QString &path, qint64 offset) {
    QFile file(path);
    check(file.open(QIODevice::ReadWrite));
    check(file.seek(offset));
    QByteArray data = file.read(1);
    check(data.size() == 1);
    data[0] = char(static_cast<unsigned char>(data[0]) ^ 0x5a);
    check(file.seek(offset));
    check(file.write(data) == 1);
}
quint64 big64(const QByteArray &data, qsizetype offset) {
    quint64 result = 0;
    for (int i = 0; i < 8; ++i)
        result = (result << 8) | static_cast<unsigned char>(data[offset + i]);
    return result;
}
void store64(QByteArray &data, qsizetype offset, quint64 value) {
    for (int i = 7; i >= 0; --i) {
        data[offset + i] = char(value);
        value >>= 8;
    }
}
template <class F> void failsWith(const QString &code, F &&operation) {
    // Checking the domain code distinguishes the intended parser barrier from
    // an unrelated failure, including the final digest check of a bad fixture.
    try {
        operation();
    } catch (const cv::Error &error) {
        check(error.code == code, "expected " + code + ", received " + error.code);
        return;
    }
    check(false, "expected domain error " + code);
}
QByteArray compressedFixture(char codec, const QByteArray &plain, const QByteArray &payload) {
    // Construct noncanonical but legal wire fixtures independently of the
    // codec encoder: raw fallback would otherwise hide small decoder cases.
    auto block = cv::compression::encode(plain, "none").first(53);
    block[4] = codec;
    store64(block, 13, quint64(payload.size()));
    return block + payload;
}
void storeFrequency(QByteArray &table, unsigned symbol, quint32 frequency) {
    for (int index = 3; index >= 0; --index) {
        table[qsizetype(symbol * 4 + unsigned(index))] = char(frequency);
        frequency >>= 8;
    }
}
QByteArray sidecarFixture(const QByteArray &json, const QByteArray &parity = {}) {
    // Both independent locators have valid checksums. Mutations of JSON fields
    // therefore reach semantic validation instead of failing a checksum first.
    QByteArray length(8, char(0));
    store64(length, 0, quint64(json.size()));
    const auto digest = cv::crypto::sha256(json);
    return "CVRHEAD1" + length + digest + json + parity + json + digest + length + "CVRTAIL1";
}
QJsonObject emptySidecarMetadata() {
    return {{"version", 1},          {"shardSize", int(cv::ChunkSize)},
            {"dataShards", 10},      {"parityShards", 2},
            {"originalSize", "0"},   {"archiveSha256", QString::fromLatin1(cv::crypto::sha256("").toHex())},
            {"groups", QJsonArray{}}};
}
#ifdef _WIN32
struct DenyDeleteLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit DenyDeleteLock(const QString &path) {
        // A real Windows sharing restriction allows temporary writes but
        // prevents the final replacement. This exercises commit failure
        // without requiring an administrator, a full disk, or fake I/O.
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "cannot acquire destination sharing lock");
    }
    ~DenyDeleteLock() {
        CloseHandle(handle);
    }
    DenyDeleteLock(const DenyDeleteLock &) = delete;
    DenyDeleteLock &operator=(const DenyDeleteLock &) = delete;
};
#endif
} // namespace

void algorithmTests(cv::test::Suite &suite) {
    using namespace cv;
    suite.run("SHA256/FIPS-empty", "白盒单元", [] {
        check(crypto::sha256("").toHex() ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    });
    suite.run("SHA256/FIPS-abc", "白盒单元", [] {
        check(crypto::sha256("abc").toHex() ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    });
    suite.run("SHA256/FIPS-multiblock", "白盒单元", [] {
        check(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").toHex() ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    });
    suite.run("SHA256/FIPS-million-a-stream", "白盒单元", [] {
        crypto::Sha256 hash;
        for (int i = 0; i < 1000; ++i)
            hash.update(QByteArray(1000, 'a'));
        check(hash.finish().toHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    });
    suite.run("SHA256/finish-idempotent-reject-update", "异常健壮性", [] {
        crypto::Sha256 hash;
        hash.update("abc");
        const auto value = hash.finish();
        check(hash.finish() == value);
        throws([&] { hash.update("d"); });
    });
    for (int boundary : {0, 1, 55, 56, 63, 64, 65, 127, 128, 129}) {
        suite.run(QString("SHA256/stream-split-%1").arg(boundary), "白盒单元", [boundary] {
            const auto data = noise(1025);
            crypto::Sha256 hash;
            hash.update(data.first(boundary));
            hash.update(data.mid(boundary));
            check(hash.finish() == crypto::sha256(data));
        });
    }
    // Expected values are published known-answer vectors, not generated by
    // encrypting then decrypting with the implementation under test.
    suite.run("HMAC/RFC4231-case1", "白盒单元", [] {
        check(crypto::hmac(QByteArray(20, char(0x0b)), "Hi There").toHex() ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    });
    suite.run("HMAC/RFC4231-case2", "白盒单元", [] {
        check(crypto::hmac("Jefe", "what do ya want for nothing?").toHex() ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    });
    suite.run("HMAC/RFC4231-long-key", "白盒单元", [] {
        check(crypto::hmac(QByteArray(131, char(0xaa)),
                           "Test Using Larger Than Block-Size Key - Hash Key First")
                  .toHex() == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    });
    suite.run("PBKDF2/RFC7914-SHA256", "白盒单元", [] {
        check(crypto::pbkdf2("passwd", "salt", 1, 64).toHex() ==
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef"
              "317c71b845b1e30bd509112041d3a19783");
    });
    suite.run("PBKDF2/SHA256-iterations-2", "白盒单元", [] {
        check(crypto::pbkdf2("password", "salt", 2).toHex() ==
              "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    });
    suite.run("KDF/invalid-parameters", "安全", [] {
        throws([] { crypto::pbkdf2("x", "y", 0); });
        throws([] { crypto::pbkdf2("x", "y", 1, 0); });
        throws([] { crypto::hkdf("x", "", "", 8161); });
        throws([] { crypto::hkdf("x", "", "", 0); });
    });
    suite.run("HKDF/RFC5869-case1", "白盒单元", [] {
        check(crypto::hkdf(QByteArray(22, char(0x0b)), hex("000102030405060708090a0b0c"),
                           hex("f0f1f2f3f4f5f6f7f8f9"), 42)
                  .toHex() ==
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    });
    suite.run("HKDF/RFC5869-empty-salt-info", "白盒单元", [] {
        check(crypto::hkdf(QByteArray(22, char(0x0b)), "", "", 42).toHex() ==
              "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    });
    suite.run("AES256CTR/NIST-SP800-38A-F5", "白盒单元", [] {
        const auto key = hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
        const auto iv = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        const auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce"
                               "411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
        // NIST SP800-38A F.5.5, printed page 57: keep blocks on separate
        // lines so their exact 16-byte lengths are independently reviewable.
        const auto expected = hex("601ec313775789a5b7a7f504bbf3d228"
                                  "f443e3ca4d62b59aca84e990cacaf5c5"
                                  "2b0930daa23de94ce87017ba2d84988d"
                                  "dfc9c58db67aada613c2dd08457941a6");
        const auto actual = crypto::aes256Ctr(plain, key, iv);
        check(actual == expected, "NIST known-vector actual=" + QString::fromLatin1(actual.toHex()));
        check(crypto::aes256Ctr(expected, key, iv) == plain);
    });
    suite.run("AES256CTR/counter-exhaustion", "安全", [] {
        check(
            crypto::aes256Ctr(QByteArray(16, 'x'), QByteArray(32, 'k'), QByteArray(16, char(0xff))).size() ==
            16);
        throws(
            [] { crypto::aes256Ctr(QByteArray(17, 'x'), QByteArray(32, 'k'), QByteArray(16, char(0xff))); });
    });
    suite.run("ChaCha20/RFC8439-block", "白盒单元", [] {
        const auto key = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        check(crypto::chacha20(QByteArray(64, char(0)), key, hex("000000090000004a00000000"), 1).toHex() ==
              "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02"
              "a2b5129cd1de164eb9cbd083e8a2503c4e");
    });
    suite.run("ChaCha20/RFC8439-sunscreen", "白盒单元", [] {
        const auto key = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        const QByteArray plain("Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
                               "for the future, sunscreen would be it.");
        check(crypto::chacha20(plain, key, hex("000000000000004a00000000"), 1).toHex() ==
              "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3"
              "571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab779"
              "37365af90bbf74a35be6b40b8eedf2785e42874d");
    });
    suite.run("ChaCha20/counter-exhaustion", "安全", [] {
        check(crypto::chacha20(QByteArray(64, 'x'), QByteArray(32, 'k'), QByteArray(12, 'n'),
                               std::numeric_limits<quint32>::max())
                  .size() == 64);
        throws([] {
            crypto::chacha20(QByteArray(65, 'x'), QByteArray(32, 'k'), QByteArray(12, 'n'),
                             std::numeric_limits<quint32>::max());
        });
    });
    suite.run("Crypto/invalid-key-nonce", "安全", [] {
        throws([] { crypto::aes256Ctr("", "short", QByteArray(16, 'i')); });
        throws([] { crypto::chacha20("", QByteArray(32, 'k'), "short"); });
        throws([] { crypto::seal("x", QByteArray(32, 'k'), "none"); });
        throws([] { crypto::seal("x", "short", "aes256"); });
        check(!crypto::equal("a", "b") && !crypto::equal("a", "aa") && crypto::equal("abc", "abc"));
    });
    suite.run("Crypto/independent-parameter-lengths", "安全", [] {
        failsWith("CIPHER_PARAMETERS", [] { crypto::aes256Ctr("", QByteArray(32, 'k'), "short"); });
        failsWith("CIPHER_PARAMETERS", [] { crypto::chacha20("", "short", QByteArray(12, 'n')); });
        failsWith("KEY_SIZE", [] { crypto::open(QByteArray(97, 'x'), "short"); });
        failsWith("KDF_PARAMETERS", [] { crypto::pbkdf2("p", "s", 1, 1024 * 1024 + 1); });
        failsWith("RANDOM_SIZE", [] { crypto::randomBytes(64 * 1024 * 1024 + 1); });
        check(crypto::equal({}, {}));
    });
    suite.run("KDF/partial-block-and-HKDF-maximum", "白盒单元", [] {
        check(crypto::pbkdf2("password", "salt", 2, 17).toHex() == "ae4d0c95af6b46d32d0adff928f06dd02a");
        const auto maximum = crypto::hkdf(QByteArray(22, char(0x0b)), "", "", 8160);
        check(maximum.size() == 8160 &&
              maximum.first(42).toHex() ==
                  "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    });
    for (const QString alias : {QString("aes-256"), QString("aes256-ctr"), QString("aes")}) {
        suite.run("Envelope/AES-alias-" + alias, "集成", [alias] {
            const QByteArray key(32, 'k');
            const auto envelope = crypto::seal("alias contract", key, alias);
            check(envelope[8] == 1 && crypto::open(envelope, key) == "alias contract");
        });
    }
    suite.run("Envelope/authenticated-reserved-nonce-rejected", "安全", [] {
        const QByteArray key(32, 'k');
        auto envelope = crypto::seal("payload", key, "chacha20");
        envelope[53] = 1;
        const auto macKey = crypto::hkdf(key, envelope.mid(9, 32), "ChengVault/authentication/v1");
        envelope.replace(envelope.size() - 32, 32,
                         crypto::hmac(macKey, envelope.first(envelope.size() - 32)));
        failsWith("ENVELOPE", [&] { crypto::open(envelope, key); });
    });
    for (const QString cipher : {QString("aes256"), QString("chacha20")}) {
        for (int length : {0, 1, 15, 16, 17, 63, 64, 65, 1048576}) {
            suite.run(QString("Envelope/%1/roundtrip-%2").arg(cipher).arg(length), "集成", [cipher, length] {
                const auto plain = noise(length), key = noise(32),
                           envelope = crypto::seal(plain, key, cipher);
                check(crypto::open(envelope, key) == plain);
                check(envelope != crypto::seal(plain, key, cipher), "salt and nonce must change");
            });
        }
        suite.run("Envelope/" + cipher + "/wrong-password", "安全", [cipher] {
            const auto envelope = crypto::seal("private file names and bytes", QByteArray(32, 'k'), cipher);
            throws([&] { crypto::open(envelope, QByteArray(32, 'w')); });
        });
        for (int offset : {0, 8, 9, 40, 41, 56, 57, 64, 65, 100}) {
            suite.run(QString("Envelope/%1/tamper-%2").arg(cipher).arg(offset), "安全", [cipher, offset] {
                const QByteArray key(32, 'k');
                auto envelope = crypto::seal(QByteArray(100, 'p'), key, cipher);
                envelope[offset] = char(static_cast<unsigned char>(envelope[offset]) ^ 1);
                throws([&] { crypto::open(envelope, key); });
            });
        }
        suite.run("Envelope/" + cipher + "/truncate-append-tag", "安全", [cipher] {
            const QByteArray key(32, 'k');
            const auto envelope = crypto::seal("payload", key, cipher);
            throws([&] { crypto::open(envelope.first(20), key); });
            throws([&] { crypto::open(envelope.first(envelope.size() - 1), key); });
            throws([&] { crypto::open(envelope + 'x', key); });
            auto badTag = envelope;
            badTag[badTag.size() - 1] ^= 1;
            throws([&] { crypto::open(badTag, key); });
        });
    }
    suite.run("Entropy/OS-random-length-and-distinct", "系统功能", [] {
        check(crypto::randomBytes(0).isEmpty());
        check(crypto::randomBytes(32) != crypto::randomBytes(32));
        throws([] { crypto::randomBytes(-1); });
    });
#ifdef _WIN32
    suite.run("Credentials/DPAPI-current-user-roundtrip", "集成", [] {
        const auto key = crypto::randomBytes(32), encrypted = crypto::protectLocalKey(key);
        check(encrypted != key && crypto::unprotectLocalKey(encrypted) == key);
        throws([&] { crypto::unprotectLocalKey(encrypted.first(10)); });
    });
#endif

    const std::array<QByteArray, 8> fixtures = {QByteArray{},
                                                QByteArray("x"),
                                                QByteArray(16384, 'A'),
                                                QByteArray("ABCD").repeated(16384),
                                                noise(65536),
                                                noise(65535) + QByteArray(9000, 'z'),
                                                QByteArray(1048576, char(0)),
                                                noise(1048576)};
    for (const QString algorithm : {QString("none"), QString("huffman"), QString("lz77")}) {
        for (size_t index = 0; index < fixtures.size(); ++index) {
            suite.run(QString("Compression/%1/fixture-%2").arg(algorithm).arg(index), "白盒单元",
                      [&, algorithm, index] {
                          const auto packed = compression::encode(fixtures[index], algorithm);
                          check(compression::decode(packed) == fixtures[index]);
                          if (index == 2 && algorithm != "none")
                              check(packed.size() < fixtures[index].size());
                          if (index == 4)
                              check(packed[4] == 0, "incompressible data must use raw fallback");
                      });
        }
        suite.run("Compression/" + algorithm + "/bounded-output", "安全", [algorithm] {
            const auto packed = compression::encode(QByteArray(10000, 'x'), algorithm);
            throws([&] { compression::decode(packed, 9999); });
            throws([&] { compression::decode(packed, -1); });
        });
        suite.run("Compression/" + algorithm + "/truncate-append-digest", "异常健壮性", [algorithm] {
            const auto packed = compression::encode(QByteArray(4096, 'x'), algorithm);
            throws([&] { compression::decode(packed.first(packed.size() - 1)); });
            throws([&] { compression::decode(packed + 'x'); });
            auto altered = packed;
            altered[21] ^= 1;
            throws([&] { compression::decode(altered); });
        });
    }
    suite.run("Compression/unknown-algorithm", "异常健壮性", [] {
        throws([] { compression::encode("x", "zip"); });
        auto block = compression::encode("x", "none");
        block[4] = 99;
        throws([&] { compression::decode(block); });
    });
    suite.run("Compression/forged-64bit-length", "安全", [] {
        auto block = compression::encode("x", "none");
        store64(block, 5, std::numeric_limits<quint64>::max());
        throws([&] { compression::decode(block); });
    });
    suite.run("Huffman/corrupt-frequency-table", "安全", [] {
        auto block = compression::encode(QByteArray(10000, 'A'), "huffman");
        check(block[4] == 1);
        block[53] = 1;
        throws([&] { compression::decode(block); });
    });
    suite.run("Huffman/multi-symbol-bits", "白盒单元", [] {
        const auto input =
            QByteArray(10000, 'A') + QByteArray(2000, 'B') + QByteArray(1000, 'C') + QByteArray(500, 'D');
        const auto packed = compression::encode(input, "huffman");
        check(packed[4] == 1);
        check(compression::decode(packed) == input);
        auto bad = packed;
        bad[bad.size() - 1] ^= 0x80;
        throws([&] { compression::decode(bad); });
    });
    suite.run("LZ77/zero-distance-forgery", "安全", [] {
        auto packed = compression::encode(QByteArray(4096, 'a'), "lz77");
        check(packed[4] == 2);
        // First token is a literal; the next is an overlapping distance=1 match.
        packed[55] = 0;
        packed[56] = 0;
        throws([&] { compression::decode(packed); });
    });
    suite.run("LZ77/window-and-overlap", "白盒单元", [] {
        auto input = noise(65530);
        input += input.first(200);
        input += QByteArray("abc").repeated(9000);
        check(compression::decode(compression::encode(input, "lz77")) == input);
    });
    suite.run("Compression/short-header-wrong-magic-and-raw-length", "安全", [] {
        failsWith("COMPRESSED_FORMAT", [] { compression::decode("CVC1"); });
        auto block = compression::encode("abc", "none");
        block[0] = 'X';
        failsWith("COMPRESSED_FORMAT", [&] { compression::decode(block); });
        block[0] = 'C';
        store64(block, 5, 2);
        failsWith("COMPRESSED_FORMAT", [&] { compression::decode(block); });
    });
    suite.run("Compression/hard-block-limit-independent-of-caller-bound", "安全", [] {
        auto block = compression::encode("", "none");
        store64(block, 5, 64 * 1024 * 1024 + 1);
        failsWith("BLOCK_SIZE", [&] { compression::decode(block, 128 * 1024 * 1024); });
        failsWith("BLOCK_SIZE", [] { compression::encode(QByteArray(64 * 1024 * 1024 + 1, 'x'), "none"); });
    });
    suite.run("Huffman/empty-nonraw-and-trailing-byte", "异常健壮性", [] {
        const QByteArray table(1024, char(0));
        check(compression::decode(compressedFixture(1, "", table), 0).isEmpty());
        failsWith("COMPRESSED_FORMAT", [&] { compression::decode(compressedFixture(1, "", table + 'x')); });
        failsWith("COMPRESSED_FORMAT",
                  [&] { compression::decode(compressedFixture(1, "", table.first(1023))); });
    });
    suite.run("Huffman/single-symbol-extra-stream-rejected", "安全", [] {
        QByteArray table(1024, char(0));
        storeFrequency(table, 'A', 1);
        check(compression::decode(compressedFixture(1, "A", table)) == "A");
        failsWith("COMPRESSED_FORMAT",
                  [&] { compression::decode(compressedFixture(1, "A", table + char(0))); });
    });
    suite.run("Huffman/two-symbol-valid-padding", "白盒单元", [] {
        QByteArray table(1024, char(0));
        storeFrequency(table, 'A', 1);
        storeFrequency(table, 'B', 1);
        check(compression::decode(compressedFixture(1, "AB", table + char(0x40))) == "AB");
    });
    for (const auto &item :
         std::array<std::pair<const char *, QByteArray>, 4>{{{"truncated-bits", {}},
                                                             {"wrong-frequencies", QByteArray(1, char(0))},
                                                             {"nonzero-padding", QByteArray(1, char(0x41))},
                                                             {"extra-byte", hex("4000")}}}) {
        suite.run("Huffman/" + QString(item.first), "安全", [bits = item.second] {
            QByteArray table(1024, char(0));
            storeFrequency(table, 'A', 1);
            storeFrequency(table, 'B', 1);
            failsWith("COMPRESSED_FORMAT",
                      [&] { compression::decode(compressedFixture(1, "AB", table + bits)); });
        });
    }
    suite.run("LZ77/handwritten-literal-and-overlapping-match", "白盒单元", [] {
        check(compression::decode(compressedFixture(2, "AAAA", hex("0241000100"))) == "AAAA");
        check(compression::decode(compressedFixture(2, "abcdefgh", QByteArray(1, char(0)) + "abcdefgh")) ==
              "abcdefgh");
        check(compression::decode(compressedFixture(2, "", ""), 0).isEmpty());
    });
    struct LzFailure {
        const char *name;
        QByteArray plain;
        QByteArray payload;
    };
    for (const auto &item : std::array<LzFailure, 8>{{{"missing-flags", "A", {}},
                                                      {"missing-literal", "A", hex("00")},
                                                      {"short-match", "AAA", hex("010001")},
                                                      {"distance-after-output", "AAAA", hex("0241000200")},
                                                      {"match-exceeds-remaining", "AAA", hex("0241000100")},
                                                      {"unused-flags", "A", hex("0241")},
                                                      {"trailing-payload", "A", hex("004100")},
                                                      {"empty-with-payload", "", hex("00")}}}) {
        suite.run("LZ77/" + QString(item.name), "安全", [item] {
            failsWith("COMPRESSED_FORMAT",
                      [&] { compression::decode(compressedFixture(2, item.plain, item.payload)); });
        });
    }

    QVector<QByteArray> data;
    for (int shard = 0; shard < 10; ++shard)
        data.append(noise(4097, quint32(shard + 37)));
    const auto encoded = repair::encodeShards(data);
    suite.run("RS12-10/systematic-data-unchanged", "白盒单元", [&] {
        check(encoded.size() == 12);
        for (int i = 0; i < 10; ++i)
            check(encoded[i] == data[i]);
        check(encoded[10] != encoded[11]);
    });
    for (int first = 0; first < 12; ++first) {
        suite.run(QString("RS12-10/single-erasure-%1").arg(first), "白盒单元", [&, first] {
            auto damaged = encoded;
            QVector<bool> present(12, true);
            present[first] = false;
            damaged[first].clear();
            check(repair::recoverShards(damaged, present) == encoded);
        });
        for (int second = first + 1; second < 12; ++second) {
            suite.run(QString("RS12-10/double-erasure-%1-%2").arg(first).arg(second), "白盒单元",
                      [&, first, second] {
                          auto damaged = encoded;
                          QVector<bool> present(12, true);
                          present[first] = present[second] = false;
                          damaged[first].clear();
                          damaged[second].clear();
                          check(repair::recoverShards(damaged, present) == encoded);
                      });
        }
    }
    suite.run("RS12-10/three-erasures-rejected", "异常健壮性", [&] {
        QVector<bool> present(12, true);
        present[0] = present[5] = present[11] = false;
        throws([&] { repair::recoverShards(encoded, present); });
    });
    suite.run("RS12-10/inconsistent-survivor-rejected", "安全", [&] {
        auto bad = encoded;
        bad[11][0] ^= 1;
        throws([&] { repair::recoverShards(bad, QVector<bool>(12, true)); });
    });
    suite.run("RS12-10/shard-count-and-size-rejected", "异常健壮性", [&] {
        auto bad = data;
        bad.removeLast();
        throws([&] { repair::encodeShards(bad); });
        bad = data;
        bad[0].chop(1);
        throws([&] { repair::encodeShards(bad); });
        throws([&] { repair::recoverShards(encoded, QVector<bool>(11, true)); });
    });
    suite.run("RS12-10/zero-length-shards-and-survivor-length", "异常健壮性", [&] {
        const auto empty = repair::encodeShards(QVector<QByteArray>(10));
        check(repair::recoverShards(empty, QVector<bool>(12, true)) == empty);
        auto bad = encoded;
        bad[11].chop(1);
        failsWith("REPAIR_SHARDS", [&] { repair::recoverShards(bad, QVector<bool>(12, true)); });
        bad.removeLast();
        failsWith("REPAIR_SHARDS", [&] { repair::recoverShards(bad, QVector<bool>(12, true)); });
    });

    QTemporaryDir temporary;
    bool ready = false;
    const QString archive = temporary.path() + "/original.cv", sidecar = temporary.path() + "/original.cvr";
    const QByteArray original = noise(2 * ChunkSize + 137);
    suite.run("Repair/create-real-sidecar", "集成", [&] {
        check(temporary.isValid());
        atomicWrite(archive, original);
        const auto result = repair::create(archive, sidecar);
        check(result.success && result.bytes == quint64(original.size()));
        const auto status = repair::check(archive, sidecar);
        check(status["healthy"].toBool() && status["recoverable"].toBool());
        ready = true;
    });
    auto clone = [&](const QString &prefix) {
        check(ready, "sidecar fixture was not created");
        const QString a = temporary.path() + "/" + prefix + ".cv";
        const QString p = temporary.path() + "/" + prefix + ".cvr";
        check(QFile::copy(archive, a));
        check(QFile::copy(sidecar, p));
        return QPair<QString, QString>(a, p);
    };
    suite.run("Repair/two-damaged-data-shards", "系统功能", [&] {
        auto pair = clone("two");
        flip(pair.first, 11);
        flip(pair.first, ChunkSize + 19);
        const auto status = repair::check(pair.first, pair.second);
        check(!status["healthy"].toBool() && status["recoverable"].toBool());
        const auto before = crypto::hashFile(pair.first);
        const QString output = temporary.path() + "/two-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
        check(crypto::hashFile(pair.first) == before);
    });
    suite.run("Repair/data-and-parity-damaged", "系统功能", [&] {
        auto pair = clone("mixed");
        flip(pair.first, 11);
        const auto bytes = readFile(pair.second);
        flip(pair.second, qint64(48 + big64(bytes, 8) + 10));
        const QString output = temporary.path() + "/mixed-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/both-parity-damaged", "系统功能", [&] {
        auto pair = clone("parity");
        const auto bytes = readFile(pair.second);
        const qint64 start = qint64(48 + big64(bytes, 8));
        flip(pair.second, start + 10);
        flip(pair.second, start + ChunkSize + 10);
        const QString output = temporary.path() + "/parity-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/three-damaged-reject-no-output", "异常健壮性", [&] {
        auto pair = clone("three");
        flip(pair.first, 3);
        flip(pair.first, ChunkSize + 3);
        flip(pair.first, 2 * ChunkSize + 3);
        check(!repair::check(pair.first, pair.second)["recoverable"].toBool());
        const QString output = temporary.path() + "/must-not-exist.cv";
        throws([&] { repair::recover(pair.first, pair.second, output); });
        check(!QFileInfo::exists(output));
    });
    suite.run("Repair/truncated-archive-tail", "异常健壮性", [&] {
        auto pair = clone("truncated");
        QFile file(pair.first);
        check(file.open(QIODevice::ReadWrite));
        check(file.resize(ChunkSize + 123));
        file.close();
        const QString output = temporary.path() + "/truncated-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/head-metadata-damaged-tail-used", "异常健壮性", [&] {
        auto pair = clone("head");
        flip(pair.second, 1);
        flip(pair.first, 12);
        const auto status = repair::check(pair.first, pair.second);
        check(!status["headMetadataValid"].toBool() && status["tailMetadataValid"].toBool());
        const QString output = temporary.path() + "/head-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/tail-metadata-damaged-head-used", "异常健壮性", [&] {
        auto pair = clone("tail");
        flip(pair.second, QFileInfo(pair.second).size() - 1);
        flip(pair.first, 12);
        const QString output = temporary.path() + "/tail-repaired.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/both-metadata-copies-damaged", "安全", [&] {
        auto pair = clone("indexes");
        flip(pair.second, 1);
        flip(pair.second, QFileInfo(pair.second).size() - 1);
        const QString output = temporary.path() + "/indexes-output.cv";
        throws([&] { repair::recover(pair.first, pair.second, output); });
        check(!QFileInfo::exists(output));
    });
    suite.run("Repair/valid-but-conflicting-metadata-copies", "安全", [&] {
        auto pair = clone("conflict");
        auto bytes = readFile(pair.second);
        const qsizetype size = qsizetype(big64(bytes, 8)), tailJson = bytes.size() - 48 - size;
        QByteArray json = bytes.mid(tailJson, size);
        const qsizetype hashOffset = json.indexOf("archiveSha256\":\"") + 16;
        check(hashOffset >= 16 && hashOffset < json.size());
        json[hashOffset] = json[hashOffset] == 'a' ? 'b' : 'a';
        bytes.replace(tailJson, size, json);
        bytes.replace(bytes.size() - 48, 32, crypto::sha256(json));
        atomicWrite(pair.second, bytes);
        throws([&] { repair::check(pair.first, pair.second); });
    });
    suite.run("Repair/appended-archive-rejected", "安全", [&] {
        auto pair = clone("appended");
        QFile file(pair.first);
        check(file.open(QIODevice::Append));
        check(file.write("x") == 1);
        file.close();
        throws([&] { repair::check(pair.first, pair.second); });
    });
    suite.run("Repair/truncated-sidecar-parity", "异常健壮性", [&] {
        auto pair = clone("short-parity");
        const qint64 start = qint64(48 + big64(readFile(pair.second), 8));
        QFile file(pair.second);
        check(file.open(QIODevice::ReadWrite));
        check(file.resize(start + 123));
        file.close();
        const QString output = temporary.path() + "/short-parity-output.cv";
        repair::recover(pair.first, pair.second, output);
        check(readFile(output) == original);
    });
    suite.run("Repair/existing-output-protected", "安全", [&] {
        check(ready);
        const QString output = temporary.path() + "/existing.cv";
        atomicWrite(output, "do not change");
        throws([&] { repair::recover(archive, sidecar, output); });
        check(readFile(output) == "do not change");
        throws([&] { repair::recover(archive, sidecar, archive); });
        check(readFile(archive) == original);
        throws([&] { repair::create(archive, sidecar); });
    });
    suite.run("Repair/cancel-without-output", "异常健壮性", [&] {
        check(ready);
        std::atomic_bool cancelled = true;
        Context context;
        context.cancelled = &cancelled;
        const QString output = temporary.path() + "/cancelled.cv";
        throws([&] { repair::recover(archive, sidecar, output, context); });
        check(!QFileInfo::exists(output));
    });
    suite.run("Repair/wrong-sidecar-rejected", "安全", [&] {
        auto pair = clone("wrong");
        atomicWrite(pair.first, QByteArray(original.size(), 'W'));
        const QString output = temporary.path() + "/wrong-output.cv";
        throws([&] { repair::recover(pair.first, pair.second, output); });
        check(!QFileInfo::exists(output));
    });
    suite.run("Repair/empty-archive", "集成", [&] {
        const QString a = temporary.path() + "/empty.cv", p = a + ".cvr", out = a + ".fixed";
        atomicWrite(a, "");
        repair::create(a, p);
        check(repair::check(a, p)["healthy"].toBool());
        repair::recover(a, p, out);
        check(readFile(out).isEmpty());
    });
    suite.run("Repair/encrypted-archive-still-needs-password", "集成", [&] {
        const QByteArray key(32, 'k'), plain = QByteArray("private content").repeated(1000);
        const auto encrypted = crypto::seal(compression::encode(plain, "lz77"), key, "chacha20");
        const QString a = temporary.path() + "/secret.cv", p = a + ".cvr", out = a + ".fixed";
        atomicWrite(a, encrypted);
        repair::create(a, p);
        flip(a, 110);
        repair::recover(a, p, out);
        check(readFile(out) == encrypted);
        throws([&] { crypto::open(readFile(out), QByteArray(32, 'x')); });
        check(compression::decode(crypto::open(readFile(out), key)) == plain);
    });
    suite.run("Repair/two-stripes-independent-erasure-budgets", "集成", [&] {
        const auto bytes = noise(10 * ChunkSize + 17);
        const auto a = temporary.filePath("two-stripes.cv"), p = a + ".cvr", out = a + ".fixed";
        atomicWrite(a, bytes);
        repair::create(a, p);
        const auto parityOffset = qint64(48 + big64(readFile(p), 8));
        flip(a, 3);
        flip(a, ChunkSize + 3);
        flip(a, 10 * ChunkSize + 3);
        flip(p, parityOffset + 2 * ChunkSize + 3);
        const auto status = repair::check(a, p);
        check(status["recoverable"].toBool() && status["damagedShards"] == "4");
        check(status["damagedGroups"].toArray().size() == 2);
        const auto result = repair::recover(a, p, out);
        check(result.details["repairedShards"] == "4" && readFile(out) == bytes);
    });
    for (const QString field :
         {QString("version"), QString("shardSize"), QString("dataShards"), QString("parityShards")}) {
        suite.run("Repair/unsupported-index-" + field, "安全", [&, field] {
            auto metadata = emptySidecarMetadata();
            metadata[field] = metadata[field].toInt() + 1;
            const auto p = temporary.filePath("index-" + field + ".cvr");
            atomicWrite(p, sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
            failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        });
    }
    struct MetadataFailure {
        const char *name;
        QString field;
        QJsonValue value;
    };
    for (const auto &item :
         std::array<MetadataFailure, 11>{{{"negative-size", "originalSize", "-1"},
                                          {"leading-zero-size", "originalSize", "01"},
                                          {"signed-overflow-size", "originalSize", "9223372036854775808"},
                                          {"nondecimal-size", "originalSize", "1.5"},
                                          {"numeric-size", "originalSize", 0},
                                          {"index-capacity", "originalSize", "1048576000001"},
                                          {"groups-not-array", "groups", QJsonObject{}},
                                          {"extra-group", "groups", QJsonArray{QJsonArray{}}},
                                          {"short-hash", "archiveSha256", "ab"},
                                          {"nonhex-hash", "archiveSha256", QString(64, 'g')},
                                          {"uppercase-hash", "archiveSha256", QString(64, 'A')}}}) {
        suite.run("Repair/index-" + QString(item.name), "安全", [&, item] {
            auto metadata = emptySidecarMetadata();
            metadata[item.field] = item.value;
            const auto p = temporary.filePath(QString(item.name) + ".cvr");
            atomicWrite(p, sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
            failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        });
    }
    for (const QByteArray json : {QByteArray("{"), QByteArray("[]")}) {
        suite.run("Repair/checksummed-index-" +
                      (json == "{" ? QString("malformed-json") : QString("nonobject-json")),
                  "安全", [&, json] {
                      const auto p = temporary.filePath("malformed-index.cvr");
                      atomicWrite(p, sidecarFixture(json));
                      failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
                  });
    }
    suite.run("Repair/index-shard-row-and-digest-validation", "安全", [&] {
        auto metadata = emptySidecarMetadata();
        metadata["originalSize"] = "1";
        const auto p = temporary.filePath("invalid-shard-row.cvr");
        metadata["groups"] = QJsonArray{QJsonArray{}};
        atomicWrite(p, sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
        failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        QJsonArray row;
        for (int i = 0; i < 12; ++i)
            row.append(QString(64, '0'));
        row[11] = "wrong";
        metadata["groups"] = QJsonArray{row};
        atomicWrite(p, sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
        failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
    });
    suite.run("Repair/locator-invalid-lengths-and-checksum", "安全", [&] {
        const auto valid =
            sidecarFixture(QJsonDocument(emptySidecarMetadata()).toJson(QJsonDocument::Compact));
        const auto p = temporary.filePath("locator.cvr");
        for (quint64 length : {quint64(0), quint64(128 * 1024 * 1024 + 1), quint64(valid.size())}) {
            auto damaged = valid;
            store64(damaged, 8, length);
            store64(damaged, damaged.size() - 16, length);
            atomicWrite(p, damaged);
            failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        }
        auto damaged = valid;
        damaged[16] ^= 1;
        damaged[damaged.size() - 48] ^= 1;
        atomicWrite(p, damaged);
        failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        atomicWrite(p, valid.first(47));
        failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
        atomicWrite(p, valid + 'x');
        failsWith("REPAIR_METADATA", [&] { repair::check(archive, p); });
    });
    suite.run("Repair/whole-digest-inconsistent-with-valid-shards", "安全", [&] {
        auto pair = clone("whole-hash");
        const auto bytes = readFile(pair.second);
        const auto length = qsizetype(big64(bytes, 8));
        auto metadata = QJsonDocument::fromJson(bytes.mid(48, length)).object();
        metadata["archiveSha256"] = QString(64, '0');
        const auto parity = bytes.mid(48 + length, bytes.size() - 96 - 2 * length);
        atomicWrite(pair.second,
                    sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact), parity));
        const auto status = repair::check(pair.first, pair.second);
        check(!status["healthy"].toBool() && !status["recoverable"].toBool() &&
              status["damagedShards"] == "0");
        const auto output = temporary.filePath("whole-hash-output.cv");
        failsWith("REPAIR_CHECKSUM", [&] { repair::recover(pair.first, pair.second, output); });
        check(!QFileInfo::exists(output) && readFile(pair.first) == original);
    });
    suite.run("Repair/reconstructed-shard-hash-must-match-index", "安全", [&] {
        auto pair = clone("shard-hash");
        const auto bytes = readFile(pair.second);
        const auto length = qsizetype(big64(bytes, 8));
        auto metadata = QJsonDocument::fromJson(bytes.mid(48, length)).object();
        auto groups = metadata["groups"].toArray();
        auto row = groups[0].toArray();
        row[0] = QString(64, '0');
        groups[0] = row;
        metadata["groups"] = groups;
        atomicWrite(pair.second, sidecarFixture(QJsonDocument(metadata).toJson(QJsonDocument::Compact),
                                                bytes.mid(48 + length, bytes.size() - 96 - 2 * length)));
        const auto output = temporary.filePath("shard-hash-output.cv");
        failsWith("REPAIR_CHECKSUM", [&] { repair::recover(pair.first, pair.second, output); });
        check(!QFileInfo::exists(output) && readFile(pair.first) == original);
    });
    suite.run("Repair/missing-inputs-and-invalid-output-parent", "异常健壮性", [&] {
        const auto missing = temporary.filePath("missing.cv"), output = temporary.filePath("read-failed.cv");
        failsWith("READ", [&] { repair::create(missing, output); });
        failsWith("READ", [&] { repair::check(missing, sidecar); });
        failsWith("READ", [&] { repair::check(archive, missing); });
        failsWith("READ", [&] { repair::recover(missing, sidecar, output); });
        failsWith("READ", [&] { repair::recover(archive, missing, output); });
        failsWith("PATH", [&] { repair::recover(archive, sidecar, temporary.filePath("absent/output.cv")); });
        // Equal missing paths must fail before creating an output or opening
        // either input; normalized path protection does not require existence.
        failsWith("PATH", [&] { repair::recover(missing, sidecar, missing); });
        failsWith("PATH", [&] { repair::recover(archive, missing, missing); });
        check(!QFileInfo::exists(output));
    });
    for (bool timestampOnly : {false, true}) {
        suite.run(QString("Repair/source-changed-") + (timestampOnly ? "timestamp" : "length"), "并发稳定性",
                  [&, timestampOnly] {
                      auto pair = clone(timestampOnly ? "modified-time" : "modified-length");
                      const auto output = pair.first + ".new.cvr";
                      Context context;
                      context.progress = [&](quint64, quint64, const QString &) {
                          QFile writer(pair.first);
                          check(writer.open(QIODevice::ReadWrite));
                          if (timestampOnly)
                              check(writer.setFileTime(QFileInfo(pair.first).lastModified().addSecs(60),
                                                       QFileDevice::FileModificationTime));
                          else {
                              check(writer.seek(writer.size()));
                              check(writer.write("x") == 1 && writer.flush());
                          }
                      };
                      failsWith("SOURCE_CHANGED", [&] { repair::create(pair.first, output, context); });
                      check(!QFileInfo::exists(output));
                  });
    }
    for (bool creating : {false, true}) {
        suite.run(QString("Repair/cancel-after-work-") + (creating ? "create" : "recover"), "异常健壮性",
                  [&, creating] {
                      const auto output = temporary.filePath(creating ? "cancel-late.cvr" : "cancel-late.cv");
                      std::atomic_bool cancelled = false;
                      Context context;
                      context.cancelled = &cancelled;
                      context.progress = [&](quint64 done, quint64, const QString &) {
                          check(done > 0);
                          cancelled = true;
                      };
                      failsWith("CANCELLED", [&] {
                          if (creating)
                              repair::create(archive, output, context);
                          else
                              repair::recover(archive, sidecar, output, context);
                      });
                      check(!QFileInfo::exists(output) && readFile(archive) == original);
                  });
        suite.run(
            QString("Repair/concurrent-output-preserved-") + (creating ? "create" : "recover"), "并发稳定性",
            [&, creating] {
                const auto output = temporary.filePath(creating ? "racing-output.cvr" : "racing-output.cv");
                Context context;
                context.progress = [&](quint64, quint64, const QString &) {
                    atomicWrite(output, "another writer committed first");
                };
                failsWith("EXISTS", [&] {
                    if (creating)
                        repair::create(archive, output, context);
                    else
                        repair::recover(archive, sidecar, output, context);
                });
                check(readFile(output) == "another writer committed first" && readFile(archive) == original);
            });
    }
    suite.run("HashFile/stream-and-cancel", "集成", [&] {
        check(ready);
        check(crypto::hashFile(archive) == crypto::sha256(original));
        std::atomic_bool cancelled = true;
        Context context;
        context.cancelled = &cancelled;
        throws([&] { crypto::hashFile(archive, context); });
    });
    suite.run("HashFile/missing-file-and-cancel-after-read", "异常健壮性", [&] {
        failsWith("READ", [&] { crypto::hashFile(temporary.filePath("missing-hash.cv")); });
        std::atomic_bool cancelled = false;
        Context context;
        context.cancelled = &cancelled;
        context.progress = [&](quint64 done, quint64 total, const QString &) {
            check(done > 0 && done <= total);
            cancelled = true;
        };
        failsWith("CANCELLED", [&] { crypto::hashFile(archive, context); });
    });
    suite.run("Common/bounded-read-exact-and-over-limit", "安全", [&] {
        const auto path = temporary.path() + "/bounded.txt";
        atomicWrite(path, "bounded");
        check(readFile(path, 7) == "bounded");
        throws([&] { readFile(path, 6); });
        throws([&] { readFile(path, -1); });
        throws([&] { readFile(path, std::numeric_limits<qint64>::max()); });
    });
    suite.run("Common/empty-file-zero-bound", "异常健壮性", [&] {
        const auto path = temporary.path() + "/zero.txt";
        atomicWrite(path, "");
        check(readFile(path, 0).isEmpty());
        throws([&] { readFile(path + ".missing"); });
    });
    suite.run("Common/json-object-required", "安全", [&] {
        const auto path = temporary.path() + "/metadata.json";
        for (const QByteArray malformed : {QByteArray("[]"), QByteArray("null"), QByteArray("{")}) {
            atomicWrite(path, malformed);
            throws([&] { readJson(path); });
        }
        writeJson(path, {{"accepted", true}});
        check(readJson(path)["accepted"].toBool());
    });
    suite.run("Common/plan-defaults-and-password-serialization", "安全", [] {
        const auto defaults = BackupPlan::fromJson({});
        check(defaults.sources.isEmpty() && defaults.destination.isEmpty() && defaults.format == "indexed" &&
              defaults.compression == "lz77" && defaults.cipher == "none" && defaults.strictMetadata);
        BackupPlan plan;
        plan.sources = {"D:/资料", "D:/source"};
        plan.destination = "D:/backup";
        plan.format = "tar";
        plan.compression = "huffman";
        plan.cipher = "aes256";
        plan.password = "explicit protected transport only";
        plan.filters = {{"minSize", 17}};
        plan.strictMetadata = false;
        check(!plan.toJson().contains("password"));
        const auto withSecret = plan.toJson(true);
        check(withSecret["password"] == plan.password);
        check(BackupPlan::fromJson(withSecret).toJson(true) == withSecret);
        check(BackupPlan::fromJson(plan.toJson()).password.isEmpty());
    });
    suite.run("Common/atomic-write-parent-blocked-keeps-original", "异常健壮性", [&] {
        const auto parent = temporary.filePath("blocking-file");
        atomicWrite(parent, "keep parent bytes");
        failsWith("MKDIR_FAILED", [&] { atomicWrite(parent + "/child", "cannot commit"); });
        check(readFile(parent) == "keep parent bytes");
        const auto directory = temporary.filePath("directory-target");
        check(QDir().mkdir(directory));
        failsWith("WRITE_FAILED", [&] { atomicWrite(directory, "cannot replace directory"); });
        check(QFileInfo(directory).isDir());
    });
#ifdef _WIN32
    suite.run("Common/atomic-commit-sharing-denial-keeps-original", "异常健壮性", [&] {
        const auto path = temporary.filePath("locked-destination.txt");
        atomicWrite(path, "original survives failed replacement");
        {
            const DenyDeleteLock lock(path);
            failsWith("COMMIT_FAILED", [&] { atomicWrite(path, "replacement bytes"); });
            check(readFile(path) == "original survives failed replacement");
        }
        atomicWrite(path, "replacement after unlock");
        check(readFile(path) == "replacement after unlock");
    });
    suite.run("Repair/invalid-Windows-output-name-cleans-up", "异常健壮性", [&] {
        const auto output = temporary.filePath("invalid*name.cv");
        failsWith("WRITE", [&] { repair::recover(archive, sidecar, output); });
        failsWith("WRITE", [&] { repair::create(archive, output); });
        check(readFile(archive) == original);
        check(
            QDir(temporary.path()).entryList({".chengvault-parity-*"}, QDir::Files | QDir::Hidden).isEmpty());
    });
#endif
    suite.run("Common/app-data-directory-invalid-override", "异常健壮性", [&] {
        // Restore process configuration even if the assertion throws, because
        // other modules may execute later in the same test process.
        struct RestoreEnvironment {
            bool existed = qEnvironmentVariableIsSet("CHENGVAULT_DATA_HOME");
            QByteArray previous = qgetenv("CHENGVAULT_DATA_HOME");
            ~RestoreEnvironment() {
                if (existed)
                    qputenv("CHENGVAULT_DATA_HOME", previous);
                else
                    qunsetenv("CHENGVAULT_DATA_HOME");
            }
        } restore;
        const auto path = temporary.filePath("profile-as-file");
        atomicWrite(path, "must survive");
        qputenv("CHENGVAULT_DATA_HOME", path.toUtf8());
        failsWith("MKDIR_FAILED", [] { appDataDirectory(); });
        check(readFile(path) == "must survive");
        const auto directory = temporary.filePath("isolated-profile");
        qputenv("CHENGVAULT_DATA_HOME", directory.toUtf8());
        check(appDataDirectory() == directory && QFileInfo(directory).isDir());
    });
}
