#include "chengvault/archive.hpp"
#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <cstring>
#include <limits>

namespace cv::archive {
namespace {
// Archive architecture separates two independent concerns:
// 1. Inner encoding: indexed content-addressed layout or interoperable PAX/TAR.
// 2. Outer framing: bounded compression, optional encryption, and integrity.
// The outer layer encrypts metadata and filenames as well as file bytes.
// A random master key is wrapped once under the password-derived key.
// Each record binds a sequence number and the exact serialized header hash.
// The final record binds total uncompressed length and whole-stream digest.
// Plain archives use checksums, which are not claimed to resist active forgery.
// Intermediate files keep RAM bounded but require temporary disk capacity.
// An archive is committed only after every transform and final record succeeds.
// Restore fully validates the outer stream before creating destination objects.
// Limits below are format-reader safety budgets, not filesystem size limits.
// Large files are supported through bounded blocks and 64-bit aggregate lengths.
// Unknown algorithm IDs are errors rather than fallback to weaker transforms.
// The writer is independent from external TAR, compression or password tools.
// Tests may use an external TAR reader only as an interoperability oracle.
// EOF is part of the format: trailing bytes after a final record are corruption.
// Invalid compressed lengths are rejected before output buffers are allocated.
// User paths never become temporary spool filenames; only SHA-256 IDs do.
// A private temporary directory owns all intermediate files for automatic cleanup.
// Unencrypted checksums detect accidents but do not hide file information.
// An encrypted header intentionally reveals algorithms and bounded KDF settings.
// Algorithm choice and storage byte counts are returned for measured reporting.
constexpr qint64 ManifestLimit = 64LL * 1024 * 1024;
constexpr quint32 EnvelopeLimit = 4 * 1024 * 1024;
const QByteArray OuterMagic("CVARCH2\n", 8), IndexMagic("CVIDX01\n", 8);
struct Location {
    qint64 offset = 0;
    qint64 length = 0;
};
struct Parsed {
    QJsonObject manifest;
    QMap<QString, Location> locations;
};
void write(QIODevice &device, const QByteArray &data) {
    require(device.write(data) == data.size(), "ARCHIVE_WRITE", device.errorString());
}
// All variable-length reads have an explicit cap before Qt allocates memory.
// A short read is always corruption/truncation; it is never silently padded.
// File payloads larger than this bound are streamed by callers, not read whole.
// Bounded decoders validate lengths before they become allocation sizes.
// A short read is truncation even without an operating-system I/O error.
QByteArray read(QIODevice &device, qint64 size) {
    require(size >= 0 && size <= ManifestLimit, "ARCHIVE_LENGTH", "归档字段长度越界");
    const auto bytes = device.read(size);
    require(bytes.size() == size, "ARCHIVE_TRUNCATED", "归档被截断");
    return bytes;
}
// Project framing is byte-defined and independent of compiler ABI or padding.
// TAR fields use their own ASCII/octal representation in separate helpers.
// Explicit bytes avoid host endianness, structure padding and compiler ABI.
// The versioned format remains stable across compatible builds.
QByteArray le(quint64 value, int size) {
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; ++i)
        bytes[i] = char(value >> (8 * i));
    return bytes;
}
quint64 number(const QByteArray &bytes, int offset, int size) {
    require(offset >= 0 && size > 0 && size <= 8 && offset + size <= bytes.size(), "ARCHIVE_INTEGER",
            "归档整数越界");
    quint64 result = 0;
    for (int i = 0; i < size; ++i)
        result |= quint64(quint8(bytes[offset + i])) << (i * 8);
    return result;
}
// A syntactically valid JSON scalar/array is not a valid manifest or header.
// Report parser errors before any path or length from the object is trusted.
// A JSON scalar or array cannot serve as the manifest/header root.
// Parsing must succeed before any schema-dependent access occurs.
QJsonObject object(const QByteArray &bytes) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "ARCHIVE_JSON", "归档 JSON 损坏");
    return doc.object();
}
QByteArray json(const QJsonObject &value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}
QString canonicalCipher(QString cipher) {
    cipher = cipher.toLower();
    if (cipher == "aes" || cipher == "aes-256" || cipher == "aes256-ctr")
        cipher = "aes256";
    require(cipher == "none" || cipher == "aes256" || cipher == "chacha20", "CIPHER_UNSUPPORTED", cipher);
    return cipher;
}
// Repository and filesystem contracts enumerate ADS through the same streams
// array as ordinary data. This helper prevents named streams being accidentally
// omitted from deduplication, verification, or standalone archive materialization.
// Use the same walker for indexing and verification, including directory ADS.
// A file-only scan would silently lose named data attached to directories.
QList<QJsonObject> streams(const QJsonObject &manifest) {
    QList<QJsonObject> result;
    for (const auto &ev : manifest["entries"].toArray())
        for (const auto &sv : ev.toObject()["streams"].toArray())
            result.append(sv.toObject());
    return result;
}
// Equal content references share an indexed payload regardless of pathname.
// Logical order stays in the manifest rather than this unordered set.
QSet<QString> chunkIds(const QJsonObject &manifest) {
    QSet<QString> result;
    for (const auto &stream : streams(manifest))
        for (const auto &chunk : stream["chunks"].toArray())
            result.insert(chunk.toObject()["id"].toString());
    return result;
}
// Indexed archives store each unique plaintext content block once, even when
// several file/stream offsets reference it. Deterministic sorted IDs simplify
// format inspection; encrypted output remains randomized by the outer envelope.
// File offsets are built by the reader, so all sizes remain explicit on disk.
void writeIndex(const QString &target, const QJsonObject &manifest, const QString &spool,
                const Context &context) {
    QFile file(target);
    require(file.open(QIODevice::WriteOnly), "ARCHIVE_CREATE", file.errorString());
    write(file, IndexMagic);
    const auto meta = json(manifest);
    require(meta.size() <= ManifestLimit, "MANIFEST_SIZE", "清单超过 64 MiB 上限");
    write(file, le(quint64(meta.size()), 8));
    write(file, meta);
    auto ids = chunkIds(manifest).values();
    std::sort(ids.begin(), ids.end());
    write(file, le(quint64(ids.size()), 8));
    for (const auto &id : ids) {
        context.check();
        const auto data = readFile(spool + '/' + id, ChunkSize);
        write(file, id.toLatin1());
        write(file, le(quint64(data.size()), 4));
        write(file, data);
    }
    require(file.flush(), "ARCHIVE_FLUSH", file.errorString());
}
// Parse once to build seek locations. Do not read all raw blocks into RAM.
// Reject duplicate IDs, out-of-range lengths, missing references and trailing
// bytes: a successful parse must describe exactly the advertised block set.
// Chunk content hashes are checked when fetched by verify or restore.
Parsed parseIndex(QFile &file, const Context &context) {
    require(read(file, 8) == IndexMagic, "INDEX_MAGIC", "索引归档标识错误");
    const auto length = number(read(file, 8), 0, 8);
    require(length <= quint64(ManifestLimit), "MANIFEST_SIZE", "清单长度越界");
    Parsed result;
    result.manifest = object(read(file, qint64(length)));
    fs::validateManifest(result.manifest);
    const auto count = number(read(file, 8), 0, 8);
    require(count <= 10000000, "CHUNK_COUNT", "数据块数量越界");
    for (quint64 i = 0; i < count; ++i) {
        context.check();
        const auto id = QString::fromLatin1(read(file, 64));
        const auto size = number(read(file, 4), 0, 4);
        require(size > 0 && size <= quint64(ChunkSize) && !result.locations.contains(id) &&
                    QRegularExpression("^[0-9a-f]{64}$").match(id).hasMatch() &&
                    qint64(size) <= file.size() - file.pos(),
                "INDEX_INVALID", "索引数据块损坏");
        result.locations[id] = {file.pos(), qint64(size)};
        require(file.seek(file.pos() + qint64(size)), "ARCHIVE_SEEK", file.errorString());
    }
    require(file.atEnd(), "INDEX_TRAILING", "索引归档含多余尾部数据");
    const auto expected = chunkIds(result.manifest);
    require(expected.size() == result.locations.size(), "INDEX_MISMATCH", "数据块索引与清单不一致");
    for (const auto &id : expected)
        require(result.locations.contains(id), "CHUNK_MISSING", id);
    return result;
}
// A POSIX PAX record's decimal length includes its own digits and newline.
// Iterate to a fixed point because crossing a power of ten increases the
// length field itself. Count UTF-8 bytes, never QString character positions.
// The PAX length includes its own decimal digits and trailing newline.
// Adding a decimal digit can grow that length again; iterate to a fixed point.
// Writer-controlled keys and UTF-8 values have separate framing roles.
QByteArray paxLine(const QByteArray &key, const QByteArray &value) {
    const auto body = key + '=' + value + '\n';
    qsizetype length = body.size() + 2;
    while (true) {
        const auto next = body.size() + 1 + QByteArray::number(length).size();
        if (next == length)
            break;
        length = next;
    }
    return QByteArray::number(length) + ' ' + body;
}
// Reserve the final byte for the USTAR field terminator.
// PAX carries large sizes instead of silently truncating octal values.
void octal(QByteArray &header, int offset, int length, quint64 value) {
    const auto digits = QByteArray::number(value, 8);
    require(digits.size() < length, "TAR_SIZE", "TAR 数值字段超过 USTAR/PAX 容量");
    std::memset(header.data() + offset, '0', size_t(length - 1));
    std::memcpy(header.data() + offset + length - 1 - digits.size(), digits.constData(),
                size_t(digits.size()));
    header[offset + length - 1] = '\0';
}
// USTAR's header checksum treats its checksum field as eight ASCII spaces.
// Numeric fields are NUL-terminated octal, with PAX carrying oversized lengths.
// The placeholder pathname is intentionally superseded by the PAX path record.
// Actual link type flags remain standard TAR flags for independent readers.
// Standard headers let independent tar tools inspect the decoded artifact.
// The checksum field contains spaces while computing the header checksum.
// PAX carries long names; this header uses a portable short placeholder.
void tarHeader(QFile &file, const QByteArray &name, char type, quint64 size, const QByteArray &link = {}) {
    QByteArray header(512, '\0');
    std::memcpy(header.data(), name.constData(), size_t(std::min<qsizetype>(name.size(), 100)));
    octal(header, 100, 8, type == '5' ? 0700 : 0600);
    octal(header, 108, 8, 0);
    octal(header, 116, 8, 0);
    // PAX size carries lengths beyond the 11 octal digit USTAR field.
    octal(header, 124, 12, size <= 077777777777ULL ? size : 0);
    octal(header, 136, 12, 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = type;
    std::memcpy(header.data() + 157, link.constData(), size_t(std::min<qsizetype>(link.size(), 100)));
    std::memcpy(header.data() + 257, "ustar\00000", 8);
    quint64 sum = 0;
    for (const auto byte : header)
        sum += quint8(byte);
    const auto checksum = QByteArray::number(sum, 8).rightJustified(6, '0');
    std::memcpy(header.data() + 148, checksum.constData(), 6);
    header[154] = '\0';
    header[155] = ' ';
    write(file, header);
}
// Payload framing aligns each following TAR header on a 512-byte boundary.
// Padding is excluded from the logical member length.
void padding(QFile &file, qint64 size) {
    if (size % 512)
        write(file, QByteArray(512 - size % 512, '\0'));
}
// Each extension header applies only to the immediately following member.
// Path and linkpath values bypass the narrow fixed-width USTAR fields.
void paxHeader(QFile &file, const QString &path, char type, qint64 size, const QString &link,
               const QMap<QByteArray, QByteArray> &extra = {}) {
    // Per-member extended headers avoid a global PAX state that could leak
    // attributes into later unrelated entries. Vendor keys identify our NTFS
    // metadata members without reserving a source root name from the user.
    QByteArray pax = paxLine("path", path.toUtf8()) + paxLine("size", QByteArray::number(size));
    if (!link.isEmpty())
        pax += paxLine("linkpath", link.toUtf8());
    for (auto it = extra.begin(); it != extra.end(); ++it)
        pax += paxLine(it.key(), it.value());
    tarHeader(file, "PaxHeaders/ChengVault", 'x', quint64(pax.size()));
    write(file, pax);
    padding(file, pax.size());
    tarHeader(file, "ChengVaultEntry", type, quint64(size));
}
// A PAX member contains logical file bytes; sparse holes expand to zeros.
// The NTFS manifest separately retains allocation ranges for sparse restore.
// Zero gaps are emitted in bounded pieces instead of whole-file buffers.
void copyStream(QFile &file, const QJsonObject &stream, const QString &spool, const Context &context) {
    // TAR ordinary-file bodies contain actual logical file bytes, not chunk IDs.
    // Reconstruct sparse gaps as zeros; indexed archives are more space-efficient
    // before compression for huge sparse streams. Padding is outside file size.
    // This loop never materializes the entire stream in a QByteArray.
    qint64 position = 0;
    const QByteArray zero(ChunkSize, '\0');
    auto zeros = [&](qint64 length) {
        while (length > 0) {
            context.check();
            const auto n = std::min<qint64>(length, zero.size());
            write(file, zero.left(n));
            length -= n;
        }
    };
    for (const auto &cv : stream["chunks"].toArray()) {
        context.check();
        const auto chunk = cv.toObject();
        const auto off = chunk["offset"].toInteger();
        zeros(off - position);
        const auto data = readFile(spool + '/' + chunk["id"].toString(), ChunkSize);
        require(data.size() == chunk["length"].toInteger(), "CHUNK_SIZE", "数据块长度不一致");
        write(file, data);
        position = off + data.size();
    }
    zeros(stream["size"].toInteger() - position);
    padding(file, stream["size"].toInteger());
}
void writeTar(const QString &target, const QJsonObject &manifest, const QString &spool,
              const Context &context) {
    // Metadata is first so the parser can validate each subsequent member's path,
    // type, length and stream identity as it arrives. NTFS streams use regular
    // TAR members with explicit vendor attributes pointing to their owning path.
    // Empty ordinary files still get a standard zero-length member.
    // Hardlinks refer to the canonical data pathname recorded by the scanner.
    // Two full zero headers terminate the archive; no ambiguous extra members.
    QFile file(target);
    require(file.open(QIODevice::WriteOnly), "ARCHIVE_CREATE", file.errorString());
    const auto meta = json(manifest);
    require(meta.size() <= ManifestLimit, "MANIFEST_SIZE", "清单超过上限");
    // This is an actual POSIX PAX/TAR directory/file/link archive. Its metadata
    // member is identified by a PAX vendor key, avoiding source-name collisions.
    paxHeader(file, "__chengvault_metadata__/manifest.json", '0', meta.size(), {},
              {{"CHENGVAULT.kind", "manifest"}});
    write(file, meta);
    padding(file, meta.size());
    int streamIndex = 0;
    for (const auto &ev : manifest["entries"].toArray()) {
        context.check();
        const auto entry = ev.toObject();
        const auto type = entry["type"].toString();
        const auto path = entry["path"].toString();
        if (type == "directory")
            paxHeader(file, path, '5', 0, {});
        else if (type == "hardlink")
            paxHeader(file, path, '1', 0, entry["target"].toString());
        else if (type == "symlink" || type == "junction")
            paxHeader(file, path, '2', 0, entry["target"].toString());
        for (const auto &sv : entry["streams"].toArray()) {
            const auto stream = sv.toObject();
            const auto name = stream["name"].toString();
            if (name.isEmpty())
                paxHeader(file, path, '0', stream["size"].toInteger(), {});
            else
                paxHeader(file, "__chengvault_metadata__/streams/" + QString::number(streamIndex++), '0',
                          stream["size"].toInteger(), {},
                          {{"CHENGVAULT.kind", "stream"},
                           {"CHENGVAULT.path", path.toUtf8()},
                           {"CHENGVAULT.stream", name.toUtf8()}});
            copyStream(file, stream, spool, context);
        }
    }
    write(file, QByteArray(1024, '\0'));
    require(file.flush(), "ARCHIVE_FLUSH", file.errorString());
}
// Accept standard surrounding spaces and NULs, rejecting non-octal bytes.
// Historical base-256 fields are outside this writer-defined TAR dialect.
quint64 tarOctal(const QByteArray &field) {
    auto text = field;
    text.replace('\0', ' ');
    text = text.trimmed();
    if (text.isEmpty())
        return 0;
    bool ok = false;
    const auto value = text.toULongLong(&ok, 8);
    require(ok, "TAR_NUMBER", "TAR 八进制字段无效");
    return value;
}
// PAX lengths delimit values even when a path contains embedded line breaks.
// Reject duplicate keys rather than silently adopting attacker-controlled last
// values. Bounds are checked before slicing each record's key/value bytes.
// Length prefixes locate values even when filenames contain newlines.
// Duplicate attributes are rejected to avoid ambiguous precedence rules.
QMap<QByteArray, QByteArray> parsePax(const QByteArray &payload) {
    QMap<QByteArray, QByteArray> result;
    qsizetype offset = 0;
    while (offset < payload.size()) {
        const auto space = payload.indexOf(' ', offset);
        require(space > offset && space - offset < 12, "PAX_LENGTH", "PAX 记录长度损坏");
        bool ok = false;
        const auto length = payload.mid(offset, space - offset).toLongLong(&ok);
        require(ok && length > space - offset + 2 && length <= payload.size() - offset &&
                    payload[offset + length - 1] == '\n',
                "PAX_LENGTH", "PAX 记录越界");
        const auto body = payload.mid(space + 1, offset + length - space - 2);
        const auto equals = body.indexOf('=');
        require(equals > 0, "PAX_KEY", "PAX 记录缺少键值");
        const auto key = body.left(equals);
        require(!result.contains(key), "PAX_DUPLICATE", "PAX 重复键");
        result[key] = body.mid(equals + 1);
        offset += length;
    }
    return result;
}
Parsed parseTar(QFile &file, const Context &context) {
    // Trust neither TAR headers nor the manifest independently: cross-check both.
    // The manifest provides logical chunk offsets within each file member, which
    // become seek offsets into the decoded TAR. ADS use exactly the same logic.
    // Symbolic/hard link targets must agree with the captured NTFS manifest.
    // Every required member must appear exactly once, including empty directories.
    // External arbitrary TAR import is deliberately unsupported; this reader
    // accepts ChengVault's PAX extension contract, not every historical TAR dialect.
    Parsed result;
    QMap<QByteArray, QByteArray> pax;
    QMap<QString, QJsonObject> entries;
    QSet<QString> observed;
    int zeroBlocks = 0;
    while (!file.atEnd()) {
        context.check();
        auto header = read(file, 512);
        if (header == QByteArray(512, '\0')) {
            if (++zeroBlocks == 2)
                break;
            continue;
        }
        require(zeroBlocks == 0, "TAR_TERMINATOR", "TAR 结束块损坏");
        const auto checksum = tarOctal(header.mid(148, 8));
        std::memset(header.data() + 148, ' ', 8);
        quint64 sum = 0;
        for (char ch : header)
            sum += quint8(ch);
        require(sum == checksum && header.mid(257, 5) == "ustar", "TAR_CHECKSUM", "TAR 头校验失败");
        quint64 size = tarOctal(header.mid(124, 12));
        const char type = header[156];
        if (type == 'x') {
            require(pax.isEmpty() && size <= quint64(ManifestLimit), "PAX_SIZE", "PAX 扩展头越界");
            pax = parsePax(read(file, qint64(size)));
            const auto pad = (512 - qint64(size) % 512) % 512;
            // Extended-header padding is part of our canonical TAR encoding,
            // just like ordinary member padding. An outer checksum can be
            // recomputed for an unencrypted file; validate these inner bytes too.
            require(read(file, pad) == QByteArray(pad, '\0'), "TAR_PADDING", "PAX 对齐填充损坏");
            continue;
        }
        require(pax.contains("path") && pax.contains("size"), "PAX_MISSING", "缺少 ChengVault PAX 属性");
        bool sizeOk = false;
        size = pax["size"].toULongLong(&sizeOk);
        require(sizeOk && size <= quint64(std::numeric_limits<qint64>::max()) &&
                    qint64(size) <= file.size() - file.pos(),
                "TAR_SIZE", "TAR 内容长度越界");
        const auto path = QString::fromUtf8(pax["path"]);
        const auto kind = pax.value("CHENGVAULT.kind");
        if (kind == "manifest") {
            require(result.manifest.isEmpty() && size <= quint64(ManifestLimit) && type == '0',
                    "TAR_MANIFEST", "清单重复或损坏");
            result.manifest = object(read(file, qint64(size)));
            fs::validateManifest(result.manifest);
            for (const auto &ev : result.manifest["entries"].toArray())
                entries[ev.toObject()["path"].toString()] = ev.toObject();
        } else {
            require(!result.manifest.isEmpty(), "TAR_MANIFEST", "清单必须为首个成员");
            const auto actualPath = kind == "stream" ? QString::fromUtf8(pax["CHENGVAULT.path"]) : path;
            const auto streamName =
                kind == "stream" ? QString::fromUtf8(pax["CHENGVAULT.stream"]) : QString{};
            require(entries.contains(actualPath), "TAR_UNEXPECTED", "TAR 含未登记路径");
            const auto e = entries[actualPath];
            const auto entryType = e["type"].toString();
            const auto token = actualPath + QChar(0) + streamName;
            require(!observed.contains(token), "TAR_DUPLICATE", "TAR 重复成员");
            observed.insert(token);
            if (kind != "stream" && entryType != "file") {
                const char expected = entryType == "directory" ? '5' : entryType == "hardlink" ? '1' : '2';
                require(type == expected && size == 0, "TAR_TYPE", "TAR 对象类型与清单不符");
                if (type != '5')
                    require(QString::fromUtf8(pax["linkpath"]) == e["target"].toString(), "TAR_LINK",
                            "链接目标不符");
            } else {
                QJsonObject stream;
                for (const auto &sv : e["streams"].toArray())
                    if (sv.toObject()["name"].toString() == streamName)
                        stream = sv.toObject();
                require(!stream.isEmpty() && stream["size"].toInteger() == qint64(size) && type == '0',
                        "TAR_STREAM", "TAR 数据流与清单不符");
                for (const auto &cv : stream["chunks"].toArray()) {
                    const auto c = cv.toObject();
                    result.locations[c["id"].toString()] = {file.pos() + c["offset"].toInteger(),
                                                            c["length"].toInteger()};
                }
            }
            require(file.seek(file.pos() + qint64(size)), "TAR_SEEK", file.errorString());
        }
        const auto pad = (512 - qint64(size) % 512) % 512;
        require(read(file, pad) == QByteArray(pad, '\0'), "TAR_PADDING", "TAR 对齐填充损坏");
        pax.clear();
    }
    require(zeroBlocks == 2 && file.atEnd() && !result.manifest.isEmpty() && pax.isEmpty(), "TAR_TERMINATOR",
            "TAR 末尾损坏");
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        require(observed.contains(it.key() + QChar(0)), "TAR_MISSING", "TAR 缺少成员");
        for (const auto &sv : it.value()["streams"].toArray())
            require(observed.contains(it.key() + QChar(0) + sv.toObject()["name"].toString()), "TAR_MISSING",
                    "TAR 缺少数据流");
    }
    return result;
}
void wrap(const QString &inner, const QString &destination, const BackupPlan &plan, const Context &context) {
    // Derive the password key once, then use a random master for all records.
    // Per-record seal derives independent keys and authenticates its envelope.
    // Header binding inside each record prevents undetected parameter edits.
    // Record sequence detects reordering/duplication before decoded bytes escape.
    // The final digest prevents a valid prefix being mistaken for a full archive.
    // QSaveFile direct-write fallback stays disabled to protect existing backups.
    const auto cipher = canonicalCipher(plan.cipher);
    require(cipher == "none" || !plan.password.isEmpty(), "PASSWORD_REQUIRED", "加密归档必须指定密码");
    require(plan.compression == "none" || plan.compression == "huffman" || plan.compression == "lz77",
            "COMPRESSION_UNSUPPORTED", plan.compression);
    QJsonObject header{{"version", 2},
                       {"format", plan.format == "pax" ? "tar" : plan.format},
                       {"compression", plan.compression},
                       {"cipher", cipher},
                       {"chunkSize", qint64(ChunkSize)}};
    QByteArray master;
    if (cipher != "none") {
        master = crypto::randomBytes(32);
        const auto salt = crypto::randomBytes(16);
        constexpr int iterations = 120000;
        const auto kek = crypto::pbkdf2(plan.password.toUtf8(), salt, iterations);
        header["salt"] = QString::fromLatin1(salt.toBase64());
        header["iterations"] = iterations;
        header["wrappedMaster"] = QString::fromLatin1(crypto::seal(master, kek, cipher).toBase64());
    }
    const auto headerBytes = json(header), headerHash = crypto::sha256(headerBytes);
    QFile source(inner);
    require(source.open(QIODevice::ReadOnly), "ARCHIVE_READ", source.errorString());
    require(QDir().mkpath(QFileInfo(destination).absolutePath()), "MKDIR_FAILED", destination);
    QSaveFile target(destination);
    target.setDirectWriteFallback(false);
    require(target.open(QIODevice::WriteOnly), "ARCHIVE_CREATE", target.errorString());
    write(target, OuterMagic);
    write(target, le(quint64(headerBytes.size()), 4));
    write(target, headerBytes);
    crypto::Sha256 whole;
    quint64 sequence = 0, total = 0;
    auto emitRecord = [&](char kind, const QByteArray &body) {
        const auto plain = le(sequence++, 8) + QByteArray(1, kind) + headerHash + body;
        const auto envelope =
            cipher == "none" ? plain + crypto::sha256(plain) : crypto::seal(plain, master, cipher);
        require(envelope.size() <= EnvelopeLimit, "ENVELOPE_SIZE", "归档封装块过长");
        write(target, le(quint64(envelope.size()), 4));
        write(target, envelope);
    };
    while (!source.atEnd()) {
        context.check();
        const auto bytes = source.read(ChunkSize);
        require(!bytes.isEmpty() || source.atEnd(), "ARCHIVE_READ", source.errorString());
        whole.update(bytes);
        total += quint64(bytes.size());
        emitRecord(1, le(quint64(bytes.size()), 4) + crypto::sha256(bytes) +
                          compression::encode(bytes, plan.compression));
        context.update(total, quint64(source.size()), "打包 / 压缩 / 加密");
    }
    emitRecord(0, le(total, 8) + whole.finish());
    context.check();
    require(target.commit(), "ARCHIVE_COMMIT", target.errorString());
}
QString unwrap(const QString &archive, const QString &inner, const QString &password,
               const Context &context) {
    // Reject expensive/unbounded KDF parameters before computing a password key.
    // Wrong passwords fail while opening the wrapped master, before target files.
    // Compression framing includes its own algorithm and original output size;
    // the caller additionally enforces one-megabyte output and SHA-256 checks.
    // Decrypted bytes remain in a temporary file until the final digest passes.
    // This implementation is streaming in memory but not an in-place decryptor.
    // A terminated/truncated archive never produces a successful Parsed result.
    QFile source(archive);
    require(source.open(QIODevice::ReadOnly), "ARCHIVE_READ", source.errorString());
    require(read(source, 8) == OuterMagic, "ARCHIVE_MAGIC", "这不是 ChengVault 归档");
    const auto headerLength = number(read(source, 4), 0, 4);
    require(headerLength <= 65536, "HEADER_SIZE", "归档头过长");
    const auto bytes = read(source, qint64(headerLength)), headerHash = crypto::sha256(bytes);
    const auto header = object(bytes);
    require(header["version"].toInt() == 2 && header["chunkSize"].toInteger() == ChunkSize, "ARCHIVE_VERSION",
            "不支持的归档版本");
    const auto format = header["format"].toString(), cipher = canonicalCipher(header["cipher"].toString());
    require(format == "indexed" || format == "tar", "ARCHIVE_FORMAT", "不支持的内部格式");
    QByteArray master;
    if (cipher != "none") {
        require(!password.isEmpty(), "PASSWORD_REQUIRED", "此归档需要密码");
        const auto salt = QByteArray::fromBase64(header["salt"].toString().toLatin1());
        const int iterations = header["iterations"].toInt();
        require(salt.size() == 16 && iterations >= 10000 && iterations <= 2000000, "KDF_PARAMETERS",
                "口令派生参数越界");
        const auto kek = crypto::pbkdf2(password.toUtf8(), salt, iterations);
        master = crypto::open(QByteArray::fromBase64(header["wrappedMaster"].toString().toLatin1()), kek);
        require(master.size() == 32, "KEY_SIZE", "归档主密钥损坏");
    }
    QFile target(inner);
    require(target.open(QIODevice::WriteOnly), "TEMP_CREATE", target.errorString());
    quint64 sequence = 0, total = 0;
    crypto::Sha256 whole;
    bool final = false;
    while (!source.atEnd()) {
        context.check();
        const auto length = number(read(source, 4), 0, 4);
        require(length > 0 && length <= EnvelopeLimit, "ENVELOPE_SIZE", "归档块长度越界");
        const auto envelope = read(source, qint64(length));
        QByteArray plain;
        if (cipher == "none") {
            require(envelope.size() >= 32, "ENVELOPE_SIZE", "归档块过短");
            plain = envelope.first(envelope.size() - 32);
            require(crypto::equal(crypto::sha256(plain), envelope.last(32)), "ARCHIVE_CHECKSUM",
                    "归档块校验失败");
        } else
            plain = crypto::open(envelope, master);
        require(plain.size() >= 41 && number(plain, 0, 8) == sequence++ &&
                    crypto::equal(plain.mid(9, 32), headerHash),
                "ARCHIVE_SEQUENCE", "归档块顺序或头部认证失败");
        const auto body = plain.mid(41);
        if (plain[8] == 0) {
            require(body.size() == 40 && number(body, 0, 8) == total &&
                        crypto::equal(body.mid(8), whole.finish()) && source.atEnd(),
                    "ARCHIVE_FINAL", "归档完整性终结记录校验失败");
            final = true;
            break;
        }
        require(plain[8] == 1 && body.size() >= 36, "ARCHIVE_RECORD", "非法归档记录");
        const auto expected = number(body, 0, 4);
        require(expected <= quint64(ChunkSize), "BLOCK_SIZE", "解压块超过上限");
        const auto data = compression::decode(body.mid(36), ChunkSize);
        require(quint64(data.size()) == expected && crypto::equal(crypto::sha256(data), body.mid(4, 32)),
                "ARCHIVE_CHECKSUM", "归档解压数据损坏");
        require(total <= quint64(std::numeric_limits<qint64>::max()) - quint64(data.size()), "ARCHIVE_SIZE",
                "归档长度溢出");
        write(target, data);
        whole.update(data);
        total += quint64(data.size());
        context.update(quint64(source.pos()), quint64(source.size()), "校验 / 解密 / 解压");
    }
    require(final, "ARCHIVE_TRUNCATED", "归档缺少完整性终结记录");
    require(target.flush(), "TEMP_FLUSH", target.errorString());
    return format;
}
Parsed parse(QFile &file, const QString &format, const Context &context) {
    return format == "indexed" ? parseIndex(file, context) : parseTar(file, context);
}
// A checked location identifies bytes; the plaintext digest is checked again.
// Repeated reads do not trust a previously returned mutable buffer.
QByteArray getChunk(QFile &file, const Parsed &parsed, const QString &id) {
    // Indexed and TAR readers share the same location abstraction. This checks
    // actual bytes even if a malformed inner manifest references the wrong range.
    // Standalone archive IDs are SHA-256; repositories may use keyed IDs instead.
    require(parsed.locations.contains(id), "CHUNK_MISSING", "归档数据块缺失");
    const auto location = parsed.locations[id];
    require(file.seek(location.offset), "ARCHIVE_SEEK", file.errorString());
    const auto data = read(file, location.length);
    require(crypto::sha256(data).toHex() == id.toLatin1(), "CHUNK_CHECKSUM", "归档数据块校验失败");
    return data;
}
} // namespace

