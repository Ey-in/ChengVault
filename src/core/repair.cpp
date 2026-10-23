#include "chengvault/repair.hpp"
#include "chengvault/crypto.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>
#include <array>
#include <limits>

// Systematic RS(12,10) sidecar implementation
// -----------------------------------------
// This is erasure coding over byte positions in equally sized shards.
// SHA256 first locates a bad shard; RS then reconstructs up to two erasures.
// Without location information, two parity shards do not correct two arbitrary
// unknown errors. Reporting that distinction is part of the public contract.
// Each archive stripe consists of ten 1 MiB data shards and two parity shards.
// Padding belongs to shard hashes but not to the whole-archive hash.
// Matrix construction uses GF(256) arithmetic modulo polynomial 0x11D.
// Field tables are generated locally, and matrix inversion is local code.
// No third-party codec or external repair program performs these operations.
// The first ten rows of the generator are identity rows, retaining data bytes.
// Any ten distinct rows form an invertible matrix under the Vandermonde code.
// File recovery never edits the damaged source or its supplied sidecar.
// Validated output is published at a new path through QSaveFile.
// Temporary files are automatically removed on exceptions or cancellation.
// Both metadata copies have their own locator and checksum.
// A destroyed front length therefore cannot hide an intact tail copy.
// Conflicting valid metadata copies are rejected instead of guessing intent.
// Truncated shards are erasures; inserting bytes is not a supported corruption.
// The metadata checksum detects accidents, not hostile re-signing by an
// attacker who can rewrite both sidecar copies and all unkeyed checksums.
// The encrypted archive's own authentication must still pass after repair.
// Holding only one stripe keeps memory roughly proportional to 12 MiB.
// Index memory grows by 12 hashes per stripe and has an explicit upper bound.
// See docs/repair-format.md for byte layout, offsets, limits and failure rules.

