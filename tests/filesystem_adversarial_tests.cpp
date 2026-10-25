#include "chengvault/archive.hpp"
#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include "test_support.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <algorithm>
#include <cstring>

#include <windows.h>

#include <aclapi.h>
#include <winioctl.h>

namespace {
using cv::test::check;

// Malformed-input tests check the public error category as well as rejection.
// This prevents an unrelated parser failure from hiding an untested guard.
template <class F> void errorCode(const QString &expected, F &&fn) {
    try {
        fn();
    } catch (const cv::Error &e) {
        check(e.code == expected, "expected " + expected + ", received " + e.code);
        return;
    }
    check(false, "expected error " + expected);
}
struct Fixture {
    QTemporaryDir directory;
    QString source;
    cv::BackupPlan plan;
    QMap<QString, QByteArray> chunks;
    Fixture() {
        check(directory.isValid());
        source = directory.path() + "/source";
        check(QDir().mkpath(source + "/empty"));
        cv::atomicWrite(source + "/a.txt", "alpha file bytes");
        cv::atomicWrite(source + "/b.bin", "beta");
        plan.sources = {source};
        plan.destination = directory.path() + "/archive.cv";
        plan.compression = "none";
    }
    QJsonObject capture() {
        return cv::fs::capture(plan, [&](const QByteArray &b) {
            const QString id = QString::fromLatin1(cv::crypto::sha256(b).toHex());
            chunks[id] = b;
            return id;
        });
    }
    QString output() const {
        return directory.path() + "/restored";
    }
    cv::TaskResult restore(const QJsonObject &manifest, const cv::RestoreOptions &options = {}) {
        return cv::fs::restore(
            manifest, [&](const QString &id) { return chunks.value(id); }, output(), options);
    }
};
QByteArray integer(quint64 n, int length) {
    QByteArray out(length, '\0');
    for (int i = 0; i < length; ++i)
        out[i] = char(n >> (8 * i));
    return out;
}
quint64 uintAt(const QByteArray &data, qsizetype pos, int size) {
    check(pos >= 0 && pos + size <= data.size());
    quint64 n = 0;
    for (int i = 0; i < size; ++i)
        n |= quint64(quint8(data[pos + i])) << (8 * i);
    return n;
}
QByteArray compact(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Build only the public unencrypted framing described in archive-format.md.
// Recompute outer checksums deliberately: inner parser tests must reach the
// malformed TAR/index fields rather than stop at an outer checksum mismatch.
QByteArray frame(const QByteArray &inner, const QString &format = "indexed", const QJsonObject &extra = {},
                 const std::function<void(QList<QByteArray> &)> &mutate = {}) {
    QJsonObject header{{"version", 2},
                       {"format", format},
                       {"compression", "none"},
                       {"cipher", "none"},
                       {"chunkSize", qint64(cv::ChunkSize)}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        header[it.key()] = it.value();
    const auto raw = compact(header), digest = cv::crypto::sha256(raw);
    QList<QByteArray> records;
    quint64 sequence = 0;
    for (qsizetype pos = 0; pos < inner.size(); pos += cv::ChunkSize) {
        const auto part = inner.mid(pos, cv::ChunkSize);
        records.append(integer(sequence++, 8) + QByteArray(1, 1) + digest + integer(part.size(), 4) +
                       cv::crypto::sha256(part) + cv::compression::encode(part, "none"));
    }
    records.append(integer(sequence, 8) + QByteArray(1, 0) + digest + integer(inner.size(), 8) +
                   cv::crypto::sha256(inner));
    if (mutate)
        mutate(records);
    QByteArray bytes = QByteArray("CVARCH2\n", 8) + integer(raw.size(), 4) + raw;
    for (const auto &record : records) {
        const auto envelope = record + cv::crypto::sha256(record);
        bytes += integer(envelope.size(), 4) + envelope;
    }
    return bytes;
}
QByteArray innerBytes(const QByteArray &archive) {
    qsizetype pos = 12 + qsizetype(uintAt(archive, 8, 4));
    QByteArray inner;
    while (pos < archive.size()) {
        const qsizetype length = qsizetype(uintAt(archive, pos, 4));
        const auto record = archive.mid(pos + 4, length);
        check(record.size() == length && length >= 73);
        pos += 4 + length;
        if (record[8] == 0)
            break;
        inner += cv::compression::decode(record.mid(77, length - 109));
    }
    return inner;
}
QByteArray makeIndex(const QJsonObject &manifest, const QMap<QString, QByteArray> &chunks) {
    const auto meta = compact(manifest);
    QByteArray bytes =
        QByteArray("CVIDX01\n", 8) + integer(meta.size(), 8) + meta + integer(chunks.size(), 8);
    for (auto it = chunks.begin(); it != chunks.end(); ++it)
        bytes += it.key().toLatin1() + integer(it.value().size(), 4) + it.value();
    return bytes;
}
void changeEntry(QJsonObject &manifest, const QString &path, const std::function<void(QJsonObject &)> &fn) {
    auto entries = manifest["entries"].toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto e = entries[i].toObject();
        if (e["path"] == path) {
            fn(e);
            entries[i] = e;
            manifest["entries"] = entries;
            return;
        }
    }
    check(false, "missing fixture entry");
}
void changeStream(QJsonObject &e, const std::function<void(QJsonObject &)> &fn) {
    auto streams = e["streams"].toArray();
    check(!streams.isEmpty());
    auto stream = streams[0].toObject();
    fn(stream);
    streams[0] = stream;
    e["streams"] = streams;
}
void changeChunk(QJsonObject &e, const std::function<void(QJsonObject &)> &fn) {
    changeStream(e, [&](QJsonObject &s) {
        auto chunks = s["chunks"].toArray();
        check(!chunks.isEmpty());
        auto chunk = chunks[0].toObject();
        fn(chunk);
        chunks[0] = chunk;
        s["chunks"] = chunks;
    });
}
std::wstring win(const QString &path) {
    return QDir::toNativeSeparators(path).toStdWString();
}
template <class F> void restrictedToken(F &&fn) {
    HANDLE original = nullptr, restricted = nullptr;
    check(
        OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &original));
    const bool created = CreateRestrictedToken(original, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0,
                                               nullptr, &restricted);
    CloseHandle(original);
    check(created);
    if (!ImpersonateLoggedOnUser(restricted)) {
        CloseHandle(restricted);
        check(false, "cannot impersonate restricted test token");
    }
    try {
        fn();
    } catch (...) {
        RevertToSelf();
        CloseHandle(restricted);
        throw;
    }
    RevertToSelf();
    CloseHandle(restricted);
}
void junction(const QString &path, const QString &target) {
    check(QDir().mkdir(path));
    const auto substitute = ("\\??\\" + QDir::toNativeSeparators(target)).toStdWString();
    const auto print = QDir::toNativeSeparators(target).toStdWString();
    QByteArray data = integer(IO_REPARSE_TAG_MOUNT_POINT, 4) +
                      integer((substitute.size() + print.size() + 2) * 2 + 8, 2) + integer(0, 2) +
                      integer(0, 2) + integer(substitute.size() * 2, 2) +
                      integer((substitute.size() + 1) * 2, 2) + integer(print.size() * 2, 2);
    data += QByteArray(reinterpret_cast<const char *>(substitute.c_str()), (substitute.size() + 1) * 2);
    data += QByteArray(reinterpret_cast<const char *>(print.c_str()), (print.size() + 1) * 2);
    HANDLE h = CreateFileW(win(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    check(h != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, data.data(), DWORD(data.size()), nullptr, 0,
                                    &count, nullptr);
    CloseHandle(h);
    check(ok);
}
void sparseFile(const QString &path, qint64 size, qint64 at, const QByteArray &bytes) {
    HANDLE h =
        CreateFileW(win(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    const bool sparse = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr);
    LARGE_INTEGER pos{};
    pos.QuadPart = size;
    bool ok = sparse && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    pos.QuadPart = at;
    ok = ok && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
         WriteFile(h, bytes.constData(), DWORD(bytes.size()), &count, nullptr) &&
         count == DWORD(bytes.size());
    CloseHandle(h);
    check(ok);
}
QByteArray paxRecord(const QByteArray &key, const QByteArray &value) {
    const auto content = key + '=' + value + '\n';
    qsizetype size = content.size() + 2;
    while (content.size() + QByteArray::number(size).size() + 1 != size)
        size = content.size() + QByteArray::number(size).size() + 1;
    return QByteArray::number(size) + ' ' + content;
}
void fixTarChecksum(QByteArray &data, qsizetype offset) {
    data.replace(offset + 148, 8, QByteArray(8, ' '));
    quint64 sum = 0;
    for (qsizetype i = offset; i < offset + 512; ++i)
        sum += quint8(data[i]);
    data.replace(offset + 148, 8, QByteArray::number(sum, 8).rightJustified(6, '0') + '\0' + ' ');
}
struct TarMember {
    qsizetype header, size, end;
    char type;
};
QList<TarMember> tarMembers(const QByteArray &inner) {
    QList<TarMember> members;
    for (qsizetype pos = 0; pos + 512 <= inner.size() && inner.mid(pos, 512) != QByteArray(512, '\0');) {
        auto field = inner.mid(pos + 124, 12);
        field.replace('\0', ' ');
        bool ok = false;
        const auto length = field.trimmed().toLongLong(&ok, 8);
        check(ok && length >= 0);
        const auto end = pos + 512 + ((length + 511) / 512) * 512;
        check(end <= inner.size());
        members.append({pos, length, end, inner[pos + 156]});
        pos = end;
    }
    return members;
}
void replacePax(QByteArray &inner, const TarMember &member, const QByteArray &payload) {
    QByteArray replacement = inner.mid(member.header, 512);
    replacement.replace(124, 12, QByteArray::number(payload.size(), 8).rightJustified(11, '0') + '\0');
    fixTarChecksum(replacement, 0);
    replacement += payload;
    replacement += QByteArray((512 - payload.size() % 512) % 512, '\0');
    inner.replace(member.header, member.end - member.header, replacement);
}
} // namespace

void filesystemAdversarialTests(cv::test::Suite &suite) {
    using Mutation = std::function<void(QJsonObject &)>;
    struct ManifestCase {
        QString name, code;
        Mutation mutate;
    };
    const QList<ManifestCase> malformed{
        {"unsupported-version", "MANIFEST_VERSION", [](auto &m) { m["version"] = 99; }},
        {"entries-not-array", "MANIFEST_VERSION", [](auto &m) { m["entries"] = "broken"; }},
        {"non-object-entry", "MANIFEST_ENTRY",
         [](auto &m) {
             auto a = m["entries"].toArray();
             a.append(7);
             m["entries"] = a;
         }},
        {"negative-file-size", "MANIFEST_SIZE",
         [](auto &m) { changeEntry(m, "source/a.txt", [](auto &e) { e["size"] = -1; }); }},
        {"bad-filetime", "METADATA_TIME",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 auto x = e["metadata"].toObject();
                 x["created"] = "NaN";
                 e["metadata"] = x;
             });
         }},
        {"missing-main-stream", "STREAM_MISSING",
         [](auto &m) { changeEntry(m, "source/a.txt", [](auto &e) { e["streams"] = QJsonArray{}; }); }},
        {"main-stream-size-mismatch", "STREAM_SIZE",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeStream(e, [](auto &s) { s["size"] = 999; }); });
         }},
        {"invalid-stream-name", "STREAM_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeStream(e, [](auto &s) { s["name"] = ":../escape:$DATA"; }); });
         }},
        {"duplicate-stream", "STREAM_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 auto a = e["streams"].toArray();
                 a.append(a[0]);
                 e["streams"] = a;
             });
         }},
        {"directory-main-stream", "STREAM_TYPE",
         [](auto &m) { changeEntry(m, "source/a.txt", [](auto &e) { e["type"] = "directory"; }); }},
        {"empty-chunk-id", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) { changeChunk(e, [](auto &c) { c["id"] = ""; }); });
         }},
        {"oversized-chunk-id", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeChunk(e, [](auto &c) { c["id"] = QString(257, 'a'); }); });
         }},
        {"invalid-chunk-digest", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeChunk(e, [](auto &c) { c["sha256"] = "ABC"; }); });
         }},
        {"negative-chunk-offset", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeChunk(e, [](auto &c) { c["offset"] = -1; }); });
         }},
        {"zero-chunk-length", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeChunk(e, [](auto &c) { c["length"] = 0; }); });
         }},
        {"chunk-over-one-mib", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 changeChunk(e, [](auto &c) { c["length"] = qint64(cv::ChunkSize + 1); });
             });
         }},
        {"overlapping-chunks", "CHUNK_INVALID",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 changeStream(e, [](auto &s) {
                     auto a = s["chunks"].toArray();
                     a.append(a[0]);
                     s["chunks"] = a;
                 });
             });
         }},
        {"non-sparse-gap", "CHUNK_GAP",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 changeChunk(e, [](auto &c) {
                     c["offset"] = 1;
                     c["length"] = 1;
                 });
             });
         }},
        {"non-sparse-tail-missing", "CHUNK_MISSING",
         [](auto &m) {
             changeEntry(m, "source/a.txt",
                         [](auto &e) { changeChunk(e, [](auto &c) { c["length"] = 1; }); });
         }},
        {"parent-not-directory", "PARENT_INVALID",
         [](auto &m) {
             auto a = m["entries"].toArray();
             for (qsizetype i = 0; i < a.size(); ++i)
                 if (a[i].toObject()["path"] == "source") {
                     a.removeAt(i);
                     break;
                 }
             m["entries"] = a;
         }},
        {"hardlink-missing-target", "HARDLINK_TARGET",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 e["type"] = "hardlink";
                 e["streams"] = QJsonArray{};
                 e["target"] = "source/missing";
             });
         }},
        {"symlink-empty-target", "LINK_TARGET",
         [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 e["type"] = "symlink";
                 e["streams"] = QJsonArray{};
                 e["target"] = "";
             });
         }},
        {"junction-short-reparse", "JUNCTION_INVALID", [](auto &m) {
             changeEntry(m, "source/a.txt", [](auto &e) {
                 e["type"] = "junction";
                 e["streams"] = QJsonArray{};
                 e["target"] = "C:/data";
                 e["reparseBase64"] = "AA==";
             });
         }}};
    for (const auto &test : malformed)
        suite.run("fs.adversarial.manifest." + test.name, "安全", [test] {
            Fixture f;
            auto m = f.capture();
            test.mutate(m);
            errorCode(test.code, [&] { f.restore(m); });
            check(!QFileInfo::exists(f.output()),
                  "malformed manifests must fail before destination creation");
        });

    suite.run("fs.restore.unknown-selection-no-output", "安全", [] {
        Fixture f;
        const auto m = f.capture();
        cv::RestoreOptions o;
        o.selectedPaths = {"missing"};
        errorCode("SELECTION_EMPTY", [&] { f.restore(m, o); });
        check(!QFileInfo::exists(f.output()));
    });
    suite.run("fs.restore.file-directory-type-conflict", "安全", [] {
        Fixture f;
        const auto m = f.capture();
        check(QDir().mkpath(f.output() + "/source/a.txt"));
        cv::atomicWrite(f.output() + "/source/a.txt/keep", "keep");
        cv::RestoreOptions o;
        o.overwrite = true;
        errorCode("TYPE_CONFLICT", [&] { f.restore(m, o); });
        check(cv::readFile(f.output() + "/source/a.txt/keep") == "keep");
    });
    suite.run("fs.restore.existing-directory-ads-protected", "安全", [] {
        Fixture f;
        QFile ads(f.source + "/empty:origin:$DATA");
        check(ads.open(QIODevice::WriteOnly));
        check(ads.write("new") == 3);
        ads.close();
        const auto m = f.capture();
        check(QDir().mkpath(f.output() + "/source/empty"));
        QFile old(f.output() + "/source/empty:origin:$DATA");
        check(old.open(QIODevice::WriteOnly));
        check(old.write("old") == 3);
        old.close();
        cv::RestoreOptions o;
        o.overwrite = true;
        errorCode("DIRECTORY_ADS_CONFLICT", [&] { f.restore(m, o); });
        check(cv::readFile(f.output() + "/source/empty:origin:$DATA") == "old");
    });
    suite.run("fs.restore.chunk-changes-after-preflight", "异常健壮性", [] {
        Fixture f;
        const auto m = f.capture();
        check(QDir().mkpath(f.output() + "/source"));
        cv::atomicWrite(f.output() + "/source/a.txt", "old");
        cv::RestoreOptions o;
        o.overwrite = true;
        o.selectedPaths = {"source/a.txt"};
        int reads = 0;
        errorCode("CHUNK_CORRUPT", [&] {
            cv::fs::restore(
                m,
                [&](const QString &id) {
                    auto b = f.chunks[id];
                    if (++reads > 1)
                        b[0] ^= 1;
                    return b;
                },
                f.output(), o);
        });
        check(reads == 2 && cv::readFile(f.output() + "/source/a.txt") == "old");
        check(
            QDir(f.output() + "/source").entryList({".cv-restore-*"}, QDir::Files | QDir::Hidden).isEmpty());
    });
    suite.run("fs.restore.cancel-after-preflight", "异常健壮性", [] {
        Fixture f;
        const auto m = f.capture();
        std::atomic_bool stop{false};
        cv::Context ctx;
        ctx.cancelled = &stop;
        cv::RestoreOptions o;
        o.selectedPaths = {"source/a.txt"};
        errorCode("CANCELLED", [&] {
            cv::fs::restore(
                m,
                [&](const QString &id) {
                    stop = true;
                    return f.chunks[id];
                },
                f.output(), o, ctx);
        });
        check(!QFileInfo::exists(f.output() + "/source/a.txt"));
    });
    suite.run("fs.capture.no-source", "异常健壮性", [] {
        Fixture f;
        f.plan.sources.clear();
        errorCode("NO_SOURCE", [&] { f.capture(); });
    });
    suite.run("fs.capture.colliding-source-roots", "安全", [] {
        Fixture f;
        check(QDir().mkpath(f.directory.path() + "/other/source"));
        f.plan.sources << f.directory.path() + "/other/source";
        errorCode("ROOT_COLLISION", [&] { f.capture(); });
    });
    suite.run("fs.capture.sink-empty-id", "异常健壮性", [] {
        Fixture f;
        errorCode("CHUNK_ID", [&] { cv::fs::capture(f.plan, [](const QByteArray &) { return QString{}; }); });
    });
    suite.run("fs.roundtrip.path-longer-than-max-path", "系统功能", [] {
        Fixture f;
        QString relative = "source";
        for (int i = 0; i < 5; ++i)
            relative += '/' + QString(60, QChar('a' + i));
        const auto parent = f.directory.path() + '/' + relative;
        check(QDir().mkpath(parent));
        cv::atomicWrite(parent + "/中文-long.txt", "long path payload");
        const auto m = f.capture();
        f.restore(m);
        check((f.output() + '/' + relative + "/中文-long.txt").size() > 260);
        check(cv::readFile(f.output() + '/' + relative + "/中文-long.txt") == "long path payload");
    });
    suite.run("fs.filter.string-name-exclusion", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"nameInclude", "*.txt"}, {"nameExclude", "a.*"}};
        check(f.capture()["fileCount"].toInteger() == 0);
    });
    suite.run("fs.filter.attribute-exclusion", "白盒单元", [] {
        Fixture f;
        check(SetFileAttributesW(win(f.source + "/a.txt").c_str(), FILE_ATTRIBUTE_HIDDEN));
        f.plan.filters = {{"types", "file"}, {"attributesNone", qint64(FILE_ATTRIBUTE_HIDDEN)}};
        check(f.capture()["fileCount"].toInteger() == 1);
    });
    suite.run("fs.filter.time-upper-past", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"timeField", "created"}, {"timeTo", "2000-01-01T00:00:00Z"}};
        check(f.capture()["fileCount"].toInteger() == 0);
    });
    suite.run("fs.filter.invalid-time-field", "异常健壮性", [] {
        Fixture f;
        f.plan.filters = {{"timeField", "bogus"}, {"timeFrom", "2000-01-01T00:00:00Z"}};
        errorCode("FILTER_TIME_FIELD", [&] { f.capture(); });
    });
    suite.run("fs.filter.valid-time-fields", "白盒单元", [] {
        Fixture f;
        for (const auto *field : {"created", "accessed", "modified"}) {
            f.plan.filters = {{"timeField", field}, {"timeFrom", "2000-01-01T00:00:00Z"}};
            check(f.capture()["fileCount"].toInteger() > 0);
        }
    });
    suite.run("fs.filter.content-too-long", "异常健壮性", [] {
        Fixture f;
        f.plan.filters = {{"contentContains", QString(cv::ChunkSize + 1, 'x')}};
        errorCode("FILTER_SIZE", [&] { f.capture(); });
    });
    suite.run("fs.ntfs.selected-hardlink-brings-canonical-data", "系统功能", [] {
        Fixture f;
        check(CreateHardLinkW(win(f.source + "/z-link.txt").c_str(), win(f.source + "/a.txt").c_str(),
                              nullptr));
        const auto m = f.capture();
        QString alias, canonical;
        for (const auto &v : m["entries"].toArray())
            if (v.toObject()["type"] == "hardlink") {
                alias = v.toObject()["path"].toString();
                canonical = v.toObject()["target"].toString();
            }
        check(!alias.isEmpty());
        cv::RestoreOptions o;
        o.selectedPaths = {alias};
        f.restore(m, o);
        check(cv::readFile(f.output() + '/' + alias) == "alpha file bytes" &&
              cv::readFile(f.output() + '/' + canonical) == "alpha file bytes");
        check(!QFileInfo::exists(f.output() + "/source/b.bin"));
        cv::atomicWrite(f.output() + "/mutated", "replacement");
        QFile aliasFile(f.output() + '/' + alias);
        check(aliasFile.open(QIODevice::WriteOnly));
        check(aliasFile.write("shared") == 6);
        aliasFile.close();
        check(cv::readFile(f.output() + '/' + canonical) == "shared");
    });
    suite.run("fs.ntfs.empty-and-all-hole-sparse", "系统功能", [] {
        Fixture f;
        sparseFile(f.source + "/holes", cv::ChunkSize * 2, 0, {});
        sparseFile(f.source + "/zero-sparse", 0, 0, {});
        const auto m = f.capture();
        f.restore(m);
        QFile file(f.output() + "/source/holes");
        check(file.open(QIODevice::ReadOnly));
        check(file.size() == cv::ChunkSize * 2 && file.readAll() == QByteArray(cv::ChunkSize * 2, '\0'));
        check(GetFileAttributesW(win(f.output() + "/source/holes").c_str()) & FILE_ATTRIBUTE_SPARSE_FILE);
    });
    struct JunctionCase {
        QString name, code;
        std::function<void(QJsonObject &)> mutate;
    };
    const QList<JunctionCase> junctionCases{
        {"display-target-mismatch", "JUNCTION_TARGET", [](auto &e) { e["target"] = "C:\\unrelated"; }},
        {"raw-size-mismatch", "REPARSE_INVALID",
         [](auto &e) {
             auto data = QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1());
             data[4] ^= 1;
             e["reparseBase64"] = QString::fromLatin1(data.toBase64());
         }},
        {"odd-substitute-offset", "REPARSE_INVALID",
         [](auto &e) {
             auto data = QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1());
             data.replace(8, 2, integer(1, 2));
             e["reparseBase64"] = QString::fromLatin1(data.toBase64());
         }},
        {"unknown-reparse-tag", "REPARSE_UNSUPPORTED", [](auto &e) {
             auto data = QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1());
             data.replace(0, 4, integer(0x80000013, 4));
             e["reparseBase64"] = QString::fromLatin1(data.toBase64());
         }}};
    for (const auto &test : junctionCases)
        suite.run("fs.adversarial.junction." + test.name, "安全", [test] {
            Fixture f;
            junction(f.source + "/junction", f.source + "/empty");
            auto manifest = f.capture();
            changeEntry(manifest, "source/junction", test.mutate);
            errorCode(test.code, [&] { f.restore(manifest); });
            check(!QFileInfo::exists(f.output()));
        });
    suite.run("fs.ntfs.junction-print-name-is-not-target-authority", "安全", [] {
        Fixture f;
        junction(f.source + "/junction", f.source + "/empty");
        auto manifest = f.capture();
        QString actualTarget;
        changeEntry(manifest, "source/junction", [&](auto &e) {
            actualTarget = e["target"].toString();
            auto data = QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1());
            const auto printOffset = qsizetype(uintAt(data, 12, 2));
            check(uintAt(data, 14, 2) > 0);
            data[16 + printOffset] = 'X';
            e["reparseBase64"] = QString::fromLatin1(data.toBase64());
        });
        f.restore(manifest);
        f.plan.sources = {f.output() + "/source"};
        auto recaptured = f.capture();
        bool observed = false;
        changeEntry(recaptured, "source/junction", [&](auto &e) {
            observed = true;
            check(e["target"].toString() == actualTarget,
                  "capture must report the kernel's substitute target");
        });
        check(observed);
    });
    for (const auto &field : QStringList{"ownerSid", "groupSid", "daclBase64"})
        suite.run("fs.metadata.invalid-" + field + "-preserves-file", "安全", [field] {
            Fixture f;
            auto m = f.capture();
            changeEntry(m, "source/a.txt", [&](auto &e) {
                auto meta = e["metadata"].toObject();
                meta[field] = "invalid";
                e["metadata"] = meta;
            });
            check(QDir().mkpath(f.output() + "/source"));
            cv::atomicWrite(f.output() + "/source/a.txt", "unchanged");
            cv::RestoreOptions options;
            options.overwrite = true;
            options.selectedPaths = {"source/a.txt"};
            errorCode(field == "daclBase64" ? "ACL_INVALID" : "SID_INVALID", [&] { f.restore(m, options); });
            check(cv::readFile(f.output() + "/source/a.txt") == "unchanged");
            check(QDir(f.output() + "/source")
                      .entryList({".cv-restore-*"}, QDir::Files | QDir::Hidden)
                      .isEmpty());
        });
    suite.run("fs.metadata.null-dacl-roundtrip", "系统功能", [] {
        Fixture f;
        auto m = f.capture();
        changeEntry(m, "source/a.txt", [](auto &e) {
            auto meta = e["metadata"].toObject();
            meta["daclNull"] = true;
            meta.remove("daclBase64");
            e["metadata"] = meta;
        });
        f.restore(m);
        f.plan.sources = {f.output() + "/source"};
        const auto after = f.capture();
        bool found = false;
        for (const auto &v : after["entries"].toArray())
            if (v.toObject()["path"] == "source/a.txt") {
                found = true;
                check(v.toObject()["metadata"].toObject()["daclNull"].toBool());
            }
        check(found && after["warnings"].toArray().isEmpty());
    });
    for (bool strict : {true, false})
        suite.run(QString("fs.metadata.foreign-owner-") + (strict ? "strict" : "warning"), "安全", [strict] {
            Fixture f;
            auto m = f.capture();
            changeEntry(m, "source/a.txt", [](auto &e) {
                auto meta = e["metadata"].toObject();
                meta["ownerSid"] = "S-1-5-18";
                e["metadata"] = meta;
            });
            cv::RestoreOptions options;
            options.strictMetadata = strict;
            options.selectedPaths = {"source/a.txt"};
            restrictedToken([&] {
                if (strict)
                    errorCode("METADATA_PERMISSION", [&] { f.restore(m, options); });
                else {
                    const auto result = f.restore(m, options);
                    check(result.success && !result.warnings.isEmpty());
                }
            });
            check(QFileInfo::exists(f.output() + "/source/a.txt") == !strict);
        });
    suite.run("fs.filter.hardlink-type-selects-both-names", "白盒单元", [] {
        Fixture f;
        check(CreateHardLinkW(win(f.source + "/z-link.txt").c_str(), win(f.source + "/a.txt").c_str(),
                              nullptr));
        f.plan.filters = {{"types", "hardlink"}, {"extensions", "txt"}};
        const auto m = f.capture();
        check(m["fileCount"].toInteger() == 2);
        f.restore(m);
        check(!QFileInfo::exists(f.output() + "/source/b.bin"));
    });

    struct BinaryCase {
        QString name, code;
        std::function<void(QByteArray &)> mutate;
    };
    const QList<BinaryCase> indexCases{
        {"magic", "INDEX_MAGIC", [](auto &b) { b[0] = 'X'; }},
        {"manifest-too-large", "MANIFEST_SIZE",
         [](auto &b) { b.replace(8, 8, integer(64 * 1024 * 1024 + 1, 8)); }},
        {"manifest-json-array", "ARCHIVE_JSON",
         [](auto &b) {
             const auto n = uintAt(b, 8, 8);
             b.replace(16, n, "[]");
             b.replace(8, 8, integer(2, 8));
         }},
        {"chunk-count-limit", "CHUNK_COUNT",
         [](auto &b) {
             const auto at = 16 + uintAt(b, 8, 8);
             b.replace(at, 8, integer(10000001, 8));
         }},
        {"chunk-id-syntax", "INDEX_INVALID", [](auto &b) { b[24 + uintAt(b, 8, 8)] = 'z'; }},
        {"chunk-zero-length", "INDEX_INVALID",
         [](auto &b) { b.replace(24 + uintAt(b, 8, 8) + 64, 4, integer(0, 4)); }},
        {"chunk-size-limit", "INDEX_INVALID",
         [](auto &b) { b.replace(24 + uintAt(b, 8, 8) + 64, 4, integer(cv::ChunkSize + 1, 4)); }},
        {"trailing-bytes", "INDEX_TRAILING", [](auto &b) { b += 'x'; }},
        {"duplicate-block-id", "INDEX_INVALID",
         [](auto &b) {
             const auto at = 24 + uintAt(b, 8, 8);
             const auto second = at + 68 + uintAt(b, at + 64, 4);
             b.replace(second, 64, b.mid(at, 64));
         }},
        {"missing-index-block", "INDEX_MISMATCH",
         [](auto &b) {
             const auto countAt = 16 + uintAt(b, 8, 8);
             b.truncate(countAt + 8);
             b.replace(countAt, 8, integer(0, 8));
         }},
        {"unknown-index-block", "CHUNK_MISSING",
         [](auto &b) {
             const auto at = 24 + uintAt(b, 8, 8);
             b.replace(at, 64, QByteArray(64, '0'));
         }},
        {"block-content-corrupt", "CHUNK_CHECKSUM", [](auto &b) {
             const auto at = 24 + uintAt(b, 8, 8);
             b[at + 68] ^= 1;
         }}};
    for (const auto &test : indexCases)
        suite.run("archive.adversarial.index." + test.name, "安全", [test] {
            Fixture f;
            const auto m = f.capture();
            auto inner = makeIndex(m, f.chunks);
            test.mutate(inner);
            cv::atomicWrite(f.plan.destination, frame(inner));
            errorCode(test.code, [&] { cv::archive::restore(f.plan.destination, f.output()); });
            check(!QFileInfo::exists(f.output()));
        });

    const QList<BinaryCase> tarCases{
        {"bad-checksum", "TAR_CHECKSUM", [](auto &b) { b[0] ^= 1; }},
        {"bad-octal", "TAR_NUMBER", [](auto &b) { b[148] = '9'; }},
        {"bad-pax-length", "PAX_LENGTH", [](auto &b) { b[512] = 'x'; }},
        {"bad-pax-newline", "PAX_LENGTH",
         [](auto &b) {
             const auto m = tarMembers(b);
             b[512 + m[0].size - 1] = 'x';
         }},
        {"missing-pax-key", "PAX_KEY",
         [](auto &b) {
             auto m = tarMembers(b);
             replacePax(b, m[0], QByteArray("8 abcde\n"));
         }},
        {"duplicate-pax-key", "PAX_DUPLICATE",
         [](auto &b) {
             auto m = tarMembers(b);
             replacePax(b, m[0], b.mid(512, m[0].size) + paxRecord("path", "again"));
         }},
        {"consecutive-pax", "PAX_SIZE",
         [](auto &b) {
             auto m = tarMembers(b);
             b.insert(m[0].end, b.left(m[0].end));
         }},
        {"missing-pax-header", "PAX_MISSING",
         [](auto &b) {
             auto m = tarMembers(b);
             b.remove(0, m[0].end);
         }},
        {"manifest-not-first", "TAR_MANIFEST",
         [](auto &b) {
             auto m = tarMembers(b);
             b.remove(0, m[1].end);
         }},
        {"manifest-duplicate", "TAR_MANIFEST",
         [](auto &b) {
             auto m = tarMembers(b);
             b.insert(m[1].end, b.left(m[1].end));
         }},
        {"member-unregistered", "TAR_UNEXPECTED",
         [](auto &b) {
             auto m = tarMembers(b);
             replacePax(b, m[2], paxRecord("path", "unknown") + paxRecord("size", "0"));
         }},
        {"member-type-mismatch", "TAR_TYPE",
         [](auto &b) {
             auto m = tarMembers(b);
             b[m[3].header + 156] = '2';
             fixTarChecksum(b, m[3].header);
         }},
        {"member-duplicate", "TAR_DUPLICATE",
         [](auto &b) {
             auto m = tarMembers(b);
             b.insert(m[3].end, b.mid(m[2].header, m[3].end - m[2].header));
         }},
        {"member-missing", "TAR_MISSING",
         [](auto &b) {
             auto m = tarMembers(b);
             b.remove(m[2].header, m[3].end - m[2].header);
         }},
        {"manifest-padding-nonzero", "TAR_PADDING",
         [](auto &b) {
             auto m = tarMembers(b);
             check(m[1].size % 512);
             b[m[1].header + 512 + m[1].size] = 'x';
         }},
        {"pax-padding-nonzero", "TAR_PADDING",
         [](auto &b) {
             auto m = tarMembers(b);
             check(m[0].size % 512);
             b[m[0].header + 512 + m[0].size] = 'x';
         }},
        {"one-terminator-only", "TAR_TERMINATOR", [](auto &b) { b.chop(512); }},
        {"terminator-then-member", "TAR_TERMINATOR", [](auto &b) { b.insert(0, QByteArray(512, '\0')); }},
        {"trailing-after-terminator", "TAR_TERMINATOR", [](auto &b) { b.append(QByteArray(512, '\0')); }}};
    for (const auto &test : tarCases)
        suite.run("archive.adversarial.pax." + test.name, "安全", [test] {
            Fixture f;
            f.plan.format = "tar";
            cv::archive::backup(f.plan);
            auto inner = innerBytes(cv::readFile(f.plan.destination));
            test.mutate(inner);
            cv::atomicWrite(f.plan.destination, frame(inner, "tar"));
            errorCode(test.code, [&] { cv::archive::restore(f.plan.destination, f.output()); });
            check(!QFileInfo::exists(f.output()));
        });

    struct FrameCase {
        QString name, code;
        std::function<void(QList<QByteArray> &)> mutate;
    };
    const QList<FrameCase> frameCases{
        {"sequence", "ARCHIVE_SEQUENCE", [](auto &r) { r[0].replace(0, 8, integer(7, 8)); }},
        {"header-binding", "ARCHIVE_SEQUENCE", [](auto &r) { r[0][9] ^= 1; }},
        {"record-kind", "ARCHIVE_RECORD", [](auto &r) { r[0][8] = 9; }},
        {"record-short-body", "ARCHIVE_RECORD", [](auto &r) { r[0].truncate(42); }},
        {"decoded-size-limit", "BLOCK_SIZE",
         [](auto &r) { r[0].replace(41, 4, integer(cv::ChunkSize + 1, 4)); }},
        {"decoded-digest", "ARCHIVE_CHECKSUM", [](auto &r) { r[0][45] ^= 1; }},
        {"decoded-size-mismatch", "ARCHIVE_CHECKSUM", [](auto &r) { r[0].replace(41, 4, integer(0, 4)); }},
        {"final-length", "ARCHIVE_FINAL", [](auto &r) { r.last()[41] ^= 1; }},
        {"final-digest", "ARCHIVE_FINAL", [](auto &r) { r.last()[49] ^= 1; }},
        {"missing-final", "ARCHIVE_TRUNCATED", [](auto &r) { r.removeLast(); }},
        {"duplicate-final", "ARCHIVE_FINAL", [](auto &r) { r.append(r.last()); }}};
    for (const auto &test : frameCases)
        suite.run("archive.adversarial.frame." + test.name, "安全", [test] {
            Fixture f;
            const auto m = f.capture();
            cv::atomicWrite(f.plan.destination, frame(makeIndex(m, f.chunks), "indexed", {}, test.mutate));
            errorCode(test.code, [&] { cv::archive::restore(f.plan.destination, f.output()); });
            check(!QFileInfo::exists(f.output()));
        });
    for (const auto &item : QList<QPair<QString, QJsonObject>>{
             {"ARCHIVE_VERSION", {{"version", 3}}},
             {"ARCHIVE_FORMAT", {{"format", "zip"}}},
             {"CIPHER_UNSUPPORTED", {{"cipher", "xor"}}},
             {"KDF_PARAMETERS", {{"cipher", "aes256"}, {"salt", "AA=="}, {"iterations", 1}}}})
        suite.run("archive.adversarial.header." + item.first, "安全", [item] {
            Fixture f;
            const auto m = f.capture();
            cv::atomicWrite(f.plan.destination, frame(makeIndex(m, f.chunks), "indexed", item.second));
            errorCode(item.first, [&] { cv::archive::restore(f.plan.destination, f.output(), "password"); });
            check(!QFileInfo::exists(f.output()));
        });

    suite.run("archive.pax-alias-ntfs-streams-hardlinks-sparse", "集成", [] {
        Fixture f;
        f.plan.format = "pax";
        check(
            CreateHardLinkW(win(f.source + "/link.txt").c_str(), win(f.source + "/a.txt").c_str(), nullptr));
        junction(f.source + "/junction", f.source + "/empty");
        QFile ads(f.source + "/empty:origin:$DATA");
        check(ads.open(QIODevice::WriteOnly));
        check(ads.write("ADS bytes") == 9);
        ads.close();
        sparseFile(f.source + "/sparse", cv::ChunkSize * 2 + 17, cv::ChunkSize, "TAIL");
        cv::archive::backup(f.plan);
        check(cv::archive::inspect(f.plan.destination)["archiveFormat"] == "tar");
        cv::archive::restore(f.plan.destination, f.output());
        check(cv::readFile(f.output() + "/source/empty:origin:$DATA") == "ADS bytes");
        check(cv::readFile(f.output() + "/source/link.txt") == "alpha file bytes");
        check(GetFileAttributesW(win(f.output() + "/source/junction").c_str()) &
              FILE_ATTRIBUTE_REPARSE_POINT);
        check(GetFileAttributesW(win(f.output() + "/source/sparse").c_str()) & FILE_ATTRIBUTE_SPARSE_FILE);
        QFile sparse(f.output() + "/source/sparse");
        check(sparse.open(QIODevice::ReadOnly));
        check(sparse.seek(cv::ChunkSize));
        check(sparse.read(4) == "TAIL");
    });
    suite.run("archive.pax-link-target-mismatch", "安全", [] {
        Fixture f;
        f.plan.format = "tar";
        check(
            CreateHardLinkW(win(f.source + "/link.txt").c_str(), win(f.source + "/a.txt").c_str(), nullptr));
        cv::archive::backup(f.plan);
        auto inner = innerBytes(cv::readFile(f.plan.destination));
        const auto members = tarMembers(inner);
        bool changed = false;
        for (qsizetype i = 1; i < members.size(); ++i)
            if (members[i].type == '1') {
                const auto before = inner.mid(members[i - 1].header + 512, members[i - 1].size);
                const auto marker = before.indexOf("linkpath=");
                check(marker >= 0);
                auto after = before;
                after[marker + 9] ^= 1;
                replacePax(inner, members[i - 1], after);
                changed = true;
                break;
            }
        check(changed);
        cv::atomicWrite(f.plan.destination, frame(inner, "tar"));
        errorCode("TAR_LINK", [&] { cv::archive::restore(f.plan.destination, f.output()); });
        check(!QFileInfo::exists(f.output()));
    });
    suite.run("archive.cipher-alias-and-missing-password", "集成", [] {
        Fixture f;
        f.plan.cipher = "AES-256";
        errorCode("PASSWORD_REQUIRED", [&] { cv::archive::backup(f.plan); });
        check(!QFileInfo::exists(f.plan.destination));
        f.plan.password = "alias password";
        cv::archive::backup(f.plan);
        errorCode("PASSWORD_REQUIRED", [&] { cv::archive::inspect(f.plan.destination); });
        check(cv::archive::verify(f.plan.destination, f.plan.password).success);
    });
    suite.run("archive.cancel-during-wrap-preserves-existing", "异常健壮性", [] {
        Fixture f;
        cv::atomicWrite(f.plan.destination, "old archive bytes");
        std::atomic_bool cancelled{false};
        cv::Context ctx;
        ctx.cancelled = &cancelled;
        ctx.progress = [&](quint64, quint64, const QString &stage) {
            if (stage == "打包 / 压缩 / 加密")
                cancelled = true;
        };
        errorCode("CANCELLED", [&] { cv::archive::backup(f.plan, ctx); });
        check(cv::readFile(f.plan.destination) == "old archive bytes");
    });
    const QString smile = QString::fromUcs4(U"🙂.txt");
    const QStringList extraNames{"alpha.cpp", "beta.hpp", "A7.TXT",     "a-.txt", "[note].txt", "].txt",
                                 "école.txt", "中文.txt", "aaaaab.txt", smile,    "ß.txt",      "İ.txt"};
    const QStringList textNames{"a.txt",    "A7.TXT",     "a-.txt", "[note].txt", "].txt", "école.txt",
                                "中文.txt", "aaaaab.txt", smile,    "ß.txt",      "İ.txt"};
    struct GlobCase {
        QString name, pattern;
        QStringList expected;
    };
    const QList<GlobCase> globCases{
        {"star", "*.txt", textNames},
        {"one-codepoint", "?.txt", {"a.txt", "].txt", smile, "ß.txt", "İ.txt"}},
        {"two-codepoints", "??.txt", {"A7.TXT", "a-.txt", "中文.txt"}},
        {"ascii-case-fold", "A?.tXt", {"A7.TXT", "a-.txt"}},
        {"unicode-case-fold", "ÉCOLE.TXT", {"école.txt"}},
        {"simple-fold-sharp-s", "ẞ.txt", {"ß.txt"}},
        {"simple-fold-dotted-I", "İ.txt", {"İ.txt"}},
        {"fold-does-not-expand-points", "SS.txt", {}},
        {"digit-range", "a[0-9].txt", {"A7.TXT"}},
        {"character-set", "[ab].*", {"a.txt", "b.bin"}},
        {"negated-set",
         "[!a]*.txt",
         {"[note].txt", "].txt", "école.txt", "中文.txt", smile, "ß.txt", "İ.txt"}},
        {"caret-negated-set",
         "[^a]*.txt",
         {"[note].txt", "].txt", "école.txt", "中文.txt", smile, "ß.txt", "İ.txt"}},
        {"leading-hyphen", "a[-7].txt", {"a-.txt", "A7.TXT"}},
        {"trailing-hyphen", "a[7-].txt", {"a-.txt", "A7.TXT"}},
        {"literal-opening-bracket", "[[]note].txt", {"[note].txt"}},
        {"literal-closing-bracket", "[]].txt", {"].txt"}},
        {"closing-bracket-in-set", "[]a].txt", {"].txt", "a.txt"}},
        {"collapsed-stars", "**a**.cpp", {"alpha.cpp"}},
        {"multiple-star-segments", "*b*e*t*a*.hpp", {"beta.hpp"}},
        {"star-retry", "*ab.txt", {"aaaaab.txt"}},
        {"multiple-retries", "*a*a*a*.txt", {"aaaaab.txt"}},
        {"tail-star-zero-points", "*.txt*", textNames},
        {"empty-pattern", "", {}},
        {"whole-value-anchor", "alpha", {}},
        {"letter-range", "[A-Z]*.cpp", {"alpha.cpp"}},
        {"unicode-range", "[中-文]*.txt", {"中文.txt"}},
        {"all-values", "*", QStringList{"a.txt", "b.bin"} + extraNames}};
    for (const auto &test : globCases)
        suite.run("fs.filter.glob." + test.name, "白盒单元", [test, extraNames] {
            Fixture f;
            for (const auto &name : extraNames)
                cv::atomicWrite(f.source + '/' + name, "wildcard fixture");
            f.plan.filters = {{"nameInclude", test.pattern}};
            QStringList selected;
            for (const auto &value : cv::fs::preview(f.plan)) {
                const auto e = value.toObject();
                if (e["type"] == "file" && e["selected"].toBool())
                    selected.append(e["path"].toString().section('/', -1));
            }
            auto expected = test.expected;
            selected.sort();
            expected.sort();
            check(selected == expected,
                  "selected=" + selected.join(',') + "; expected=" + expected.join(','));
            check(f.capture()["fileCount"].toInteger() == selected.size(), "preview and capture must agree");
        });
    for (const auto &pattern : QStringList{"[", "[]", "[!]", "[a", "[z-a]", "[^]"})
        suite.run("fs.filter.glob.invalid-" + pattern, "异常健壮性", [pattern] {
            Fixture f;
            // An earlier valid alternative and an excluded root must not hide
            // a malformed second rule: configuration is validated before scan.
            f.plan.filters = {{"nameInclude", QJsonArray{"*", pattern}}, {"pathExclude", "source"}};
            errorCode("FILTER_PATTERN", [&] { cv::fs::preview(f.plan); });
            check(!QFileInfo::exists(f.plan.destination));
        });
    suite.run("fs.filter.glob.literal-directory-boundary", "白盒单元", [] {
        Fixture f;
        check(QDir().mkpath(f.source + "/empty-other"));
        cv::atomicWrite(f.source + "/empty/file.txt", "inside");
        cv::atomicWrite(f.source + "/empty-other/file.txt", "outside");
        f.plan.filters = {{"pathInclude", "SOURCE\\EMPTY"}};
        const auto manifest = f.capture();
        check(manifest["fileCount"].toInteger() == 1);
        f.restore(manifest);
        check(cv::readFile(f.output() + "/source/empty/file.txt") == "inside");
        check(!QFileInfo::exists(f.output() + "/source/empty-other"));
    });
    suite.run("fs.filter.glob.star-crosses-path-separator", "白盒单元", [] {
        Fixture f;
        check(QDir().mkpath(f.source + "/empty/deep"));
        cv::atomicWrite(f.source + "/empty/deep/only.txt", "deep");
        f.plan.filters = {{"pathInclude", "source/*only.txt"}};
        const auto manifest = f.capture();
        check(manifest["fileCount"].toInteger() == 1);
        check(manifest["entries"].toArray().size() == 4);
    });
    suite.run("fs.filter.glob.pattern-length-limit", "异常健壮性", [] {
        Fixture f;
        f.plan.filters = {{"nameInclude", QString(32001, 'a')}};
        errorCode("FILTER_PATTERN", [&] { cv::fs::preview(f.plan); });
    });
    suite.run("fs.filter.glob.explicit-work-budget", "异常健壮性", [] {
        Fixture f;
        QString longPath = f.source;
        for (int i = 0; i < 3; ++i)
            longPath += '/' + QString(200, 'a');
        check(QDir().mkpath(longPath));
        f.plan.filters = {{"pathInclude", "*[" + QString(31990, 'z') + "]"}};
        errorCode("FILTER_PATTERN_COMPLEXITY", [&] { cv::fs::preview(f.plan); });
        check(!QFileInfo::exists(f.plan.destination));
    });
}
