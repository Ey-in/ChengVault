#pragma once
#include "common.hpp"
namespace cv::fs {
// Preview reports selection reasons without committing any backup data.
QJsonArray preview(const BackupPlan &plan, const Context &context = {});
// Chunks contain at most ChunkSize bytes and IDs are opaque sink identifiers.
// Every reference also carries its plaintext SHA for independent validation.
// A successful manifest is complete and can restore without previous versions.
QJsonObject capture(const BackupPlan &plan, const ChunkSink &storeChunk, const Context &context = {});
// All blocks are prevalidated; replacement is atomic per file, not per tree.
// Strict metadata failures throw; explicitly lenient restores return warnings.
TaskResult restore(const QJsonObject &manifest, const ChunkSource &loadChunk, const QString &destination,
                   const RestoreOptions &options = {}, const Context &context = {});
// Validate safe paths, types and stream ranges without reading block contents.
void validateManifest(const QJsonObject &manifest);
} // namespace cv::fs
