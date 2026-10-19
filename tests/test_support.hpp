#pragma once
#include "chengvault/common.hpp"
#include <QElapsedTimer>
#include <QTextStream>
#include <functional>

namespace cv::test {
// A tiny original harness makes every registered case visible in the JSON
// evidence. Assertions fail the test process; no skipped case becomes a pass.
struct Suite {
    QJsonArray records;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    void run(const QString &name, const QString &category, const std::function<void()> &fn) {
        QElapsedTimer timer;
        timer.start();
        QString result = "PASS", detail;
        try {
            fn();
            ++passed;
        } catch (const std::exception &error) {
            result = "FAIL";
            detail = QString::fromUtf8(error.what());
            ++failed;
        } catch (...) {
            result = "FAIL";
            detail = "Unknown exception";
            ++failed;
        }
        records.append(
            QJsonObject{{"name", name},
                        {"category", category},
                        {"result", result},
                        {"detail", detail},
                        {"durationMs", timer.elapsed()},
                        {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
        QTextStream(stdout) << result << " " << name << (detail.isEmpty() ? "" : ": " + detail) << Qt::endl;
    }
    void skip(const QString &name, const QString &reason) {
        ++skipped;
        records.append(
            QJsonObject{{"name", name}, {"category", "环境权限"}, {"result", "SKIP"}, {"detail", reason}});
    }
};
inline void check(bool condition, const QString &message = "assertion failed") {
    if (!condition)
        throw std::runtime_error(message.toStdString());
}
template <class F> void throws(F &&fn, const QString &message = "expected failure") {
    bool caught = false;
    try {
        fn();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, message);
}
} // namespace cv::test
