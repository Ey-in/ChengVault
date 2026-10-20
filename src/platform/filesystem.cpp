#include "chengvault/filesystem.hpp"
#include "chengvault/crypto.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <limits>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

namespace cv::fs {
namespace {
// Filesystem adapter design notes:
// - A manifest is the portable boundary; live HANDLEs never leave this module.
// - Paths are UTF-16 at the Win32 boundary and UTF-8 inside serialized JSON.
// - File identity is volume serial plus file ID, never a filename heuristic.
// - Reading reparse metadata always uses FILE_FLAG_OPEN_REPARSE_POINT.
// - Directory enumeration does not descend through a reparse point.
// - An object rejected by filters may still be needed as a parent directory.
// - Sparse allocation is orthogonal to logical length and stream contents.
// - Named streams are independent byte streams, including directory streams.
// - A hardlink shares all streams, so the manifest stores those streams once.
// - Windows access checks are preserved; errors are never converted to data.
// - Privilege adjustment only enables privileges already present in the token.
// - A successful capture means every selected stream was completely read.
// - Restore validates names and chunks before creating any destination files.
// - Overwrite is opt-in and performed by same-directory file replacement.
// - Every object receives its recorded ACL; parent ACL propagation is suppressed.
// - This is a per-stream consistent capture, not a VSS database transaction.
// - A multi-file restore is not advertised as an all-or-nothing transaction.
// A handle always closes before any temporary file is renamed. This also keeps
// error paths exception-safe when NTFS rejects a privilege-dependent operation.
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
// Capture the thread-local code before Qt allocation or formatting calls.
// Messages include the operation and path; no secret material is logged.
QString winError(const QString &operation, const QString &path = {}) {
    return operation + " " + path + " (Win32=" + QString::number(GetLastError()) + ")";
}
// Extended-length paths avoid MAX_PATH truncation, including for ADS names.
// UNC paths need the UNC namespace form, not a second drive-style prefix.
// Conversion happens only after making the path absolute, so process working
// directory changes cannot silently redirect later Win32 operations.
// Extended-length syntax bypasses legacy MAX_PATH limits at Win32 boundaries.
// UNC paths need the UNC namespace prefix instead of a second drive prefix.
// Callers supply cleaned absolute paths before invoking filesystem APIs.
std::wstring native(const QString &path) {
    QString p = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (!p.startsWith("\\\\?\\"))
        p = p.startsWith("\\\\") ? "\\\\?\\UNC\\" + p.mid(2) : "\\\\?\\" + p;
    return p.toStdWString();
}
QString absolute(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).replace('\\', '/');
}
// Compare component boundaries: C:/foo must not contain C:/foobar.
// Windows name comparisons follow the supported case-insensitive NTFS convention.
bool containsPath(const QString &parent, const QString &child) {
    const auto p = absolute(parent), c = absolute(child);
    return c.compare(p, Qt::CaseInsensitive) == 0 || c.startsWith(p + '/', Qt::CaseInsensitive);
}
// FILETIME has 100 ns resolution and exceeds JSON's exact IEEE-double range.
// Store it as decimal text and reconstruct its two DWORDs without floating
// point conversion. Unix milliseconds are used only for user filter bounds.
quint64 ticks(const FILETIME &t) {
    return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
FILETIME fileTime(const QJsonValue &v) {
    bool ok = false;
    const auto n = v.toString().toULongLong(&ok);
    require(ok, "METADATA_TIME", "非法 FILETIME");
    return FILETIME{DWORD(n), DWORD(n >> 32)};
}
QDateTime dateTime(const QJsonValue &value) {
    return QDateTime::fromMSecsSinceEpoch(qint64(value.toString().toULongLong() / 10000) - 11644473600000LL,
                                          QTimeZone::UTC);
}
QString sidText(PSID sid) {
    LPWSTR text = nullptr;
    require(sid && ConvertSidToStringSidW(sid, &text), "SID_READ", "无法读取账户 SID");
    const QString result = QString::fromWCharArray(text);
    LocalFree(text);
    return result;
}
// Account display names are optional: a saved SID may no longer resolve on the
// machine performing the backup. Ownership preservation uses the SID itself.
// The two-call API allocates exact buffers without assuming a username limit.
// A deleted or foreign account can have a valid SID without a display name.
// Keep the SID authoritative; lookup failure must not discard that identity.
// The two-call pattern asks Windows for exact UTF-16 buffer sizes first.
QString sidAccount(PSID sid) {
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use;
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    if (!nameSize)
        return {};
    std::wstring name(nameSize, L'\0'), domain(domainSize, L'\0');
    if (!LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use))
        return {};
    const auto n = QString::fromWCharArray(name.c_str()), d = QString::fromWCharArray(domain.c_str());
    return d.isEmpty() ? n : d + "\\" + n;
}
// Relaxed metadata mode is deliberately separate from data error handling.
// A warning may describe an ACL that could not be captured/restored; missing
// stream bytes, invalid chunk checksums and unsafe paths still always fail.
void metadataFailure(bool strict, QStringList &warnings, const QString &message) {
    if (strict)
        throw Error("METADATA_PERMISSION", message);
    warnings << message;
}
// Request READ_CONTROL together with attributes to capture ACL and owner from
// the same non-following object handle. A failed ACL access may fall back to
// attribute-only access solely when the caller explicitly selected relaxed mode.
// DACL absence (null DACL) is distinct from an empty ACL and must survive JSON.
// Security descriptor memory is owned by LocalFree, not by a C++ delete call.
// The FILE_INFORMATION tuple also supplies hardlink identity for deduplication.
QJsonObject metadata(const QString &path, bool strict, QStringList &warnings, QString *identity = nullptr) {
    Handle h(CreateFileW(native(path).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.value == INVALID_HANDLE_VALUE) {
        metadataFailure(strict, warnings, winError("读取元数据失败", path));
        h.value = CreateFileW(native(path).c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    require(h.value != INVALID_HANDLE_VALUE, "STAT_FAILED", winError("读取属性失败", path));
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h.value, &info), "STAT_FAILED", winError("读取文件标识失败", path));
    if (identity)
        *identity = QString::number(info.dwVolumeSerialNumber) + ':' +
                    QString::number((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
    QJsonObject result{{"created", QString::number(ticks(info.ftCreationTime))},
                       {"accessed", QString::number(ticks(info.ftLastAccessTime))},
                       {"modified", QString::number(ticks(info.ftLastWriteTime))},
                       {"attributes", qint64(info.dwFileAttributes)},
                       {"linkCount", qint64(info.nNumberOfLinks)}};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    const DWORD error =
        GetSecurityInfo(h.value, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, &group, &dacl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS) {
        SetLastError(error);
        metadataFailure(strict, warnings, winError("读取属主/权限失败", path));
    } else {
        result["ownerSid"] = sidText(owner);
        result["ownerAccount"] = sidAccount(owner);
        result["groupSid"] = sidText(group);
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        GetSecurityDescriptorControl(descriptor, &control, &revision);
        result["securityControl"] = int(control);
        result["daclNull"] = dacl == nullptr;
        if (dacl)
            result["daclBase64"] =
                QString::fromLatin1(QByteArray(reinterpret_cast<char *>(dacl), dacl->AclSize).toBase64());
        LocalFree(descriptor);
    }
    return result;
}
// Parse only the two reparse tags whose semantics the restorer understands.
// An unknown tag can represent a cloud placeholder or a filesystem provider;
// treating it as an ordinary file could create a false successful snapshot.
// Offsets inside reparse payloads are byte offsets relative to the path buffer.
// Parse kernel and archived reparse bytes with the same bounds checks. The
// SubstituteName determines actual filesystem traversal; PrintName is merely
// display text and must never authorize a different target shown in a manifest.
// Keep raw bytes for faithful junction recreation, but verify their semantics.
QJsonObject decodeReparse(const QByteArray &data, const QString &path) {
    const auto size = data.size();
    require(size >= 8 && size <= MAXIMUM_REPARSE_DATA_BUFFER_SIZE, "REPARSE_INVALID", path);
    auto u16 = [&](int o) { return quint16(quint8(data[o]) | (unsigned(quint8(data[o + 1])) << 8)); };
    auto u32 = [&](int o) { return quint32(u16(o)) | (quint32(u16(o + 2)) << 16); };
    require(qsizetype(u16(4)) + 8 == size, "REPARSE_INVALID", "重解析数据长度不一致: " + path);
    const auto tag = u32(0);
    require(tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT, "REPARSE_UNSUPPORTED",
            "不支持的重解析点，不会跟随扫描: " + path);
    const int base = tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
    require(size >= base, "REPARSE_INVALID", path);
    const int offset = u16(8), length = u16(10), printOffset = u16(12), printLength = u16(14);
    require(length > 0 && offset % 2 == 0 && length % 2 == 0 && base + offset + length <= size &&
                printOffset % 2 == 0 && printLength % 2 == 0 && base + printOffset + printLength <= size,
            "REPARSE_INVALID", path);
    QString target = QString::fromWCharArray(
        reinterpret_cast<const wchar_t *>(data.constData() + base + offset), length / 2);
    require(!target.contains(QChar(0)), "REPARSE_INVALID", "链接目标含空字符: " + path);
    if (target.startsWith("\\??\\UNC\\"))
        target = "\\\\" + target.mid(8);
    else if (target.startsWith("\\??\\"))
        target = target.mid(4);
    return {{"type", tag == IO_REPARSE_TAG_SYMLINK ? "symlink" : "junction"},
            {"target", target},
            {"reparseBase64", QString::fromLatin1(data.toBase64())}};
}
QJsonObject reparse(const QString &path) {
    Handle h(CreateFileW(native(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr));
    std::array<unsigned char, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> data{};
    DWORD size = 0;
    require(h.value != INVALID_HANDLE_VALUE &&
                DeviceIoControl(h.value, FSCTL_GET_REPARSE_POINT, nullptr, 0, data.data(), DWORD(data.size()),
                                &size, nullptr),
            "REPARSE_READ", winError("读取链接失败", path));
    return decodeReparse(QByteArray(reinterpret_cast<const char *>(data.data()), size), path);
}
// Accept one rule or an array so hand-written CLI plans stay compact.
// An empty list means this dimension imposes no selection restriction.
QStringList strings(const QJsonValue &value) {
    QStringList result;
    if (value.isString())
        result << value.toString();
    else
        for (const auto &v : value.toArray())
            result << v.toString();
    return result;
}
QList<uint> foldedPoints(const QString &text) {
    auto points = text.toUcs4();
    // Simple folding preserves the original code-point count. Full string
    // folding could expand sharp-s or dotted-I and change what '?' consumes.
    for (auto &point : points)
        point = uint(QChar::toCaseFolded(char32_t(point)));
    return points;
}
// The wildcard grammar is implemented here rather than delegated to a regex or
// Qt matching engine. Qt only supplies Unicode case folding and code-point
// containers: '?' consumes one code point, including a supplementary character.
// Classes support ranges, leading !/^ negation and leading ] or edge - literals.
// Backslash is literal except for the path-field separator normalization below.
struct Wildcard {
    enum Kind { Literal, Any, Star, Class };
    struct Token {
        Kind kind = Literal;
        uint literal = 0;
        bool negated = false;
        QList<QPair<uint, uint>> ranges;
    };
    QList<Token> tokens;
    bool hasWildcard = false;

    explicit Wildcard(const QString &pattern) {
        // Bound user input before allocating its folded representation. This
        // permits full supported long paths while rejecting oversized rules.
        // Folding happens before tokenization, so literals and range endpoints
        // use exactly the same Unicode comparison domain as candidate values.
        require(pattern.size() <= 32000, "FILTER_PATTERN", "通配规则超过 32000 字符上限");
        const auto points = foldedPoints(pattern);
        for (qsizetype i = 0; i < points.size(); ++i) {
            const uint c = points[i];
            Token token;
            if (c == '*') {
                // Adjacent stars have identical meaning; collapse them so
                // retry state always refers to one consuming wildcard.
                hasWildcard = true;
                if (!tokens.isEmpty() && tokens.last().kind == Star)
                    continue;
                token.kind = Star;
            } else if (c == '?') {
                hasWildcard = true;
                token.kind = Any;
            } else if (c == '[') {
                // A closing bracket in the first class position is a literal:
                // []] matches ']', and [[] matches '[' without escape syntax.
                // A leading or trailing '-' is likewise literal; only a dash
                // between two actual endpoints introduces a range.
                hasWildcard = true;
                token.kind = Class;
                ++i;
                if (i < points.size() && (points[i] == '!' || points[i] == '^')) {
                    // Negation applies to the complete union of ranges. These
                    // characters elsewhere inside the class are ordinary data.
                    token.negated = true;
                    ++i;
                }
                bool first = true;
                while (i < points.size() && (points[i] != ']' || first)) {
                    const uint low = points[i++];
                    uint high = low;
                    first = false;
                    if (i + 1 < points.size() && points[i] == '-' && points[i + 1] != ']') {
                        high = points[i + 1];
                        i += 2;
                        require(low <= high, "FILTER_PATTERN", "通配字符范围起点大于终点: " + pattern);
                    }
                    token.ranges.append({low, high});
                }
                require(i < points.size() && points[i] == ']' && !token.ranges.isEmpty(), "FILTER_PATTERN",
                        "通配字符集合缺少结束括号或内容: " + pattern);
            } else {
                token.literal = c;
            }
            tokens.append(token);
        }
    }
    bool literalDirectory(const QList<uint> &text) const {
        // Prefix inclusion is a path-only convenience, not an unanchored glob.
        // Requiring the separator prevents 'src' from also selecting 'src-old'.
        // Classes are wildcard syntax even if they happen to contain one value.
        if (hasWildcard || text.size() < tokens.size())
            return false;
        for (qsizetype i = 0; i < tokens.size(); ++i)
            if (tokens[i].literal != text[i])
                return false;
        return text.size() == tokens.size() || text[tokens.size()] == '/';
    }
    bool matches(const QList<uint> &text) const {
        qsizetype input = 0, token = 0, lastStar = -1, retry = 0;
        quint64 work = 0;
        auto step = [&] {
            // Exceeding the budget is an explicit invalid-operation result;
            // treating it as a non-match would silently omit wanted backups.
            require(++work <= 8 * 1024 * 1024, "FILTER_PATTERN_COMPLEXITY",
                    "通配规则匹配超过工作量上限，请简化规则");
        };
        auto atom = [&](const Token &t, uint c) {
            step();
            if (t.kind == Any)
                return true;
            if (t.kind == Literal)
                return t.literal == c;
            bool found = false;
            for (const auto &range : t.ranges) {
                step();
                if (range.first <= c && c <= range.second) {
                    found = true;
                    break;
                }
            }
            return t.negated ? !found : found;
        };
        // Iterative last-star retry needs no recursion or exponential search
        // tree. When a suffix fails, let the last star consume one more point.
        // Worst-case comparisons are O(pattern * text), memory O(pattern+text).
        // The explicit work budget also includes class ranges, so a crafted
        // repeated class cannot freeze the GUI while scanning many filenames.
        while (input < text.size()) {
            step();
            if (token < tokens.size() && tokens[token].kind == Star) {
                lastStar = token++;
                retry = input;
            } else if (token < tokens.size() && atom(tokens[token], text[input])) {
                ++token;
                ++input;
            } else if (lastStar >= 0) {
                token = lastStar + 1;
                input = ++retry;
            } else {
                return false;
            }
        }
        while (token < tokens.size() && tokens[token].kind == Star)
            ++token;
        // Matching is anchored at both ends. An exhausted input accepts only
        // remaining stars; '?' and classes cannot consume an empty string.
        return token == tokens.size();
    }
};
// Validate all rule strings before traversal, even if a prior exclusion would
// hide an invalid rule. Errors are configuration failures, never silent misses.
void validateWildcardFilters(const QJsonObject &filters) {
    for (const auto key :
         {"pathInclude", "pathExclude", "nameInclude", "nameExclude", "types", "extensions", "owners"})
        for (auto pattern : strings(filters[key])) {
            if (QString::fromLatin1(key).startsWith("path"))
                pattern.replace('\\', '/');
            (void)Wildcard(pattern);
        }
}
// Literal directory rules include descendants; wildcard rules match the whole
// value. '*' intentionally crosses '/' for compatibility with existing plans.
// Only path patterns normalize separators; DOMAIN\user remains an account.
bool anyMatch(const QStringList &patterns, const QString &text, bool path = false) {
    const auto points = foldedPoints(text);
    for (QString p : patterns) {
        if (path)
            p.replace('\\', '/');
        const Wildcard pattern(p);
        if (path && pattern.literalDirectory(points))
            return true;
        if (pattern.matches(points))
            return true;
    }
    return false;
}
struct Record {
    QString source;
    QJsonObject entry;
    bool selected = false;
    QString reason;
    QString identity;
};
// Filter evaluation has a stable, documented order for useful preview reasons.
// Explicit path/name exclusion is checked before any positive predicate.
// Different predicates combine with AND; patterns within one field use OR.
// Path rules refer to manifest-relative paths, including each source basename.
// A literal directory path matches its entire subtree; name globs do not.
// Extension matching is independent from object type matching.
// Owner filters accept SID or resolved DOMAIN\user display names.
// File size bounds are inclusive and use logical size, not allocation size.
// Date bounds are inclusive and parsed as ISO8601, including timezone offsets.
// Attribute predicates operate on Win32 flags, not localized labels.
// Content searching preserves an overlap so matches may cross 1 MiB reads.
// Content means UTF-8 bytes; the program does not guess a file's text encoding.
// Change-since compares metadata; timestamps are not proof of content equality.
// Parent retention is a separate pass and never weakens directory exclusion.
QString filterReason(const Record &record, const QJsonObject &filters,
                     const QMap<QString, QJsonObject> &previous) {
    const auto path = record.entry["path"].toString(), type = record.entry["type"].toString();
    const auto name = path.section('/', -1);
    if (anyMatch(strings(filters["pathExclude"]), path, true))
        return "路径排除";
    if (anyMatch(strings(filters["nameExclude"]), name))
        return "名字排除";
    auto condition = [&](const char *key, const QString &text, bool pathMode = false) {
        const auto values = strings(filters[key]);
        return values.isEmpty() || anyMatch(values, text, pathMode);
    };
    if (!condition("pathInclude", path, true))
        return "路径不匹配";
    if (!condition("nameInclude", name))
        return "名字不匹配";
    const auto meta = record.entry["metadata"].toObject();
    if (!condition("types", type) && !(type == "file" && meta["linkCount"].toInteger() > 1 &&
                                       anyMatch(strings(filters["types"]), "hardlink")))
        return "对象类型不匹配";
    auto extensions = strings(filters["extensions"]);
    for (auto &extension : extensions) {
        if (extension.startsWith('.'))
            extension.remove(0, 1);
    }
    if (!extensions.isEmpty() && !anyMatch(extensions, QFileInfo(name).suffix()))
        return "扩展名不匹配";
    const auto owners = strings(filters["owners"]);
    if (!owners.isEmpty() && !anyMatch(owners, meta["ownerSid"].toString()) &&
        !anyMatch(owners, meta["ownerAccount"].toString()))
        return "属主不匹配";
    const auto size = record.entry["size"].toInteger();
    if (filters.contains("minSize") && size < filters["minSize"].toInteger())
        return "小于最小尺寸";
    if (filters.contains("maxSize") && size > filters["maxSize"].toInteger())
        return "大于最大尺寸";
    if (filters.contains("maxDepth") && path.count('/') > filters["maxDepth"].toInt())
        return "超过递归深度";
    const auto attrs = meta["attributes"].toInteger();
    const auto all = filters["attributesAll"].toInteger(), none = filters["attributesNone"].toInteger();
    if ((attrs & all) != all || (attrs & none) != 0)
        return "文件属性不匹配";
    const auto when = dateTime(meta[filters["timeField"].toString("modified")]);
    for (const auto key : {"timeFrom", "timeTo"}) {
        if (!filters.contains(key))
            continue;
        const auto bound = QDateTime::fromString(filters[key].toString(), Qt::ISODate);
        require(bound.isValid(), "FILTER_TIME", "时间筛选必须为 ISO8601");
        if ((QString(key) == "timeFrom" && when < bound) || (QString(key) == "timeTo" && when > bound))
            return "时间不匹配";
    }
    if (filters.contains("changedSinceManifest")) {
        const auto old = previous.value(path.toCaseFolded());
        const bool sameType =
            old["type"] == record.entry["type"] ||
            (old["type"] == "hardlink" && type == "file" && meta["linkCount"].toInteger() > 1);
        if (!old.isEmpty() && sameType && old["size"] == record.entry["size"] &&
            old["metadata"].toObject()["modified"] == meta["modified"] &&
            old["metadata"].toObject()["attributes"] == meta["attributes"])
            return "相对快照未改变";
    }
    const auto needle = filters["contentContains"].toString().toUtf8();
    if (!needle.isEmpty()) {
        if (type != "file")
            return "内容条件仅适用普通文件";
        require(needle.size() <= ChunkSize, "FILTER_SIZE", "内容查询字符串过长");
        QFile file(record.source);
        require(file.open(QIODevice::ReadOnly), "FILTER_READ", file.errorString());
        QByteArray overlap;
        bool found = false;
        while (!file.atEnd()) {
            const auto bytes = file.read(ChunkSize);
            require(!bytes.isEmpty() || file.atEnd(), "FILTER_READ", file.errorString());
            const auto joined = overlap + bytes;
            if (joined.contains(needle)) {
                found = true;
                break;
            }
            overlap = joined.right(needle.size() - 1);
        }
        if (!found)
            return "内容不匹配（按 UTF-8 字节检索）";
    }
    return {};
}
QList<Record> scan(const BackupPlan &plan, QStringList &warnings, const Context &context) {
    // Scan before reading content so preview and capture use the same selection.
    // Each selected source contributes a basename root in the restore namespace.
    // Colliding basenames are rejected rather than silently merging two roots.
    // Canonical source/parent paths catch ordinary junction-mediated overlap.
    // A hard maximum bounds memory used by the entry list and preview UI.
    // Cancellation is polled between objects and long content reads elsewhere.
    // An excluded directory is pruned, while an unmatched inclusion is traversed.
    // Unreadable directories are errors; an empty listing alone is not proof
    // that a source directory truly contains no files.
    require(!plan.sources.isEmpty(), "NO_SOURCE", "请选择至少一个备份源");
    validateWildcardFilters(plan.filters);
    QSet<QString> names;
    QList<Record> records;
    QMap<QString, QJsonObject> previous;
    for (const auto &v : plan.filters["changedSinceManifest"].toObject()["entries"].toArray()) {
        const auto entry = v.toObject();
        previous[entry["path"].toString().toCaseFolded()] = entry;
    }
    std::function<void(const QString &, const QString &)> visit;
    visit = [&](const QString &source, const QString &path) {
        context.check();
        require(records.size() < 1000000, "ENTRY_LIMIT", "目录对象数量超过上限");
        Record r;
        r.source = source;
        const auto m = metadata(source, plan.strictMetadata, warnings, &r.identity);
        const DWORD attrs = DWORD(m["attributes"].toInteger());
        const bool directory = attrs & FILE_ATTRIBUTE_DIRECTORY;
        r.entry = {{"path", path},
                   {"type", directory ? "directory" : "file"},
                   {"size", directory ? qint64(0) : QFileInfo(source).size()},
                   {"metadata", m},
                   {"streams", QJsonArray{}}};
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            const auto link = reparse(source);
            for (auto it = link.begin(); it != link.end(); ++it)
                r.entry[it.key()] = it.value();
            r.entry["targetDirectory"] = directory;
            r.entry["size"] = qint64(0);
        }
        r.reason = filterReason(r, plan.filters, previous);
        r.selected = r.reason.isEmpty();
        records.append(r);
        // Never traverse a reparse point, even one targeting another selected root.
        const bool excluded = anyMatch(strings(plan.filters["pathExclude"]), path, true) ||
                              anyMatch(strings(plan.filters["nameExclude"]), path.section('/', -1));
        if (directory && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !excluded) {
            const auto children = QDir(source).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            // Enumerating an unreadable directory must fail instead of silently
            // creating an apparently complete empty-directory backup.
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW(native(source + "/*").c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE)
                require(GetLastError() == ERROR_FILE_NOT_FOUND, "DIRECTORY_READ",
                        winError("目录枚举失败", source));
            else
                FindClose(find);
            for (const auto &child : children)
                visit(child.absoluteFilePath(), path + '/' + child.fileName());
        }
    };
    for (const auto &source : plan.sources) {
        const auto full = absolute(source);
        if (!plan.destination.isEmpty()) {
            auto target = absolute(plan.destination);
            auto realSource = QFileInfo(full).canonicalFilePath();
            auto realParent = QFileInfo(QFileInfo(target).absolutePath()).canonicalFilePath();
            if (!realParent.isEmpty())
                target = realParent + '/' + QFileInfo(target).fileName();
            require(!containsPath(realSource.isEmpty() ? full : realSource, target) &&
                        !containsPath(target, full),
                    "SOURCE_TARGET_OVERLAP", "源和目标不可重叠: " + source);
        }
        QString name = QFileInfo(full).fileName();
        if (name.isEmpty() || name.endsWith(':'))
            name = "volume-" + QString(full[0]);
        require(!names.contains(name.toCaseFolded()), "ROOT_COLLISION", "多源目录名称冲突: " + name);
        names.insert(name.toCaseFolded());
        visit(full, name);
    }
    QSet<QString> ancestors;
    // Compute the ancestor closure after matching all entries. This avoids
    // empty restore parents being accidentally removed by filename/size rules.
    // Ancestors carry their own real NTFS metadata and are explained in preview.
    for (const auto &r : records)
        if (r.selected) {
            QString path = r.entry["path"].toString();
            while (path.contains('/')) {
                path = path.left(path.lastIndexOf('/'));
                ancestors.insert(path);
            }
        }
    for (auto &r : records)
        if (r.entry["type"] == "directory" && ancestors.contains(r.entry["path"].toString())) {
            r.selected = true;
            r.reason = "保留命中文件所需的祖先目录";
        }
    return records;
}
// Sparse streams may contain long logical holes with no physical allocation.
// Query ranges in bounded batches; ERROR_MORE_DATA is a continuation signal.
// Each iteration advances strictly beyond the previous range to avoid loops.
// Allocated tail ranges are clamped to EOF because allocation is block-aligned.
// Non-sparse streams have one full-length range and therefore no implicit gaps.
// Zero-length streams have no ranges but still appear explicitly in the manifest.
// Logical holes read as zeros but should not consume backup data blocks.
// Allocation queries can return partial batches with ERROR_MORE_DATA.
// Advance from the last returned range rather than assuming a fixed stride.
// Non-sparse streams use the downstream path with one complete extent.
// Empty streams have no extents and remain explicit manifest items.
QJsonArray allocatedRanges(HANDLE h, qint64 size, bool sparse) {
    QJsonArray ranges;
    if (!size)
        return ranges;
    if (!sparse) {
        ranges.append(QJsonObject{{"offset", qint64(0)}, {"length", size}});
        return ranges;
    }
    qint64 next = 0;
    while (next < size) {
        FILE_ALLOCATED_RANGE_BUFFER query{};
        query.FileOffset.QuadPart = next;
        query.Length.QuadPart = size - next;
        std::array<FILE_ALLOCATED_RANGE_BUFFER, 256> output{};
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query), output.data(),
                                        DWORD(sizeof(output)), &returned, nullptr);
        require(ok || GetLastError() == ERROR_MORE_DATA, "SPARSE_READ", winError("读取稀疏区间失败"));
        const auto count = returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        if (!count)
            break;
        for (size_t i = 0; i < count; ++i) {
            const auto offset = output[i].FileOffset.QuadPart;
            const auto length = std::min(output[i].Length.QuadPart, size - offset);
            require(offset >= next && length > 0 && offset <= size, "SPARSE_INVALID", "非法分配区间");
            ranges.append(QJsonObject{{"offset", qint64(offset)}, {"length", qint64(length)}});
            next = offset + length;
        }
        if (ok)
            break;
    }
    return ranges;
}
// Stream enumeration includes the unnamed stream and named $DATA streams.
// Directory streams carry bytes even though their owner is a directory.
// A sparse hole is an offset gap, never a fabricated stored block.
// Opening without write sharing rejects active concurrent writers.
// Size and timestamp rechecks close the scan-to-open consistency window.
QJsonArray captureStreams(const Record &record, const ChunkSink &sink, const Context &context,
                          quint64 &bytes) {
    // FindFirstStream includes the unnamed ::$DATA stream for ordinary files.
    // Normalize that name to an empty string so callers need not know Win32
    // default-stream syntax. Directory unnamed streams are not file data.
    // A directory with no streams legitimately reports ERROR_HANDLE_EOF.
    // A named stream's logical length and data chunks belong to that stream,
    // not to the parent file's unnamed stream or its file-size field.
    // The sink receives original bytes and chooses its own storage identity.
    // A separate unkeyed SHA-256 in each chunk validates sink/source correctness.
    // Reading never allocates an entire file, so a >4 GiB stream stays bounded.
    QJsonArray streams;
    WIN32_FIND_STREAM_DATA streamData{};
    HANDLE find = FindFirstStreamW(native(record.source).c_str(), FindStreamInfoStandard, &streamData, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        require(error == ERROR_HANDLE_EOF && record.entry["type"] == "directory", "STREAM_ENUM",
                winError("枚举数据流失败", record.source));
        return streams;
    }
    try {
        do {
            context.check();
            QString streamName = QString::fromWCharArray(streamData.cStreamName);
            if (streamName == "::$DATA")
                streamName.clear();
            if (record.entry["type"] == "directory" && streamName.isEmpty())
                continue;
            const auto full = record.source + streamName;
            // Denying concurrent writers provides a stable snapshot per stream.
            // An actively written file fails clearly and can be retried by tasks.
            Handle h(CreateFileW(native(full).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                                     FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr));
            require(h.value != INVALID_HANDLE_VALUE, "FILE_BUSY", winError("数据流被占用或无法读取", full));
            LARGE_INTEGER length{};
            require(GetFileSizeEx(h.value, &length) && length.QuadPart >= 0, "STREAM_SIZE", full);
            if (streamName.isEmpty()) {
                BY_HANDLE_FILE_INFORMATION now{};
                require(GetFileInformationByHandle(h.value, &now), "STAT_FAILED", full);
                require(length.QuadPart == record.entry["size"].toInteger() &&
                            QString::number(ticks(now.ftLastWriteTime)) ==
                                record.entry["metadata"].toObject()["modified"].toString(),
                        "SOURCE_CHANGED", "扫描后源文件发生变化，请重试: " + full);
            }
            const bool sparse = DWORD(record.entry["metadata"].toObject()["attributes"].toInteger()) &
                                FILE_ATTRIBUTE_SPARSE_FILE;
            const auto ranges = allocatedRanges(h.value, length.QuadPart, sparse);
            QJsonArray chunks;
            for (const auto &rv : ranges) {
                const auto range = rv.toObject();
                qint64 offset = range["offset"].toInteger(), remaining = range["length"].toInteger();
                LARGE_INTEGER position{};
                position.QuadPart = offset;
                require(SetFilePointerEx(h.value, position, nullptr, FILE_BEGIN), "SEEK_FAILED", full);
                while (remaining > 0) {
                    context.check();
                    QByteArray data(qsizetype(std::min<qint64>(ChunkSize, remaining)), Qt::Uninitialized);
                    DWORD count = 0;
                    require(ReadFile(h.value, data.data(), DWORD(data.size()), &count, nullptr) &&
                                count == DWORD(data.size()),
                            "SHORT_READ", winError("数据流读取失败", full));
                    const auto id = sink(data);
                    require(!id.isEmpty(), "CHUNK_ID", "数据块存储未返回标识");
                    chunks.append(QJsonObject{{"id", id},
                                              {"offset", offset},
                                              {"length", qint64(data.size())},
                                              {"sha256", QString::fromLatin1(crypto::sha256(data).toHex())}});
                    offset += data.size();
                    remaining -= data.size();
                    bytes += quint64(data.size());
                    context.update(bytes, 0, record.entry["path"].toString() + streamName);
                }
            }
            streams.append(QJsonObject{{"name", streamName},
                                       {"size", qint64(length.QuadPart)},
                                       {"sparse", sparse},
                                       {"ranges", ranges},
                                       {"chunks", chunks}});
        } while (FindNextStreamW(find, &streamData));
        require(GetLastError() == ERROR_HANDLE_EOF, "STREAM_ENUM", winError("枚举数据流失败", record.source));
        FindClose(find);
    } catch (...) {
        FindClose(find);
        throw;
    }
    return streams;
}
// Restore paths are a smaller language than the complete Windows path syntax.
// Reject alternate separators, drive syntax and trailing-dot aliases early.
// Device basenames stay reserved with extensions, for example CON.txt.
// Validation does not depend on whether the path already exists.
void validateRelative(const QString &path) {
    // Apply Windows naming rules even before a target directory exists.
    // Both slash traversal and alternate stream injection must be rejected.
    // Reserved DOS device basenames remain reserved when an extension is added.
    // Trailing spaces/dots are unsafe because Win32 may normalize them away.
    // Case collisions are checked globally by validateManifest, not here.
    require(!path.isEmpty() && path.size() <= 32000 && !path.startsWith('/') && !path.contains('\\') &&
                !path.contains(':') && !path.contains(QChar(0)),
            "UNSAFE_PATH", "非法归档路径: " + path);
    static const QRegularExpression device(
        "^(CON|CONIN\\$|CONOUT\\$|CLOCK\\$|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\..*)?$",
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : path.split('/')) {
        require(!part.isEmpty() && part != "." && part != ".." && !part.endsWith('.') &&
                    !part.endsWith(' ') && !device.match(part).hasMatch() &&
                    !part.contains(QRegularExpression("[<>\"|?*\\x00-\\x1f]")),
                "UNSAFE_PATH", "非法归档路径: " + path);
    }
}
// Existing junctions or symlinks could redirect writes outside the chosen tree.
// Check each existing ancestor, not just final-path attributes.
// A missing suffix can be created under the already checked ancestor.
// This protects detected redirection; it is not a hostile multi-user sandbox.
void safeAncestors(const QString &destination, const QString &relative) {
    // Prefix checks alone do not stop junction or symlink directory escapes.
    // Inspect every existing ancestor up to the volume root before writes.
    // Missing ancestors are safe to create only after their existing parents
    // have passed these checks; a non-directory ancestor is also a conflict.
    // The caller repeats this immediately before each object's restore phase.
    QString full = absolute(destination + '/' + relative);
    require(containsPath(destination, full), "PATH_ESCAPE", relative);
    QString current = QFileInfo(full).absolutePath();
    // Walk all ancestors, including the destination itself and its ancestors:
    // an existing junction above the restore root is just as dangerous.
    while (true) {
        const DWORD attrs = GetFileAttributesW(native(current).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES)
            require((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT),
                    "REPARSE_ESCAPE", current);
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
}
// CREATE_ALWAYS applies to a new temporary object or its named stream.
// The existing user target is never truncated by this function.
// Sparse setup precedes length extension to preserve allocation holes.
// Manifest validation checks offsets before this code performs writes.
void restoreStream(const QString &path, const QJsonObject &stream, const ChunkSource &source,
                   const Context &context, TaskResult &result) {
    // Callers supply a fresh temporary file path, or a newly created directory.
    // Establish sparse mode before extending length to avoid allocating holes.
    // Seeking to chunk offsets reconstructs holes without storing zero buffers.
    // Verify returned bytes against both expected length and captured checksum.
    // Disk-full and short-write failures abort the temporary object.
    // Flush before commit makes success mean the OS accepted the complete stream.
    const QString target = path + stream["name"].toString();
    Handle h(CreateFileW(native(target).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    require(h.value != INVALID_HANDLE_VALUE, "CREATE_STREAM", winError("创建数据流失败", target));
    DWORD count = 0;
    if (stream["sparse"].toBool())
        require(DeviceIoControl(h.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr),
                "SPARSE_WRITE", target);
    LARGE_INTEGER length{};
    length.QuadPart = stream["size"].toInteger();
    require(SetFilePointerEx(h.value, length, nullptr, FILE_BEGIN) && SetEndOfFile(h.value), "SET_LENGTH",
            winError("设置长度失败", target));
    for (const auto &cv : stream["chunks"].toArray()) {
        context.check();
        const auto chunk = cv.toObject();
        const auto bytes = source(chunk["id"].toString());
        require(bytes.size() == chunk["length"].toInteger() &&
                    crypto::sha256(bytes).toHex() == chunk["sha256"].toString().toLatin1(),
                "CHUNK_CORRUPT", "数据块校验失败");
        LARGE_INTEGER position{};
        position.QuadPart = chunk["offset"].toInteger();
        require(SetFilePointerEx(h.value, position, nullptr, FILE_BEGIN) &&
                    WriteFile(h.value, bytes.constData(), DWORD(bytes.size()), &count, nullptr) &&
                    count == DWORD(bytes.size()),
                "RESTORE_WRITE", winError("恢复写入失败", target));
        result.bytes += quint64(bytes.size());
        context.update(result.bytes, 0, target);
    }
    require(FlushFileBuffers(h.value), "FLUSH_FAILED", winError("同步磁盘失败", target));
}
// Apply reparse metadata to the link itself instead of its target.
// Valid content does not excuse a failed strict metadata restoration.
// DACL nullness, ACE payload and inheritance protection are distinct.
// Windows may normalize AUTO_INHERITED provenance while preserving actual ACEs.
void applyMetadata(const QString &path, const QJsonObject &meta, bool strict, QStringList &warnings) {
    // Times are set on a non-following handle, including for symbolic links.
    // ACL bytes are validated before they are passed to a kernel API.
    // OWNER/GROUP are real SIDs, never translated from a mutable display name.
    // Protected-vs-inherited DACL state is restored along with ACE data.
    // Directory ACL propagation is suppressed because descendants already have
    // their captured descriptors; parent application must not replace them.
    // Physical type bits cannot be fabricated by SetFileAttributes and are
    // intentionally excluded: sparse/link/directory construction creates them.
    // EFS keys, SACL audit entries and NTFS compression are outside this format.
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    {
        Handle h(CreateFileW(native(path).c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             flags, nullptr));
        const auto created = fileTime(meta["created"]), accessed = fileTime(meta["accessed"]),
                   modified = fileTime(meta["modified"]);
        if (h.value == INVALID_HANDLE_VALUE || !SetFileTime(h.value, &created, &accessed, &modified))
            metadataFailure(strict, warnings, winError("恢复时间失败", path));
    }
    if (meta.contains("ownerSid")) {
        PSID owner = nullptr, group = nullptr;
        const bool valid =
            ConvertStringSidToSidW(meta["ownerSid"].toString().toStdWString().c_str(), &owner) &&
            ConvertStringSidToSidW(meta["groupSid"].toString().toStdWString().c_str(), &group);
        if (!valid) {
            if (owner)
                LocalFree(owner);
            if (group)
                LocalFree(group);
            throw Error("SID_INVALID", path);
        }
        auto acl = QByteArray::fromBase64(meta["daclBase64"].toString().toLatin1());
        PACL dacl = meta["daclNull"].toBool() ? nullptr : reinterpret_cast<PACL>(acl.data());
        const bool aclValid = dacl == nullptr || (acl.size() >= qsizetype(sizeof(ACL)) &&
                                                  dacl->AclSize <= acl.size() && IsValidAcl(dacl));
        if (!aclValid) {
            LocalFree(owner);
            LocalFree(group);
            throw Error("ACL_INVALID", path);
        }
        Handle h(CreateFileW(native(path).c_str(), MAXIMUM_ALLOWED,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             flags, nullptr));
        DWORD error = ERROR_ACCESS_DENIED;
        if (h.value != INVALID_HANDLE_VALUE) {
            const auto control = SECURITY_DESCRIPTOR_CONTROL(meta["securityControl"].toInt());
            SECURITY_INFORMATION info =
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
            // Newly created objects normally already have the requested owner
            // and group. Setting an identical SID still requires WRITE_OWNER;
            // omit only fields whose actual SID has been read and compared.
            // A genuinely different owner/group keeps the strict permission path.
            PSID currentOwner = nullptr, currentGroup = nullptr;
            PSECURITY_DESCRIPTOR currentDescriptor = nullptr;
            if (GetSecurityInfo(h.value, SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION, &currentOwner,
                                &currentGroup, nullptr, nullptr, &currentDescriptor) == ERROR_SUCCESS) {
                if (currentOwner && EqualSid(owner, currentOwner))
                    info &= ~OWNER_SECURITY_INFORMATION;
                if (currentGroup && EqualSid(group, currentGroup))
                    info &= ~GROUP_SECURITY_INFORMATION;
                LocalFree(currentDescriptor);
            }
            info |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                  : UNPROTECTED_DACL_SECURITY_INFORMATION;
            // SetSecurityInfo explicitly documents that MAXIMUM_ALLOWED
            // handles suppress propagation into children. Each captured object
            // therefore receives its own ACL instead of a parent's later update.
            error = SetSecurityInfo(h.value, SE_FILE_OBJECT, info,
                                    (info & OWNER_SECURITY_INFORMATION) ? owner : nullptr,
                                    (info & GROUP_SECURITY_INFORMATION) ? group : nullptr, dacl, nullptr);
        }
        LocalFree(owner);
        LocalFree(group);
        if (error != ERROR_SUCCESS) {
            SetLastError(error);
            metadataFailure(strict, warnings, winError("恢复属主/权限失败", path));
        }
    }
    // Sparse/reparse/directory bits describe physical objects and cannot be set
    // via SetFileAttributes. The corresponding objects already exist here.
    DWORD attrs =
        DWORD(meta["attributes"].toInteger()) &
        (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
         FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_OFFLINE);
    if (!attrs)
        attrs = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileAttributesW(native(path).c_str(), attrs))
        metadataFailure(strict, warnings, winError("恢复属性失败", path));
}
// AdjustTokenPrivileges cannot grant rights absent from the login token.
// Only previously authorized backup/restore privileges can be enabled here.
// Later object-level access checks remain authoritative.
void enablePrivilege(const wchar_t *name) {
    // AdjustTokenPrivileges cannot grant a privilege the process does not have.
    // Standard users continue with ordinary access checks; later required
    // operations report exact failures instead of pretending elevation worked.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    }
    CloseHandle(token);
}
} // namespace

// Reuse the capture scanner so displayed reasons cannot drift from selection.
// Preview does not store chunks or commit any backup snapshot.
QJsonArray preview(const BackupPlan &plan, const Context &context) {
    // Preview includes rejected entries and a stable human-readable reason.
    // It does not call a chunk sink or create an archive/repository object.
    QStringList warnings;
    QJsonArray result;
    for (const auto &r : scan(plan, warnings, context)) {
        auto entry = r.entry;
        entry["selected"] = r.selected;
        entry["reason"] = r.reason.isEmpty() ? "符合所有条件" : r.reason;
        result.append(entry);
    }
    return result;
}
// The storage boundary deliberately has no filesystem path knowledge.
// Sinks can deduplicate content independently from NTFS object identity.
// Every returned manifest is complete, not a delta against a previous tree.
// Hardlinks are resolved after filtering to avoid excluded canonical targets.
// A capture exception leaves the caller responsible for uncommitted storage.
QJsonObject capture(const BackupPlan &plan, const ChunkSink &storeChunk, const Context &context) {
    // Only selected files participate in hardlink canonical-target selection.
    // Therefore excluding the first pathname never creates a dangling manifest
    // reference: the first included pathname becomes the data-bearing entry.
    // Source-order identities include a volume serial, avoiding cross-volume
    // collisions between coincidentally equal per-volume file IDs.
    // Validate our own output before allowing any caller to commit a snapshot.
    // A sink may already have stored blocks on failure; unreferenced immutable
    // objects are harmless and can be collected by the repository later.
    enablePrivilege(SE_BACKUP_NAME);
    QStringList warnings;
    const auto records = scan(plan, warnings, context);
    QJsonArray entries;
    QMap<QString, QString> hardlinks;
    quint64 bytes = 0, files = 0, logical = 0;
    for (const auto &r : records) {
        if (!r.selected)
            continue;
        context.check();
        auto entry = r.entry;
        const auto type = entry["type"].toString();
        if (type == "file" && hardlinks.contains(r.identity)) {
            entry["type"] = "hardlink";
            entry["target"] = hardlinks[r.identity];
        } else if (type == "file" || type == "directory") {
            if (type == "file")
                hardlinks[r.identity] = entry["path"].toString();
            entry["streams"] = captureStreams(r, storeChunk, context, bytes);
        }
        if (type != "directory") {
            ++files;
            logical += quint64(entry["size"].toInteger());
        }
        entries.append(entry);
    }
    QJsonObject result{{"version", 1},
                       {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {"entries", entries},
                       {"fileCount", qint64(files)},
                       {"totalBytes", qint64(logical)},
                       {"storedDataBytes", qint64(bytes)},
                       {"warnings", QJsonArray::fromStringList(warnings)}};
    validateManifest(result);
    return result;
}
// Decrypted JSON remains untrusted input: authentication is not a schema.
// Bounds constrain allocations before any destination object is opened.
// Ordered stream extents must not overlap or exceed logical length.
// Regular streams cover their bytes; only sparse streams may contain holes.
// Case-folded uniqueness models the supported Windows naming convention.
// Check link references after the complete entry-name map has been built.
void validateManifest(const QJsonObject &manifest) {
    // Treat a manifest as untrusted input even when produced by our own scanner.
    // Validating both writer and reader catches internal contract regressions.
    // Stream chunk offsets must be increasing, disjoint and contained in EOF.
    // Dense streams must cover every byte; sparse streams may leave zero holes.
    // The independent SHA-256 field is required, not optional legacy metadata.
    // Named streams can only be attached to files or directories.
    // Every parent must be an explicit directory, never a link or ordinary file.
    // A hardlink refers directly to a data-bearing file, eliminating link cycles.
    // Case folding matches the supported default Windows namespace semantics.
    require(manifest["version"].toInt() == 1 && manifest["entries"].isArray(), "MANIFEST_VERSION",
            "不支持的清单版本");
    const auto entries = manifest["entries"].toArray();
    require(entries.size() <= 1000000, "ENTRY_LIMIT", "归档对象数量超过上限");
    QMap<QString, QString> paths;
    const QSet<QString> types{"file", "directory", "symlink", "junction", "hardlink"};
    for (const auto &ev : entries) {
        require(ev.isObject(), "MANIFEST_ENTRY", "非法清单项");
        const auto e = ev.toObject();
        const auto path = e["path"].toString(), type = e["type"].toString();
        validateRelative(path);
        require(types.contains(type) && !paths.contains(path.toCaseFolded()), "PATH_COLLISION",
                "归档路径或类型冲突: " + path);
        paths[path.toCaseFolded()] = type;
        require(e["size"].toInteger(-1) >= 0, "MANIFEST_SIZE", path);
        const auto meta = e["metadata"].toObject();
        for (const auto key : {"created", "modified", "accessed"})
            fileTime(meta[key]);
        QSet<QString> streamNames;
        bool defaultStream = false;
        for (const auto &sv : e["streams"].toArray()) {
            const auto s = sv.toObject();
            const auto name = s["name"].toString();
            const auto length = s["size"].toInteger(-1);
            const bool named =
                name.startsWith(':') && name.endsWith(":$DATA") && name.size() > 7 &&
                !name.mid(1, name.size() - 7).contains(QRegularExpression("[:/\\\\<>\"|?*\\x00-\\x1f]"));
            require((name.isEmpty() || named) && !streamNames.contains(name.toCaseFolded()) && length >= 0,
                    "STREAM_INVALID", "非法流: " + path + name);
            require(type == "file" || (type == "directory" && named), "STREAM_TYPE", path);
            streamNames.insert(name.toCaseFolded());
            if (name.isEmpty()) {
                defaultStream = true;
                require(length == e["size"].toInteger(), "STREAM_SIZE", path);
            }
            qint64 end = 0;
            for (const auto &cv : s["chunks"].toArray()) {
                const auto c = cv.toObject();
                const auto off = c["offset"].toInteger(-1), count = c["length"].toInteger(-1);
                require(off >= end && count > 0 && count <= ChunkSize && off <= length &&
                            count <= length - off && !c["id"].toString().isEmpty() &&
                            c["id"].toString().size() <= 256 &&
                            QRegularExpression("^[0-9a-f]{64}$").match(c["sha256"].toString()).hasMatch(),
                        "CHUNK_INVALID", path);
                require(s["sparse"].toBool() || off == end, "CHUNK_GAP", path);
                end = off + count;
            }
            require(s["sparse"].toBool() || end == length, "CHUNK_MISSING", path);
        }
        require(type != "file" || defaultStream, "STREAM_MISSING", path);
        if (type == "symlink" || type == "junction") {
            require(!e["target"].toString().isEmpty() && !e["target"].toString().contains(QChar(0)),
                    "LINK_TARGET", path);
            require(type != "junction" ||
                        QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1()).size() >= 16,
                    "JUNCTION_INVALID", path);
            if (type == "junction") {
                const auto parsed =
                    decodeReparse(QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1()), path);
                require(parsed["type"] == "junction" && parsed["target"] == e["target"], "JUNCTION_TARGET",
                        "目录联接实际目标与清单不一致: " + path);
            }
        }
    }
    for (const auto &ev : entries) {
        const auto e = ev.toObject();
        auto p = e["path"].toString();
        while (p.contains('/')) {
            p = p.left(p.lastIndexOf('/'));
            require(paths.value(p.toCaseFolded()) == "directory", "PARENT_INVALID",
                    "归档父目录缺失或为链接: " + p);
        }
        if (e["type"] == "hardlink") {
            const auto target = e["target"].toString();
            validateRelative(target);
            require(paths.value(target.toCaseFolded()) == "file", "HARDLINK_TARGET", target);
        }
    }
}
// Preflight reads every referenced block before creating a restore root.
// Selection reduces output paths but never bypasses manifest safety checks.
// Directories are created before data; their ACLs and timestamps are restored last.
// Only the current file is transactional; earlier completed files may remain.
// Cancellation follows the same cleanup path as a write failure.
// Hardlink aliases are created after their canonical data object is committed.
TaskResult restore(const QJsonObject &manifest, const ChunkSource &loadChunk, const QString &destination,
                   const RestoreOptions &options, const Context &context) {
    // Restore has three phases: selection/preflight, data/link creation, metadata.
    // Selection includes ancestors and required hardlink canonical data targets.
    // No target directory is created during block integrity preflight.
    // Existing directory structure may be reused, but ordinary files require an
    // explicit overwrite choice. A directory/file type mismatch is never coerced.
    // Existing directory ADS are refused because replacing them would bypass
    // the per-file temporary replacement strategy used for ordinary files.
    // Symlinks and junctions are created only after all ordinary file writes.
    // Per-file sibling temporaries make MoveFileEx a same-volume replacement.
    // Cancellation removes only the current temporary, never a completed file.
    // Directory times are final because adding children changes parent times.
    validateManifest(manifest);
    require(!destination.isEmpty(), "NO_DESTINATION", "请选择恢复位置");
    context.check();
    enablePrivilege(SE_RESTORE_NAME);
    TaskResult result;
    result.output = absolute(destination);
    const auto all = manifest["entries"].toArray();
    QList<QJsonObject> selected;
    QSet<QString> wanted;
    for (const auto &ev : all) {
        const auto e = ev.toObject();
        const auto path = e["path"].toString();
        if (options.selectedPaths.isEmpty() ||
            std::any_of(options.selectedPaths.begin(), options.selectedPaths.end(), [&](const QString &p) {
                return path.compare(p, Qt::CaseInsensitive) == 0 ||
                       path.startsWith(p + '/', Qt::CaseInsensitive);
            }))
            wanted.insert(path.toCaseFolded());
    }
    // Selecting a hard link also selects its canonical data target; selecting a
    // child selects ancestors. This closure is computed before touching disk.
    for (const auto &ev : all) {
        const auto e = ev.toObject();
        if (wanted.contains(e["path"].toString().toCaseFolded()) && e["type"] == "hardlink")
            wanted.insert(e["target"].toString().toCaseFolded());
    }
    for (QString p : QSet<QString>(wanted))
        while (p.contains('/')) {
            p = p.left(p.lastIndexOf('/'));
            wanted.insert(p);
        }
    for (const auto &ev : all)
        if (wanted.contains(ev.toObject()["path"].toString().toCaseFolded()))
            selected.append(ev.toObject());
    require(options.selectedPaths.isEmpty() || !selected.isEmpty(), "SELECTION_EMPTY", "所选路径不存在");
    std::sort(selected.begin(), selected.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const auto rank = [](const QJsonObject &e) {
            const auto t = e["type"].toString();
            return t == "directory" ? 0 : t == "file" ? 1 : 2;
        };
        return rank(a) == rank(b) ? a["path"].toString().size() < b["path"].toString().size()
                                  : rank(a) < rank(b);
    });
    for (const auto &e : selected) {
        const auto path = e["path"].toString();
        safeAncestors(destination, path);
        const DWORD attrs = GetFileAttributesW(native(destination + '/' + path).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            require(!(attrs & FILE_ATTRIBUTE_REPARSE_POINT), "REPARSE_ESCAPE", path);
            const bool directory = e["type"] == "directory";
            require(directory == bool(attrs & FILE_ATTRIBUTE_DIRECTORY), "TYPE_CONFLICT", path);
            require(directory || options.overwrite, "DESTINATION_EXISTS",
                    "目标已存在（默认不覆盖）: " + path);
            require(directory || e["type"] == "file" || e["type"] == "hardlink", "LINK_OVERWRITE", path);
            require(!(directory && !e["streams"].toArray().isEmpty()), "DIRECTORY_ADS_CONFLICT",
                    "已有目录的 ADS 不会被直接覆盖: " + path);
        }
    }
    // Verify every required block before creating a destination. Authentication
    // and corruption failures therefore leave even new destination trees absent.
    for (const auto &e : selected)
        for (const auto &sv : e["streams"].toArray())
            for (const auto &cv : sv.toObject()["chunks"].toArray()) {
                context.check();
                const auto c = cv.toObject();
                const auto bytes = loadChunk(c["id"].toString());
                require(bytes.size() == c["length"].toInteger() &&
                            crypto::sha256(bytes).toHex() == c["sha256"].toString().toLatin1(),
                        "CHUNK_CORRUPT", "恢复预检发现数据块损坏");
            }
    require(QDir().mkpath(destination), "MKDIR_FAILED", destination);
    for (const auto &e : selected) {
        context.check();
        const auto relative = e["path"].toString(), type = e["type"].toString();
        safeAncestors(destination, relative);
        const auto path = absolute(destination + '/' + relative);
        if (type == "directory") {
            require(QDir().mkpath(path), "MKDIR_FAILED", path);
            for (const auto &sv : e["streams"].toArray())
                restoreStream(path, sv.toObject(), loadChunk, context, result);
            continue;
        }
        const auto temp = QFileInfo(path).absolutePath() + "/.cv-restore-" + uniqueId();
        bool temporaryDirectory = false;
        try {
            if (type == "file") {
                QList<QJsonObject> streams;
                for (const auto &stream : e["streams"].toArray())
                    streams.append(stream.toObject());
                std::stable_sort(streams.begin(), streams.end(),
                                 [](const QJsonObject &a, const QJsonObject &b) {
                                     return a["name"].toString().isEmpty() && !b["name"].toString().isEmpty();
                                 });
                for (const auto &sv : streams)
                    restoreStream(temp, sv, loadChunk, context, result);
            } else if (type == "hardlink") {
                require(CreateHardLinkW(native(temp).c_str(),
                                        native(destination + '/' + e["target"].toString()).c_str(), nullptr),
                        "HARDLINK_CREATE", winError("建立硬链接失败", path));
            } else if (type == "symlink") {
                temporaryDirectory = e["targetDirectory"].toBool();
                const DWORD flags =
                    (temporaryDirectory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | 2; // unprivileged flag
                const auto target = e["target"].toString().toStdWString();
                if (!CreateSymbolicLinkW(native(temp).c_str(), target.c_str(), flags)) {
                    require(CreateSymbolicLinkW(native(temp).c_str(), target.c_str(), flags & ~DWORD(2)),
                            "SYMLINK_PRIVILEGE", winError("建立符号链接失败（需开发者模式或权限）", path));
                }
            } else if (type == "junction") {
                temporaryDirectory = true;
                require(QDir().mkdir(temp), "JUNCTION_CREATE", temp);
                const auto data = QByteArray::fromBase64(e["reparseBase64"].toString().toLatin1());
                require(data.size() <= MAXIMUM_REPARSE_DATA_BUFFER_SIZE && data.size() >= 16 &&
                            *reinterpret_cast<const DWORD *>(data.constData()) == IO_REPARSE_TAG_MOUNT_POINT,
                        "JUNCTION_INVALID", path);
                Handle h(CreateFileW(native(temp).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                DWORD returned = 0;
                require(h.value != INVALID_HANDLE_VALUE &&
                            DeviceIoControl(h.value, FSCTL_SET_REPARSE_POINT,
                                            const_cast<char *>(data.constData()), DWORD(data.size()), nullptr,
                                            0, &returned, nullptr),
                        "JUNCTION_CREATE", winError("建立目录联接失败", path));
            }
            if (type != "hardlink")
                applyMetadata(temp, e["metadata"].toObject(), options.strictMetadata, result.warnings);
            context.check();
            // MoveFileEx replaces an ordinary file atomically on the same NTFS
            // volume; the old file is never deleted before a complete new one.
            require(MoveFileExW(native(temp).c_str(), native(path).c_str(),
                                MOVEFILE_WRITE_THROUGH | (options.overwrite ? MOVEFILE_REPLACE_EXISTING : 0)),
                    "RESTORE_COMMIT", winError("恢复提交失败", path));
            ++result.files;
        } catch (...) {
            SetFileAttributesW(native(temp).c_str(), FILE_ATTRIBUTE_NORMAL);
            if (temporaryDirectory)
                RemoveDirectoryW(native(temp).c_str());
            else
                DeleteFileW(native(temp).c_str());
            throw;
        }
    }
    // Children first: setting restrictive directory ACLs earlier could prevent
    // restoring descendants, and later writes would alter directory timestamps.
    for (auto it = selected.crbegin(); it != selected.crend(); ++it)
        if ((*it)["type"] == "directory")
            applyMetadata(destination + '/' + (*it)["path"].toString(), (*it)["metadata"].toObject(),
                          options.strictMetadata, result.warnings);
    result.details = {{"entries", selected.size()}, {"metadataStrict", options.strictMetadata}};
    return result;
}
} // namespace cv::fs
