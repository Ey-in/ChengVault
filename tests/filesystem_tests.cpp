#include "chengvault/archive.hpp"
#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include "test_support.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <aclapi.h>
#include <cstring>
#include <windows.h>
#include <winioctl.h>

void filesystemAdversarialTests(cv::test::Suite &suite);

namespace {
using cv::test::check;
using cv::test::throws;
struct Fixture {
    QTemporaryDir temporary;
    QString source;
    QMap<QString, QByteArray> blocks;
    cv::BackupPlan plan;
    Fixture() {
        check(temporary.isValid());
        source = temporary.path() + "/source";
        check(QDir().mkpath(source + "/empty"));
        check(QDir().mkpath(source + "/nested"));
        cv::atomicWrite(source + "/hello.txt", "ChengVault original file\n");
        cv::atomicWrite(source + "/nested/中文.txt", QByteArray::fromHex("00ff0102030400"));
        cv::atomicWrite(source + "/zero.bin", {});
        plan.sources << source;
        plan.destination = temporary.path() + "/backup.cv";
        plan.compression = "none";
    }
    QJsonObject capture() {
        return cv::fs::capture(plan, [&](const QByteArray &data) {
            const auto id = QString::fromLatin1(cv::crypto::sha256(data).toHex());
            blocks[id] = data;
            return id;
        });
    }
    cv::TaskResult restore(const QJsonObject &m, const QString &name = "restored",
                           cv::RestoreOptions options = {}) {
        return cv::fs::restore(
            m,
            [&](const QString &id) {
                check(blocks.contains(id));
                return blocks[id];
            },
            temporary.path() + '/' + name, options);
    }
    QString restored(const QString &name = "restored") const {
        return temporary.path() + '/' + name + "/source";
    }
};
QJsonObject entry(const QJsonObject &m, const QString &name) {
    for (const auto &e : m["entries"].toArray())
        if (e.toObject()["path"].toString() == name)
            return e.toObject();
    throw std::runtime_error("manifest entry absent");
}
QStringList selectedFiles(const cv::BackupPlan &plan) {
    QStringList result;
    for (const auto &value : cv::fs::preview(plan)) {
        const auto e = value.toObject();
        if (e["selected"].toBool() && e["type"] == "file")
            result << e["path"].toString();
    }
    return result;
}
std::wstring win(const QString &p) {
    return QDir::toNativeSeparators(p).toStdWString();
}
quint64 fileId(const QString &path) {
    HANDLE h =
        CreateFileW(win(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    check(h != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    check(ok);
    return (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
bool makeJunction(const QString &path, const QString &target) {
    if (!QDir().mkdir(path))
        return false;
    const QString substitute = "\\??\\" + QDir::toNativeSeparators(target),
                  print = QDir::toNativeSeparators(target);
    const auto s = substitute.toStdWString(), p = print.toStdWString();
    QByteArray data(16 + qsizetype((s.size() + p.size() + 2) * sizeof(wchar_t)), '\0');
    auto put16 = [&](int at, quint16 n) {
        data[at] = char(n);
        data[at + 1] = char(n >> 8);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    for (int i = 0; i < 4; ++i)
        data[i] = char(tag >> (8 * i));
    put16(4, quint16(data.size() - 8));
    put16(8, 0);
    put16(10, quint16(s.size() * 2));
    put16(12, quint16((s.size() + 1) * 2));
    put16(14, quint16(p.size() * 2));
    std::memcpy(data.data() + 16, s.c_str(), (s.size() + 1) * 2);
    std::memcpy(data.data() + 16 + (s.size() + 1) * 2, p.c_str(), (p.size() + 1) * 2);
    HANDLE h = CreateFileW(win(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD count = 0;
    const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, data.data(), DWORD(data.size()), nullptr, 0,
                                    &count, nullptr);
    CloseHandle(h);
    return ok;
}
} // namespace

void filesystemTests(cv::test::Suite &suite) {
    filesystemAdversarialTests(suite);
    suite.run("fs.roundtrip.files-empty-unicode", "系统功能", [] {
        Fixture f;
        const auto m = f.capture();
        const auto result = f.restore(m);
        check(result.success && result.files == 3);
        check(QFileInfo(f.restored() + "/empty").isDir());
        check(cv::readFile(f.restored() + "/hello.txt") == cv::readFile(f.source + "/hello.txt"));
        check(cv::readFile(f.restored() + "/nested/中文.txt") == QByteArray::fromHex("00ff0102030400"));
        check(QFileInfo(f.restored() + "/zero.bin").size() == 0);
    });
    suite.run("fs.metadata.owner-group-times-dacl", "系统功能", [] {
        Fixture f;
        const auto m = f.capture();
        f.restore(m);
        cv::BackupPlan after;
        after.sources << f.restored();
        const auto restored = cv::fs::capture(
            after, [&](const QByteArray &b) { return QString::fromLatin1(cv::crypto::sha256(b).toHex()); });
        const auto expected = entry(m, "source/hello.txt")["metadata"].toObject();
        const auto actual = entry(restored, "source/hello.txt")["metadata"].toObject();
        for (const auto key : {"ownerSid", "groupSid", "created", "modified", "attributes", "daclBase64"})
            check(expected[key] == actual[key], key);
    });
    suite.run("fs.filter.path-exclusion-wins", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"pathInclude", QJsonArray{"source/*"}},
                          {"pathExclude", QJsonArray{"source/nested"}}};
        const auto files = selectedFiles(f.plan);
        check(files.size() == 2 && !files.contains("source/nested/中文.txt"));
    });
    suite.run("fs.filter.name-glob-ancestor", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"nameInclude", QJsonArray{"中文.*"}}};
        const auto m = f.capture();
        check(m["entries"].toArray().size() == 3);
        check(entry(m, "source")["type"] == "directory");
        check(selectedFiles(f.plan).size() == 1);
    });
    suite.run("fs.filter.object-type-and-extension", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"types", QJsonArray{"file"}}, {"extensions", QJsonArray{".TXT"}}};
        check(selectedFiles(f.plan).size() == 2);
    });
    suite.run("fs.filter.size-inclusive", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"minSize", 7}, {"maxSize", 7}};
        check(selectedFiles(f.plan) == QStringList{"source/nested/中文.txt"});
    });
    suite.run("fs.filter.owner-sid", "白盒单元", [] {
        Fixture f;
        const auto m = f.capture();
        const auto sid = entry(m, "source/hello.txt")["metadata"].toObject()["ownerSid"].toString();
        f.plan.filters = {{"owners", QJsonArray{sid}}};
        check(selectedFiles(f.plan).size() == 3);
        f.plan.filters = {{"owners", QJsonArray{"S-1-0-0"}}};
        check(selectedFiles(f.plan).isEmpty());
    });
    suite.run("fs.filter.owner-account-name", "白盒单元", [] {
        Fixture f;
        const auto m = f.capture();
        const auto account = entry(m, "source/hello.txt")["metadata"].toObject()["ownerAccount"].toString();
        check(!account.isEmpty());
        f.plan.filters = {{"owners", QJsonArray{account}}};
        check(selectedFiles(f.plan).size() == 3);
    });
    suite.run("fs.filter.time-bounds", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"timeFrom", QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}};
        check(selectedFiles(f.plan).isEmpty());
        f.plan.filters = {{"timeTo", QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}};
        check(selectedFiles(f.plan).size() == 3);
    });
    suite.run("fs.filter.invalid-time-rejected", "异常健壮性", [] {
        Fixture f;
        f.plan.filters = {{"timeFrom", "not-a-date"}};
        throws([&] { cv::fs::preview(f.plan); });
    });
    suite.run("fs.filter.depth", "白盒单元", [] {
        Fixture f;
        f.plan.filters = {{"maxDepth", 1}};
        check(selectedFiles(f.plan).size() == 2);
    });
    suite.run("fs.filter.attributes", "白盒单元", [] {
        Fixture f;
        check(SetFileAttributesW(win(f.source + "/hello.txt").c_str(), FILE_ATTRIBUTE_HIDDEN));
        f.plan.filters = {{"attributesAll", qint64(FILE_ATTRIBUTE_HIDDEN)}};
        check(selectedFiles(f.plan) == QStringList{"source/hello.txt"});
    });
    suite.run("fs.filter.content-boundary", "白盒单元", [] {
        Fixture f;
        cv::atomicWrite(f.source + "/boundary.txt",
                        QByteArray(cv::ChunkSize - 2, 'x') + "NEEDLE" + QByteArray(20, 'x'));
        f.plan.filters = {{"contentContains", "NEEDLE"}};
        check(selectedFiles(f.plan) == QStringList{"source/boundary.txt"});
    });
    suite.run("fs.filter.changed-since", "白盒单元", [] {
        Fixture f;
        const auto m = f.capture();
        f.plan.filters = {{"changedSinceManifest", m}};
        check(selectedFiles(f.plan).isEmpty());
        cv::atomicWrite(f.source + "/hello.txt", "different size");
        check(selectedFiles(f.plan) == QStringList{"source/hello.txt"});
    });
    suite.run("fs.restore.no-overwrite-preflight", "安全", [] {
        Fixture f;
        const auto m = f.capture();
        check(QDir().mkpath(f.restored()));
        cv::atomicWrite(f.restored() + "/hello.txt", "keep original");
        throws([&] { f.restore(m); });
        check(cv::readFile(f.restored() + "/hello.txt") == "keep original");
        check(!QFileInfo::exists(f.restored() + "/nested"));
    });
    suite.run("fs.restore.explicit-overwrite", "系统功能", [] {
        Fixture f;
        const auto m = f.capture();
        f.restore(m);
        cv::atomicWrite(f.restored() + "/hello.txt", "old");
        cv::RestoreOptions options;
        options.overwrite = true;
        f.restore(m, "restored", options);
        check(cv::readFile(f.restored() + "/hello.txt") == cv::readFile(f.source + "/hello.txt"));
    });
    suite.run("fs.restore.disk-full-write-boundary-injection", "异常健壮性", [] {
        Fixture f;
        cv::atomicWrite(f.source + "/hello.txt", QByteArray(cv::ChunkSize + 19, 'a'));
        const auto m = f.capture();
        check(QDir().mkpath(f.restored()));
        cv::atomicWrite(f.restored() + "/hello.txt", "old file survives");
        cv::RestoreOptions options;
        options.overwrite = true;
        options.selectedPaths << "source/hello.txt";
        cv::Context context;
        bool injected = false;
        context.progress = [&](quint64, quint64, const QString &) {
            injected = true;
            throw cv::Error("DISK_FULL_SIMULATED", "模拟第一块已写入后的磁盘失败");
        };
        throws([&] {
            cv::fs::restore(
                m, [&](const QString &id) { return f.blocks[id]; }, f.temporary.path() + "/restored", options,
                context);
        });
        check(injected);
        check(cv::readFile(f.restored() + "/hello.txt") == "old file survives");
        check(QDir(f.restored()).entryList({".cv-restore-*"}, QDir::Files | QDir::Hidden).isEmpty());
    });
    suite.run("fs.capture.denied-dacl-restricted-token", "安全", [] {
        Fixture f;
        const auto path = win(f.source + "/hello.txt");
        PSECURITY_DESCRIPTOR saved = nullptr;
        PACL original = nullptr;
        check(GetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr, &original, nullptr,
                                    &saved) == ERROR_SUCCESS);
        SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
        PSID everyone = nullptr;
        check(AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone));
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_READ_DATA;
        access.grfAccessMode = DENY_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<wchar_t *>(everyone);
        PACL denied = nullptr;
        HANDLE token = nullptr, restricted = nullptr;
        bool applied = false, impersonating = false;
        auto cleanup = [&] {
            if (impersonating)
                RevertToSelf();
            if (applied)
                SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, original, nullptr);
            if (restricted)
                CloseHandle(restricted);
            if (token)
                CloseHandle(token);
            if (denied)
                LocalFree(denied);
            FreeSid(everyone);
            LocalFree(saved);
        };
        try {
            check(SetEntriesInAclW(1, &access, original, &denied) == ERROR_SUCCESS);
            check(SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, denied, nullptr) == ERROR_SUCCESS);
            applied = true;
            check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE,
                                   &token));
            check(CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr,
                                        &restricted));
            check(ImpersonateLoggedOnUser(restricted));
            impersonating = true;
            throws([&] { f.capture(); }, "ACL denied reads must not produce a successful snapshot");
            cleanup();
        } catch (...) {
            cleanup();
            throw;
        }
    });
    suite.run("fs.restore.selection", "系统功能", [] {
        Fixture f;
        const auto m = f.capture();
        cv::RestoreOptions options;
        options.selectedPaths << "source/nested";
        f.restore(m, "restored", options);
        check(QFileInfo::exists(f.restored() + "/nested/中文.txt"));
        check(!QFileInfo::exists(f.restored() + "/hello.txt"));
    });
    suite.run("fs.restore.corruption-no-destination", "安全", [] {
        Fixture f;
        const auto m = f.capture();
        f.blocks.begin().value()[0] ^= 1;
        throws([&] { f.restore(m); });
        check(!QFileInfo::exists(f.temporary.path() + "/restored"));
    });
    suite.run("fs.capture.cancelled", "异常健壮性", [] {
        Fixture f;
        std::atomic_bool cancel{true};
        cv::Context context;
        context.cancelled = &cancel;
        throws([&] { cv::fs::capture(f.plan, [](const QByteArray &) { return "unused"; }, context); });
    });
    suite.run("fs.capture.source-target-overlap", "安全", [] {
        Fixture f;
        f.plan.destination = f.source + "/nested/backup.cv";
        throws([&] { f.capture(); });
    });
    suite.run("fs.capture.exclusively-locked-source", "并发稳定性", [] {
        Fixture f;
        HANDLE locked = CreateFileW(win(f.source + "/hello.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked != INVALID_HANDLE_VALUE);
        try {
            throws([&] { f.capture(); });
            CloseHandle(locked);
        } catch (...) {
            CloseHandle(locked);
            throw;
        }
    });
    for (const auto &bad :
         QStringList{"../escape", "C:/escape", "source/../escape", "source/CON.txt", "source/a.",
                     "source/a:b", "source/CONIN$", "source/CONOUT$", "source/COM¹.txt"}) {
        suite.run("fs.manifest.reject-" + bad, "安全", [bad] {
            Fixture f;
            auto m = f.capture();
            auto entries = m["entries"].toArray();
            auto first = entries[0].toObject();
            first["path"] = bad;
            entries[0] = first;
            m["entries"] = entries;
            throws([&] { cv::fs::validateManifest(m); });
        });
    }
    suite.run("fs.manifest.case-collision", "安全", [] {
        Fixture f;
        auto m = f.capture();
        auto entries = m["entries"].toArray();
        auto duplicate = entry(m, "source/hello.txt");
        duplicate["path"] = "source/HELLO.TXT";
        entries.append(duplicate);
        m["entries"] = entries;
        throws([&] { cv::fs::validateManifest(m); });
    });
    suite.run("fs.ntfs.hardlinks-shared-identity", "系统功能", [] {
        Fixture f;
        check(CreateHardLinkW(win(f.source + "/hard.txt").c_str(), win(f.source + "/hello.txt").c_str(),
                              nullptr));
        const auto m = f.capture();
        f.restore(m);
        check(fileId(f.restored() + "/hard.txt") == fileId(f.restored() + "/hello.txt"));
        f.plan.filters = {{"changedSinceManifest", m}};
        check(selectedFiles(f.plan).isEmpty(),
              "unchanged hardlinks should not repeatedly match the changed filter");
    });
    suite.run("fs.ntfs.file-and-directory-ads", "系统功能", [] {
        Fixture f;
        QFile fileAds(f.source + "/hello.txt:origin:$DATA");
        check(fileAds.open(QIODevice::WriteOnly));
        check(fileAds.write("file ADS") == 8);
        fileAds.close();
        // QSaveFile cannot rename an ADS on every Windows version; direct QFile
        // writes are safe for newly created fixture streams.
        QFile ads(f.source + "/empty:origin:$DATA");
        check(ads.open(QIODevice::WriteOnly));
        check(ads.write("directory ADS") == 13);
        ads.close();
        const auto m = f.capture();
        f.restore(m);
        check(cv::readFile(f.restored() + "/hello.txt:origin:$DATA") == "file ADS");
        check(cv::readFile(f.restored() + "/empty:origin:$DATA") == "directory ADS");
    });
    suite.run("fs.ntfs.sparse-over-4gib", "性能", [] {
        Fixture f;
        const auto path = f.source + "/large.sparse";
        HANDLE h = CreateFileW(win(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        check(h != INVALID_HANDLE_VALUE);
        DWORD count = 0;
        check(DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr));
        LARGE_INTEGER pos{};
        pos.QuadPart = (qint64(1) << 32) + 123;
        check(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN));
        check(SetEndOfFile(h));
        pos.QuadPart -= 4;
        check(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN));
        check(WriteFile(h, "TAIL", 4, &count, nullptr));
        CloseHandle(h);
        const auto m = f.capture();
        check(m["storedDataBytes"].toInteger() < 1024 * 1024);
        f.restore(m);
        check(QFileInfo(f.restored() + "/large.sparse").size() == (qint64(1) << 32) + 123);
        check(GetFileAttributesW(win(f.restored() + "/large.sparse").c_str()) & FILE_ATTRIBUTE_SPARSE_FILE);
        QFile file(f.restored() + "/large.sparse");
        check(file.open(QIODevice::ReadOnly));
        check(file.seek(file.size() - 4));
        check(file.read(4) == "TAIL");
    });
    suite.run("fs.ntfs.junction-no-recursion", "系统功能", [] {
        Fixture f;
        check(makeJunction(f.source + "/junction", f.source + "/nested"));
        const auto m = f.capture();
        check(entry(m, "source/junction")["type"] == "junction");
        for (const auto &ev : m["entries"].toArray())
            check(!ev.toObject()["path"].toString().startsWith("source/junction/"));
        f.restore(m);
        check(GetFileAttributesW(win(f.restored() + "/junction").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
    });
    suite.run("fs.restore.reparse-escape-rejected", "安全", [] {
        Fixture f;
        const auto m = f.capture();
        check(QDir().mkpath(f.temporary.path() + "/outside"));
        check(makeJunction(f.temporary.path() + "/restored", f.temporary.path() + "/outside"));
        throws([&] { f.restore(m); });
        check(!QFileInfo::exists(f.temporary.path() + "/outside/source"));
    });
    {
        Fixture permissionProbe;
        const bool permitted =
            CreateSymbolicLinkW(win(permissionProbe.source + "/probe").c_str(), L"hello.txt", 2);
        if (!permitted)
            suite.skip("fs.ntfs.symbolic-file-and-directory-links",
                       "Windows 未授予创建符号链接权限；未计为通过");
        else
            suite.run("fs.ntfs.symbolic-file-and-directory-links", "系统功能", [] {
                Fixture f;
                check(CreateSymbolicLinkW(win(f.source + "/file-link").c_str(), L"hello.txt", 2));
                check(CreateSymbolicLinkW(win(f.source + "/dir-link").c_str(), L"nested",
                                          SYMBOLIC_LINK_FLAG_DIRECTORY | 2));
                const auto m = f.capture();
                f.restore(m);
                check(GetFileAttributesW(win(f.restored() + "/file-link").c_str()) &
                      FILE_ATTRIBUTE_REPARSE_POINT);
                check(GetFileAttributesW(win(f.restored() + "/dir-link").c_str()) &
                      FILE_ATTRIBUTE_REPARSE_POINT);
                check(cv::readFile(f.restored() + "/file-link") == cv::readFile(f.source + "/hello.txt"));
            });
    }
    for (const auto &format : QStringList{"indexed", "tar"})
        for (const auto &compression : QStringList{"none", "huffman", "lz77"})
            for (const auto &cipher : QStringList{"none", "aes256", "chacha20"}) {
                suite.run("archive.roundtrip." + format + '.' + compression + '.' + cipher, "集成",
                          [format, compression, cipher] {
                              Fixture f;
                              f.plan.format = format;
                              f.plan.compression = compression;
                              f.plan.cipher = cipher;
                              f.plan.password = "密码-Test-2026";
                              cv::archive::backup(f.plan);
                              const auto m = cv::archive::inspect(f.plan.destination, f.plan.password);
                              check(m["archiveFormat"] == format);
                              check(cv::archive::verify(f.plan.destination, f.plan.password).success);
                              cv::archive::restore(f.plan.destination, f.temporary.path() + "/restored",
                                                   f.plan.password);
                              check(cv::readFile(f.restored() + "/hello.txt") ==
                                    cv::readFile(f.source + "/hello.txt"));
                              check(cv::readFile(f.restored() + "/nested/中文.txt") ==
                                    QByteArray::fromHex("00ff0102030400"));
                          });
            }
    suite.run("archive.wrong-password-no-output", "安全", [] {
        Fixture f;
        f.plan.cipher = "chacha20";
        f.plan.password = "correct";
        cv::archive::backup(f.plan);
        throws([&] { cv::archive::restore(f.plan.destination, f.temporary.path() + "/restored", "wrong"); });
        check(!QFileInfo::exists(f.temporary.path() + "/restored"));
    });
    if (QStandardPaths::findExecutable("tar").isEmpty())
        suite.skip("archive.pax-independent-reader", "环境没有 tar 可执行文件，独立格式互操作测试未运行");
    else
        suite.run("archive.pax-independent-reader", "部署兼容性", [] {
            Fixture f;
            f.plan.format = "tar";
            cv::archive::backup(f.plan);
            const auto bytes = cv::readFile(f.plan.destination);
            auto little32 = [&](qsizetype position) {
                quint32 n = 0;
                for (int i = 0; i < 4; ++i)
                    n |= quint32(quint8(bytes[position + i])) << (8 * i);
                return n;
            };
            qsizetype position = 12 + little32(8);
            QByteArray inner;
            while (position < bytes.size()) {
                const auto length = little32(position);
                position += 4;
                const auto record = bytes.mid(position, length);
                position += length;
                if (record[8] == 0)
                    break;
                // Inspect only the generic unencrypted wrapper here; external tar
                // independently validates the actual PAX header and member layout.
                inner += cv::compression::decode(record.mid(77, record.size() - 77 - 32));
            }
            const auto path = f.temporary.path() + "/portable.tar";
            cv::atomicWrite(path, inner);
            QProcess tar;
            tar.start(QStandardPaths::findExecutable("tar"), {"-tf", path});
            check(tar.waitForFinished(10000));
            check(tar.exitCode() == 0, QString::fromUtf8(tar.readAllStandardError()));
            const auto listing = tar.readAllStandardOutput();
            check(listing.contains("source/hello.txt"));
            check(listing.contains("source/empty"));
        });
    suite.run("archive.tampered-final-record", "安全", [] {
        Fixture f;
        cv::archive::backup(f.plan);
        auto bytes = cv::readFile(f.plan.destination);
        bytes[bytes.size() - 1] ^= 1;
        cv::atomicWrite(f.plan.destination, bytes);
        throws([&] { cv::archive::verify(f.plan.destination); });
    });
    suite.run("archive.truncation-detected", "异常健壮性", [] {
        Fixture f;
        cv::archive::backup(f.plan);
        auto bytes = cv::readFile(f.plan.destination);
        bytes.chop(80);
        cv::atomicWrite(f.plan.destination, bytes);
        throws([&] { cv::archive::restore(f.plan.destination, f.temporary.path() + "/restored"); });
        check(!QFileInfo::exists(f.temporary.path() + "/restored"));
    });
}
