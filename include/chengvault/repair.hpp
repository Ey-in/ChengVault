#pragma once
#include "common.hpp"
#include <QVector>
namespace cv::repair {
// Each file operation requires an existing parent directory. create/recover
// publish only at a previously nonexistent path and preserve input files.
// Sidecars protect 1 MiB shards using systematic RS(12,10), with duplicated
// metadata locators and SHA256 checks. Two located erasures per stripe are the
// exact capacity; this is not recovery from arbitrary 20% byte corruption.
TaskResult create(const QString &archive, const QString &sidecar, const Context &context = {});
QJsonObject check(const QString &archive, const QString &sidecar, const Context &context = {});
TaskResult recover(const QString &archive, const QString &sidecar, const QString &output,
                   const Context &context = {});
// Public algebra entrypoints permit independent finite-field and erasure tests.
QVector<QByteArray> encodeShards(const QVector<QByteArray> &data);
QVector<QByteArray> recoverShards(const QVector<QByteArray> &shards, const QVector<bool> &present);
} // namespace cv::repair
