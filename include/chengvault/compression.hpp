#pragma once
#include "common.hpp"
namespace cv::compression {
// Each compressed block embeds its original length and algorithm identifier.
// Decoding enforces a caller supplied size bound before allocating output.
QByteArray encode(const QByteArray &input, const QString &algorithm);
QByteArray decode(const QByteArray &input, qsizetype maxOutput = ChunkSize);
} // namespace cv::compression