namespace cv::repair {
namespace {
using Byte = unsigned char;
constexpr int DataCount = 10, ParityCount = 2, TotalCount = 12;
constexpr qint64 ShardSize = 1024 * 1024;
constexpr qint64 LocatorSize = 48;
constexpr qint64 MaxMetadataSize = 128 * 1024 * 1024;
constexpr quint64 MaxGroups = 100000;
using Matrix = QVector<QVector<Byte>>;

struct Field {
    std::array<Byte, 512> exp{};
    std::array<unsigned, 256> log{};
    std::array<std::array<Byte, 256>, 256> product{};
    Field() {
        // 2 is primitive for GF(2^8) modulo 0x11D. Exponent duplication
        // removes a modulo operation from the inner multiplication loop.
        unsigned value = 1;
        for (unsigned i = 0; i < 255; ++i) {
            exp[i] = Byte(value);
            log[value] = i;
            value <<= 1;
            if (value & 256)
                value ^= 0x11d;
        }
        for (unsigned i = 255; i < 512; ++i)
            exp[i] = exp[i - 255];
        for (unsigned a = 1; a < 256; ++a)
            for (unsigned b = 1; b < 256; ++b)
                product[a][b] = exp[log[a] + log[b]];
    }
    Byte multiply(Byte a, Byte b) const {
        return product[a][b];
    }
    Byte inverse(Byte a) const {
        require(a != 0, "REPAIR_MATRIX", "有限域不能对零求逆");
        return exp[255 - log[a]];
    }
};
const Field &field() {
    static const Field value;
    return value;
}
Matrix invert(Matrix matrix) {
    // Input and output matrices have the same finite-field element type.
    // A zero pivot is replaced by a later nonzero row before normalization.
    // Every row operation is mirrored on the identity matrix.
    // Returning a result after a missing pivot would silently corrupt files;
    // singular matrices therefore fail with a structured error.
    const qsizetype n = matrix.size();
    Matrix inverse(n, QVector<Byte>(n, 0));
    for (qsizetype row = 0; row < n; ++row) {
        require(matrix[row].size() == n, "REPAIR_MATRIX", "矩阵必须为方阵");
        inverse[row][row] = 1;
    }
    // Gaussian elimination performs row operations on [A | I]. All addition
    // and subtraction in characteristic two are XOR; division uses inverse.
    for (qsizetype col = 0; col < n; ++col) {
        qsizetype pivot = col;
        while (pivot < n && matrix[pivot][col] == 0)
            ++pivot;
        require(pivot < n, "REPAIR_MATRIX", "修复矩阵不可逆");
        if (pivot != col) {
            matrix.swapItemsAt(pivot, col);
            inverse.swapItemsAt(pivot, col);
        }
        const Byte factor = field().inverse(matrix[col][col]);
        for (qsizetype j = 0; j < n; ++j) {
            matrix[col][j] = field().multiply(matrix[col][j], factor);
            inverse[col][j] = field().multiply(inverse[col][j], factor);
        }
        for (qsizetype row = 0; row < n; ++row) {
            if (row == col || matrix[row][col] == 0)
                continue;
            const Byte multiplier = matrix[row][col];
            for (qsizetype j = 0; j < n; ++j) {
                matrix[row][j] ^= field().multiply(multiplier, matrix[col][j]);
                inverse[row][j] ^= field().multiply(multiplier, inverse[col][j]);
            }
        }
    }
    return inverse;
}
const Matrix &generator() {
    // The construction is cached once, so each archive does not redo matrix
    // inversion. Values 1..12 are distinct nonzero field points.
    // Multiplying by the inverse of the top square block is a change of basis,
    // retaining the property that any ten encoded rows determine the data.
    // Tests enumerate every one- and two-erasure combination across all rows.
    static const Matrix result = [] {
        Matrix vandermonde(TotalCount, QVector<Byte>(DataCount, 0));
        for (int row = 0; row < TotalCount; ++row) {
            Byte value = 1;
            for (int col = 0; col < DataCount; ++col) {
                vandermonde[row][col] = value;
                value = field().multiply(value, Byte(row + 1));
            }
        }
        Matrix top;
        for (int row = 0; row < DataCount; ++row)
            top.append(vandermonde[row]);
        const Matrix inverseTop = invert(top);
        Matrix systematic(TotalCount, QVector<Byte>(DataCount, 0));
        // G = V * inverse(V_top). The first ten rows become I, while any
        // ten rows remain invertible: data shards pass through unchanged.
        for (int row = 0; row < TotalCount; ++row)
            for (int col = 0; col < DataCount; ++col)
                for (int k = 0; k < DataCount; ++k)
                    systematic[row][col] ^= field().multiply(vandermonde[row][k], inverseTop[k][col]);
        return systematic;
    }();
    return result;
}
QByteArray combine(const QVector<QByteArray> &shards, const QVector<Byte> &coefficients, qsizetype size) {
    // At a fixed byte offset, each shard byte is a field element.
    // Multiply each source element by its row coefficient and XOR the products.
    // Zero coefficients contribute nothing; generated multiplication tables
    // avoid log/exp work in the large inner byte loop.
    // Callers validate equal shard lengths before reaching this function.
    QByteArray result(size, char(0));
    Byte *output = reinterpret_cast<Byte *>(result.data());
    for (qsizetype shard = 0; shard < coefficients.size(); ++shard) {
        const Byte coefficient = coefficients[shard];
        if (coefficient == 0)
            continue;
        const Byte *input = reinterpret_cast<const Byte *>(shards[shard].constData());
        const auto &table = field().product[coefficient];
        for (qsizetype offset = 0; offset < size; ++offset)
            output[offset] ^= table[input[offset]];
    }
    return result;
}
void put64(QByteArray &output, quint64 value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        output.append(char(value >> shift));
}
quint64 get64(const QByteArray &data, qsizetype offset) {
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | Byte(data[offset + i]);
    return value;
}
void writeAll(QIODevice &file, const QByteArray &data) {
    require(file.write(data) == data.size(), "WRITE", "修复文件写入失败，可能磁盘空间不足");
}
QByteArray checkedRead(QFile &file, qint64 offset, qint64 size) {
    require(file.seek(offset), "READ", "无法定位修复数据");
    QByteArray data = file.read(size);
    require(file.error() == QFileDevice::NoError, "READ", "修复数据读取失败");
    return data;
}
struct Metadata {
    QByteArray json;
    quint64 size = 0;
    quint64 groups = 0;
    QByteArray hash;
    QVector<QVector<QByteArray>> digests;
    qint64 parityOffset = 0;
    bool headValid = false;
    bool tailValid = false;
};
QByteArray candidate(QFile &file, bool tail) {
    // A candidate is independently located from one end of the sidecar.
    // Invalid lengths fail before reads or allocations of attacker-sized data.
    // An empty return value means this copy cannot be trusted, allowing the
    // other locator to be tried without depending on corrupt offset fields.
    // JSON is not parsed until its exact-byte checksum has matched.
    if (file.size() < LocatorSize)
        return {};
    const qint64 locatorOffset = tail ? file.size() - LocatorSize : 0;
    const QByteArray locator = checkedRead(file, locatorOffset, LocatorSize);
    if (locator.size() != LocatorSize)
        return {};
    const QByteArray magic = tail ? locator.last(8) : locator.first(8);
    if (magic != (tail ? "CVRTAIL1" : "CVRHEAD1"))
        return {};
    const quint64 length = get64(locator, tail ? 32 : 8);
    if (length == 0 || length > quint64(MaxMetadataSize) || length > quint64(file.size() - LocatorSize))
        return {};
    const qint64 offset = tail ? file.size() - LocatorSize - qint64(length) : LocatorSize;
    const QByteArray json = checkedRead(file, offset, qint64(length));
    const QByteArray digest = tail ? locator.first(32) : locator.mid(16, 32);
    return crypto::equal(crypto::sha256(json), digest) ? json : QByteArray{};
}
Metadata readMetadata(QFile &file) {
    // Header and footer are deliberately asymmetric byte layouts:
    // head = magic | length | digest | json, tail = json | digest | length | magic.
    // Reading the first or last 48 bytes is therefore independently sufficient
    // to locate one JSON copy. No damaged head offset is reused for the tail.
    // A good checksum on both copies does not justify choosing between them;
    // conflicting bytes mean the archive-to-sidecar relationship is ambiguous.
    // The same fixed JSON length also derives parity offsets if the head magic
    // itself was damaged while the footer remains intact.
    // Bounds include both memory consumed by the index and file-offset range.
    // The group array is validated against originalSize instead of trusting a
    // separately supplied group count that could hide unverified trailing data.
    // Strict hexadecimal validation avoids permissive fromHex truncation.
    // A valid checksum is necessary but not sufficient: semantic validation
    // checks format version, shard parameters, original size and group count.
    // Decimal strings keep full 64-bit precision through Qt's JSON handling.
    // Hex digests are strict lowercase and exactly 32 decoded bytes.
    // The parity region starts immediately after the fixed locator and JSON.
    // If only the tail index survives, its length reconstructs that same start.
    const QByteArray head = candidate(file, false), tail = candidate(file, true);
    require(!head.isEmpty() || !tail.isEmpty(), "REPAIR_METADATA", "头尾修复索引均损坏或缺失");
    require(head.isEmpty() || tail.isEmpty() || head == tail, "REPAIR_METADATA", "头尾修复索引相互冲突");
    Metadata metadata;
    metadata.json = head.isEmpty() ? tail : head;
    metadata.headValid = !head.isEmpty();
    metadata.tailValid = !tail.isEmpty();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(metadata.json, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "REPAIR_METADATA",
            "修复索引 JSON 无效");
    const auto object = document.object();
    require(object.value("version").toInt() == 1 && object.value("shardSize").toInt() == ShardSize &&
                object.value("dataShards").toInt() == DataCount &&
                object.value("parityShards").toInt() == ParityCount,
            "REPAIR_METADATA", "不支持的修复编码参数");
    bool validSize = false;
    const QString sizeText = object.value("originalSize").toString();
    metadata.size = sizeText.toULongLong(&validSize);
    require(validSize && QRegularExpression("^(0|[1-9][0-9]*)$").match(sizeText).hasMatch() &&
                metadata.size <= quint64(std::numeric_limits<qint64>::max()),
            "REPAIR_METADATA", "无效归档原始长度");
    metadata.groups = (metadata.size + quint64(DataCount * ShardSize) - 1) / quint64(DataCount * ShardSize);
    require(metadata.groups <= MaxGroups, "REPAIR_METADATA", "归档超过当前修复索引上限");
    const QRegularExpression digestPattern("^[0-9a-f]{64}$");
    auto decodeDigest = [&](const QJsonValue &value) {
        const QString text = value.toString();
        require(digestPattern.match(text).hasMatch(), "REPAIR_METADATA", "修复索引摘要格式错误");
        return QByteArray::fromHex(text.toLatin1());
    };
    metadata.hash = decodeDigest(object.value("archiveSha256"));
    const QJsonArray groups = object.value("groups").toArray();
    require(object.value("groups").isArray() && quint64(groups.size()) == metadata.groups, "REPAIR_METADATA",
            "修复分组数与长度不符");
    for (const auto &groupValue : groups) {
        const auto values = groupValue.toArray();
        require(values.size() == TotalCount, "REPAIR_METADATA", "每组必须包含 12 个分片摘要");
        QVector<QByteArray> row;
        for (const auto &value : values)
            row.append(decodeDigest(value));
        metadata.digests.append(row);
    }
    metadata.parityOffset = LocatorSize + metadata.json.size();
    const quint64 expectedSize = quint64(metadata.parityOffset) + metadata.groups * ParityCount * ShardSize +
                                 quint64(metadata.json.size()) + LocatorSize;
    // Truncation is a recoverable erasure; insertion/appending is not. Fixed
    // offsets are necessary because the code corrects shards, not byte shifts.
    require(quint64(file.size()) <= expectedSize, "REPAIR_METADATA", "修复文件存在多余或插入字节");
    return metadata;
}
struct Stripe {
    QVector<QByteArray> shards;
    QVector<bool> present;
    int missing = 0;
};
Stripe readStripe(QFile &archive, QFile &parity, const Metadata &metadata, quint64 group) {
    // Data positions are based on the original archive size, not current EOF.
    // A shortened archive must not shift subsequent shard boundaries.
    // Data shards beyond the last real byte are known zero-filled padding.
    // A partial real shard is padded only after all its expected bytes arrive.
    // Bad bytes never enter matrix decoding as if they were valid survivors.
    // Replacing them with zeros avoids accidental propagation before repair.
    Stripe stripe;
    stripe.shards.resize(TotalCount);
    stripe.present.resize(TotalCount);
    for (int shard = 0; shard < TotalCount; ++shard) {
        QByteArray bytes;
        bool complete = false;
        if (shard < DataCount) {
            const quint64 offset = (group * DataCount + quint64(shard)) * ShardSize;
            const qint64 wanted =
                offset < metadata.size ? qint64(std::min<quint64>(ShardSize, metadata.size - offset)) : 0;
            if (wanted == 0) {
                bytes = QByteArray(ShardSize, char(0));
                complete = true;
            } else {
                bytes = checkedRead(archive, qint64(offset), wanted);
                complete = bytes.size() == wanted;
                if (complete)
                    bytes.append(QByteArray(ShardSize - wanted, char(0)));
            }
        } else {
            const qint64 offset =
                metadata.parityOffset + qint64(group * ParityCount + quint64(shard - DataCount)) * ShardSize;
            bytes = checkedRead(parity, offset, ShardSize);
            complete = bytes.size() == ShardSize;
        }
        const bool valid =
            complete && crypto::equal(crypto::sha256(bytes), metadata.digests[qsizetype(group)][shard]);
        stripe.present[shard] = valid;
        if (!valid) {
            ++stripe.missing;
            bytes = QByteArray(ShardSize, char(0));
        }
        stripe.shards[shard] = bytes;
    }
    return stripe;
}
void requireNewPath(const QString &source, const QString &sidecar, const QString &output) {
    // Recovery is intentionally a new-file operation, making it reviewable.
    // Existing paths and symlinks are refused before any temporary writes.
    // Absolute normalized names also catch direct source/destination equality.
    // Parent directories must exist so a typo does not create a new tree.
    const QFileInfo outputInfo(output);
    require(!outputInfo.exists() && !outputInfo.isSymLink(), "EXISTS", "修复操作只写入不存在的新文件");
    const QString absolute = QDir::cleanPath(outputInfo.absoluteFilePath());
    require(
        absolute.compare(QDir::cleanPath(QFileInfo(source).absoluteFilePath()), Qt::CaseInsensitive) != 0 &&
            absolute.compare(QDir::cleanPath(QFileInfo(sidecar).absoluteFilePath()), Qt::CaseInsensitive) !=
                0,
        "PATH", "输出不能覆盖归档或修复文件");
    require(outputInfo.absoluteDir().exists(), "PATH", "输出目录不存在");
}
} // namespace

QVector<QByteArray> encodeShards(const QVector<QByteArray> &data) {
    // The algebra interface accepts small equal-length shards for exhaustive
    // tests; only file-level create/check/recover fixes them to exactly 1 MiB.
    // Returning all twelve rows makes identity and reconstruction observable.
    // QByteArray sharing preserves source data without an unnecessary copy.
    require(data.size() == DataCount, "REPAIR_SHARDS", "RS(12,10) 需要 10 个数据分片");
    const qsizetype size = data[0].size();
    for (const auto &shard : data)
        require(shard.size() == size, "REPAIR_SHARDS", "分片长度必须相同");
    QVector<QByteArray> result = data;
    for (int row = DataCount; row < TotalCount; ++row)
        result.append(combine(data, generator()[row], size));
    return result;
}
QVector<QByteArray> recoverShards(const QVector<QByteArray> &shards, const QVector<bool> &present) {
    // Select ten verified rows, invert their generator submatrix, and apply it
    // to the corresponding received bytes to recover the ten data shards.
    // Re-encoding produces all twelve rows for both data and parity validation.
    // With only ten survivors, algebra alone cannot detect a falsely marked
    // survivor; file-level per-shard and whole-archive hashes close that check.
    require(shards.size() == TotalCount && present.size() == TotalCount, "REPAIR_SHARDS",
            "需要 12 个分片状态");
    QVector<QByteArray> selected;
    Matrix coefficients;
    qsizetype size = -1;
    int survivors = 0;
    for (int row = 0; row < TotalCount; ++row) {
        if (!present[row])
            continue;
        ++survivors;
        if (size < 0)
            size = shards[row].size();
        require(shards[row].size() == size, "REPAIR_SHARDS", "有效分片长度不一致");
        if (selected.size() < DataCount) {
            selected.append(shards[row]);
            coefficients.append(generator()[row]);
        }
    }
    require(survivors >= DataCount, "REPAIR_CAPACITY", "同一组超过两个损坏分片，无法修复");
    const Matrix inverse = invert(coefficients);
    QVector<QByteArray> data;
    for (int row = 0; row < DataCount; ++row)
        data.append(combine(selected, inverse[row], size));
    const QVector<QByteArray> recovered = encodeShards(data);
    // Surviving rows beyond the selected ten must agree, catching incorrect
    // present flags when there is redundancy. File callers also verify SHA256.
    for (int row = 0; row < TotalCount; ++row)
        require(!present[row] || recovered[row] == shards[row], "REPAIR_INCONSISTENT", "有效分片彼此不一致");
    return recovered;
}

TaskResult create(const QString &archivePath, const QString &sidecarPath, const Context &context) {
    // Metadata size is only known after all stripe hashes have been computed.
    // Spool parity into a bounded-memory temporary file, then stream the final
    // layout into QSaveFile. Exceptions remove temporary state automatically.
    // File size and modification time are checked after scanning for changes.
    // A concurrent writer that preserves both values is outside this snapshot
    // guarantee; callers should supply a committed, stable archive file.
    context.check();
    requireNewPath(archivePath, archivePath, sidecarPath);
    QFile archive(archivePath);
    require(archive.open(QIODevice::ReadOnly), "READ", "无法打开待保护归档");
    const qint64 originalSize = archive.size();
    const QDateTime originalModified = QFileInfo(archivePath).lastModified();
    const quint64 groups = (quint64(originalSize) + DataCount * ShardSize - 1) / (DataCount * ShardSize);
    require(groups <= MaxGroups, "REPAIR_SIZE", "归档超过当前纠错索引容量");
    QTemporaryFile parity(QFileInfo(sidecarPath).absolutePath() + "/.chengvault-parity-XXXXXX");
    require(parity.open(), "WRITE", "无法创建校验临时文件");
    crypto::Sha256 fullHash;
    QJsonArray digestGroups;
    for (quint64 group = 0; group < groups; ++group) {
        context.check();
        QVector<QByteArray> data;
        for (int shard = 0; shard < DataCount; ++shard) {
            const qint64 wanted = std::min<qint64>(ShardSize, originalSize - archive.pos());
            QByteArray block = archive.read(wanted);
            require(block.size() == wanted, "READ", "归档读取失败或备份期间发生变化");
            fullHash.update(block);
            block.append(QByteArray(ShardSize - block.size(), char(0)));
            data.append(block);
        }
        const auto encoded = encodeShards(data);
        QJsonArray digests;
        for (int shard = 0; shard < TotalCount; ++shard) {
            digests.append(QString::fromLatin1(crypto::sha256(encoded[shard]).toHex()));
            if (shard >= DataCount)
                writeAll(parity, encoded[shard]);
        }
        digestGroups.append(digests);
        context.update(quint64(archive.pos()), quint64(originalSize), "生成 RS(12,10) 冗余分片");
    }
    const QByteArray completeHash = fullHash.finish();
    require(archive.size() == originalSize && QFileInfo(archivePath).lastModified() == originalModified,
            "SOURCE_CHANGED", "生成修复文件期间归档发生变化");
    const QJsonObject metadata{{"version", 1},
                               {"shardSize", int(ShardSize)},
                               {"dataShards", DataCount},
                               {"parityShards", ParityCount},
                               {"originalSize", QString::number(originalSize)},
                               {"archiveSha256", QString::fromLatin1(completeHash.toHex())},
                               {"groups", digestGroups}};
    const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    require(json.size() <= MaxMetadataSize, "REPAIR_SIZE", "修复索引超过上限");
    const QByteArray digest = crypto::sha256(json);
    QByteArray header("CVRHEAD1", 8);
    put64(header, quint64(json.size()));
    header += digest;
    QByteArray footer = digest;
    put64(footer, quint64(json.size()));
    footer += "CVRTAIL1";
    QSaveFile output(sidecarPath);
    output.setDirectWriteFallback(false);
    require(output.open(QIODevice::WriteOnly), "WRITE", "无法创建修复文件");
    writeAll(output, header);
    writeAll(output, json);
    require(parity.seek(0), "READ", "无法读取校验临时文件");
    while (!parity.atEnd()) {
        context.check();
        const auto bytes = parity.read(ChunkSize);
        require(!bytes.isEmpty(), "READ", "校验临时文件读取失败");
        writeAll(output, bytes);
    }
    writeAll(output, json);
    writeAll(output, footer);
    context.check();
    require(!QFileInfo::exists(sidecarPath), "EXISTS", "输出路径已被其他进程创建");
    require(output.commit(), "WRITE", "无法提交修复文件");
    TaskResult result;
    result.output = sidecarPath;
    result.files = 1;
    result.bytes = quint64(originalSize);
    result.storedBytes = quint64(QFileInfo(sidecarPath).size());
    result.details = {{"groups", QString::number(groups)},
                      {"dataShards", DataCount},
                      {"parityShards", ParityCount},
                      {"archiveSha256", QString::fromLatin1(completeHash.toHex())}};
    return result;
}

QJsonObject check(const QString &archivePath, const QString &sidecarPath, const Context &context) {
    // Checking is read-only. It reports archive health separately from parity
    // health, so intact source data with a damaged sidecar is not overlooked.
    // Recoverability is evaluated per stripe, never by a global damage ratio.
    // A complete archive checksum independently validates original file length.
    // The returned JSON uses strings for potentially large counters.
    context.check();
    QFile archive(archivePath), sidecar(sidecarPath);
    require(archive.open(QIODevice::ReadOnly) && sidecar.open(QIODevice::ReadOnly), "READ",
            "无法打开归档或修复文件");
    const auto metadata = readMetadata(sidecar);
    require(quint64(archive.size()) <= metadata.size, "REPAIR_SIZE", "归档增加了字节，不能按固定分片修复");
    QJsonArray damaged;
    bool recoverable = true;
    quint64 totalDamaged = 0;
    for (quint64 group = 0; group < metadata.groups; ++group) {
        context.check();
        const auto stripe = readStripe(archive, sidecar, metadata, group);
        if (stripe.missing) {
            QJsonArray missing;
            for (int shard = 0; shard < TotalCount; ++shard)
                if (!stripe.present[shard])
                    missing.append(shard);
            damaged.append(QJsonObject{{"group", QString::number(group)},
                                       {"shards", missing},
                                       {"recoverable", stripe.missing <= ParityCount}});
            totalDamaged += quint64(stripe.missing);
            if (stripe.missing > ParityCount)
                recoverable = false;
        }
        context.update(group + 1, metadata.groups, "检查归档与冗余分片");
    }
    const bool wholeHashValid = quint64(archive.size()) == metadata.size &&
                                crypto::equal(crypto::hashFile(archivePath, context), metadata.hash);
    // In a consistent index, intact data shards imply a matching full hash.
    // If that invariant fails, do not claim the archive can be repaired.
    if (!wholeHashValid && totalDamaged == 0)
        recoverable = false;
    return {{"healthy", wholeHashValid && totalDamaged == 0 && metadata.headValid && metadata.tailValid},
            {"archiveHealthy", wholeHashValid},
            {"recoverable", recoverable},
            {"damagedShards", QString::number(totalDamaged)},
            {"damagedGroups", damaged},
            {"headMetadataValid", metadata.headValid},
            {"tailMetadataValid", metadata.tailValid},
            {"originalSize", QString::number(metadata.size)},
            {"archiveSha256", QString::fromLatin1(metadata.hash.toHex())}};
}

TaskResult recover(const QString &archivePath, const QString &sidecarPath, const QString &outputPath,
                   const Context &context) {
    // Availability is per stripe: damage in separate stripes is independent.
    // A missing parity shard consumes the same erasure budget as missing data.
    // Two damaged data shards with a third damaged parity shard are impossible
    // to repair even if total damaged bytes are less than twenty percent.
    // Rebuilding parity after decoding gives a consistency check for survivors.
    // File checksums then verify the recovered values against the saved index.
    // A healthy archive can be copied to a new result using the same path;
    // its sidecar might be damaged while all original data remains readable.
    // recover() does not overwrite or regenerate that sidecar automatically.
    // A caller can explicitly create a new sidecar from the repaired archive.
    // Since reconstruction operates on ciphertext bytes, a repair tool needs
    // no archive password; opening that archive still enforces authentication.
    // Successful repair never bypasses the password or produces plaintext.
    // Validation is performed before publishing, not after replacing a file.
    // Every reconstructed row must match its stored digest, including parity.
    // The final stream hash is computed over only the original unpadded bytes.
    // A cancellation after useful work still removes the unpublished result.
    // Rechecking target existence reduces ordinary concurrent overwrite risk;
    // it does not claim immunity to a hostile local process racing the rename.
    context.check();
    requireNewPath(archivePath, sidecarPath, outputPath);
    QFile archive(archivePath), sidecar(sidecarPath);
    require(archive.open(QIODevice::ReadOnly) && sidecar.open(QIODevice::ReadOnly), "READ",
            "无法打开归档或修复文件");
    const auto metadata = readMetadata(sidecar);
    require(quint64(archive.size()) <= metadata.size, "REPAIR_SIZE", "归档增加了字节，不能按固定分片修复");
    QSaveFile output(outputPath);
    output.setDirectWriteFallback(false);
    require(output.open(QIODevice::WriteOnly), "WRITE", "无法创建修复结果");
    crypto::Sha256 fullHash;
    quint64 repaired = 0, written = 0;
    for (quint64 group = 0; group < metadata.groups; ++group) {
        context.check();
        auto stripe = readStripe(archive, sidecar, metadata, group);
        require(stripe.missing <= ParityCount, "REPAIR_CAPACITY", "同一组超过两个损坏或缺失分片");
        if (stripe.missing) {
            stripe.shards = recoverShards(stripe.shards, stripe.present);
            repaired += quint64(stripe.missing);
        }
        for (int shard = 0; shard < TotalCount; ++shard)
            require(crypto::equal(crypto::sha256(stripe.shards[shard]),
                                  metadata.digests[qsizetype(group)][shard]),
                    "REPAIR_CHECKSUM", "修复后的分片摘要不符");
        for (int shard = 0; shard < DataCount; ++shard) {
            const qsizetype length = qsizetype(std::min<quint64>(ShardSize, metadata.size - written));
            const QByteArray bytes = stripe.shards[shard].first(length);
            writeAll(output, bytes);
            fullHash.update(bytes);
            written += quint64(length);
        }
        context.update(written, metadata.size, "修复归档到新文件");
    }
    require(written == metadata.size && crypto::equal(fullHash.finish(), metadata.hash), "REPAIR_CHECKSUM",
            "修复后的归档摘要不符");
    context.check();
    require(!QFileInfo::exists(outputPath), "EXISTS", "输出路径已被其他进程创建");
    require(output.commit(), "WRITE", "无法提交已校验的修复结果");
    TaskResult result;
    result.output = outputPath;
    result.files = 1;
    result.bytes = written;
    result.storedBytes = written;
    result.details = {{"repairedShards", QString::number(repaired)},
                      {"archiveSha256", QString::fromLatin1(metadata.hash.toHex())}};
    if (!metadata.headValid || !metadata.tailValid)
        result.warnings.append("一份修复索引无效；本次使用另一份有效索引");
    return result;
}
} // namespace cv::repair