// Collect stream blocks into a temporary spool before final serialization.
// No partial envelope is published if capture or encoding fails.
// The final QSaveFile commit is the successful artifact publication boundary.
TaskResult backup(const BackupPlan &plan, const Context &context) {
    // Capture to a private spool before encoding either inner format. This gives
    // both implementations one stable snapshot and makes algorithm comparison
    // meaningful. Temporary storage is automatically removed on every exception.
    // The destination is published only by wrap's final atomic commit.
    require(plan.format == "indexed" || plan.format == "tar" || plan.format == "pax", "ARCHIVE_FORMAT",
            plan.format);
    canonicalCipher(plan.cipher);
    require(!plan.destination.isEmpty(), "NO_DESTINATION", "请选择归档目标");
    // QSaveFile can follow an existing destination symlink. Refuse that leaf so
    // source/target overlap checks cannot be bypassed through an output alias.
    require(!QFileInfo(plan.destination).isSymLink(), "ARCHIVE_TARGET_LINK", "归档目标不可是链接");
    QTemporaryDir temp;
    require(temp.isValid(), "TEMP_CREATE", "无法创建临时目录");
    const auto spool = temp.path() + "/chunks";
    require(QDir().mkpath(spool), "TEMP_CREATE", spool);
    const auto manifest = fs::capture(
        plan,
        [&](const QByteArray &data) {
            const auto id = QString::fromLatin1(crypto::sha256(data).toHex());
            if (!QFileInfo::exists(spool + '/' + id))
                atomicWrite(spool + '/' + id, data);
            return id;
        },
        context);
    const auto inner = temp.path() + "/payload";
    if (plan.format == "indexed")
        writeIndex(inner, manifest, spool, context);
    else
        writeTar(inner, manifest, spool, context);
    wrap(inner, plan.destination, plan, context);
    TaskResult result;
    result.output = QFileInfo(plan.destination).absoluteFilePath();
    result.files = quint64(manifest["fileCount"].toInteger());
    result.bytes = quint64(manifest["totalBytes"].toInteger());
    result.storedBytes = quint64(QFileInfo(plan.destination).size());
    for (const auto &w : manifest["warnings"].toArray())
        result.warnings << w.toString();
    result.details = {{"format", plan.format},
                      {"compression", plan.compression},
                      {"cipher", canonicalCipher(plan.cipher)},
                      {"entries", manifest["entries"].toArray().size()}};
    return result;
}
// Names stay private until the complete encrypted envelope authenticates.
// The returned tree uses the same schema and checks as actual restoration.
QJsonObject inspect(const QString &path, const QString &password, const Context &context) {
    // Inspect returns the same manifest contract used by repository snapshots.
    // Encrypted archives cannot expose even filenames without the password.
    // archiveFormat is an informational extension ignored by fs::restore.
    QTemporaryDir temp;
    require(temp.isValid(), "TEMP_CREATE", "无法创建临时目录");
    const auto payload = temp.path() + "/payload", format = unwrap(path, payload, password, context);
    QFile file(payload);
    require(file.open(QIODevice::ReadOnly), "ARCHIVE_READ", file.errorString());
    auto parsed = parse(file, format, context);
    parsed.manifest["archiveFormat"] = format;
    return parsed.manifest;
}
// Decode to temporary storage before delegating any destination filesystem work.
// All format paths converge on the same content and metadata restoration code.
TaskResult restore(const QString &path, const QString &destination, const QString &password,
                   const RestoreOptions &options, const Context &context) {
    // All archive transforms are reversed automatically before NTFS restoration.
    // The filesystem layer handles selection, preflight, metadata and commit.
    // The open inner QFile lives for the whole synchronous chunk-source callback.
    QTemporaryDir temp;
    require(temp.isValid(), "TEMP_CREATE", "无法创建临时目录");
    const auto payload = temp.path() + "/payload", format = unwrap(path, payload, password, context);
    QFile file(payload);
    require(file.open(QIODevice::ReadOnly), "ARCHIVE_READ", file.errorString());
    const auto parsed = parse(file, format, context);
    auto result = fs::restore(
        parsed.manifest, [&](const QString &id) { return getChunk(file, parsed, id); }, destination, options,
        context);
    result.details["format"] = format;
    return result;
}
// Use the restore decoder without creating a destination directory tree.
// Verify metadata/index agreement as well as authenticated envelope bytes.
TaskResult verify(const QString &path, const QString &password, const Context &context) {
    // Verification checks envelope authentication plus every unique inner block.
    // It never creates a restored tree or changes existing destination data.
    // Report physical archive bytes separately from logical backed-up file bytes.
    QTemporaryDir temp;
    require(temp.isValid(), "TEMP_CREATE", "无法创建临时目录");
    const auto payload = temp.path() + "/payload", format = unwrap(path, payload, password, context);
    QFile file(payload);
    require(file.open(QIODevice::ReadOnly), "ARCHIVE_READ", file.errorString());
    const auto parsed = parse(file, format, context);
    for (auto it = parsed.locations.begin(); it != parsed.locations.end(); ++it) {
        context.check();
        getChunk(file, parsed, it.key());
    }
    TaskResult result;
    result.output = path;
    result.files = quint64(parsed.manifest["fileCount"].toInteger());
    result.bytes = quint64(parsed.manifest["totalBytes"].toInteger());
    result.storedBytes = quint64(QFileInfo(path).size());
    result.details = {{"format", format}, {"verifiedChunks", parsed.locations.size()}};
    return result;
}
} // namespace cv::archive
