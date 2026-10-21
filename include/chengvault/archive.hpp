#pragma once
#include "filesystem.hpp"
namespace cv::archive {
// Indexed and PAX are independent inner formats in one authenticated envelope.
// A successful standalone artifact contains its full manifest and all data.
TaskResult backup(const BackupPlan &plan, const Context &context = {});
// Authenticate the complete envelope before exposing names or metadata.
QJsonObject inspect(const QString &path, const QString &password = {}, const Context &context = {});
// Decode completely before writing any destination data.
// Collision and metadata policies are shared with the filesystem core.
TaskResult restore(const QString &path, const QString &destination, const QString &password = {},
                   const RestoreOptions &options = {}, const Context &context = {});
// Read and verify all referenced plaintext chunks, not only the outer header.
// Verification creates no restored tree.
TaskResult verify(const QString &path, const QString &password = {}, const Context &context = {});
} // namespace cv::archive
