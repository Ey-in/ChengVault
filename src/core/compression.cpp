#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <queue>
#include <vector>

// Independent lossless block compression
// -------------------------------------
// The enclosing archive decides block boundaries and whether to encrypt.
// This module takes byte strings and has no file-system side effects.
// Original size and payload size are explicit in every encoded block.
// Original size is checked before reserving or reconstructing the output.
// Raw fallback uses a distinct ID, so decompression never guesses the codec.
// Both algorithms operate on bytes and preserve arbitrary binary input.
// Huffman describes a symbol distribution and a prefix-free bit stream.
// LZ77 describes already-seen byte sequences through backward references.
// Those are different algorithms; neither delegates to qCompress or zlib.
// Each block carries SHA256 of its original bytes to detect accidental damage.
// This checksum does not replace the keyed MAC of an encrypted archive.
// A decoder checks structural consistency before accepting that checksum.
// Forged lengths cannot expand output beyond the caller's configured bound.
// Trailing data, impossible references and nonzero padding are rejected.
// Deterministic encoding makes reproducible fixtures and comparisons possible.
// The hard 64 MiB cap also bounds encoder-side frequency and window resources.
// Normal repository chunks are 1 MiB and therefore below that cap.
// The format is versioned by the CVC1 magic; incompatible changes need a new
// version, not silently different meanings for an existing algorithm ID.
// See docs/algorithms.md for every header field and payload representation.

namespace cv::compression {
namespace {
using Byte = unsigned char;
constexpr qsizetype HeaderSize = 53;
// Blocks have a hard producer bound; callers decode with their own smaller
// bound. A forged length is rejected before reserve() or symbol expansion.
constexpr qsizetype MaxBlock = 64 * 1024 * 1024;
void put64(QByteArray &output, quint64 value) {
    for (int n = 56; n >= 0; n -= 8)
        output.append(char(value >> n));
}
quint64 get64(const QByteArray &data, qsizetype offset) {
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | Byte(data[offset + i]);
    return value;
}
struct Node {
    quint64 frequency = 0;
    int minimum = 0;
    int left = -1;
    int right = -1;
    int symbol = -1;
};
struct Tree {
    std::vector<Node> nodes;
    int root = -1;
};
Tree makeTree(const std::array<quint32, 256> &frequencies) {
    // Only present symbols need leaves; at most 256 leaves make 511 nodes.
    // Indices are stable because nodes are appended and the vector is reserved.
    // Parent frequency is the sum of children; no frequency is discarded.
    // A decoder rebuilds the tree from counts instead of trusting stored edges.
    // Heap ordering is deterministic even for an alphabet with equal counts.
    Tree tree;
    tree.nodes.reserve(511);
    // Equal frequencies use the lowest symbol as a stable tie-break, so a
    // decoder reconstructs exactly the same tree without serializing pointers.
    auto later = [&](int a, int b) {
        const auto &x = tree.nodes[size_t(a)];
        const auto &y = tree.nodes[size_t(b)];
        return x.frequency != y.frequency ? x.frequency > y.frequency : x.minimum > y.minimum;
    };
    std::priority_queue<int, std::vector<int>, decltype(later)> heap(later);
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[size_t(symbol)] == 0)
            continue;
        tree.nodes.push_back({frequencies[size_t(symbol)], symbol, -1, -1, symbol});
        heap.push(int(tree.nodes.size() - 1));
    }
    while (heap.size() > 1) {
        const int left = heap.top();
        heap.pop();
        const int right = heap.top();
        heap.pop();
        tree.nodes.push_back({tree.nodes[size_t(left)].frequency + tree.nodes[size_t(right)].frequency,
                              std::min(tree.nodes[size_t(left)].minimum, tree.nodes[size_t(right)].minimum),
                              left, right, -1});
        heap.push(int(tree.nodes.size() - 1));
    }
    if (!heap.empty())
        tree.root = heap.top();
    return tree;
}
void makeCodes(const Tree &tree, int index, std::vector<bool> &prefix,
               std::array<std::vector<bool>, 256> &codes) {
    // Push/pop maintains the root-to-current-node path without copying it at
    // every branch; only leaves save a code. Prefix-free structure means no
    // end marker is needed: the header supplies the number of output symbols.
    // With 256 leaves, recursion depth cannot exceed 255.
    const auto &node = tree.nodes[size_t(index)];
    if (node.symbol >= 0) {
        codes[size_t(node.symbol)] = prefix;
        return;
    }
    prefix.push_back(false);
    makeCodes(tree, node.left, prefix, codes);
    prefix.back() = true;
    makeCodes(tree, node.right, prefix, codes);
    prefix.pop_back();
}
QByteArray huffmanEncode(const QByteArray &input) {
    // Count storage uses u32 because one input block is bounded to 64 MiB.
    // The count table is deliberately simple to inspect during the defense.
    // For small or uniform data it can outweigh any bit savings; encode()
    // compares total payload lengths and selects raw storage when necessary.
    // Bits are accumulated most-significant-first with zero terminal padding.
    std::array<quint32, 256> frequencies{};
    for (char c : input)
        ++frequencies[Byte(c)];
    QByteArray payload;
    for (quint32 count : frequencies)
        for (int shift = 24; shift >= 0; shift -= 8)
            payload.append(char(count >> shift));
    const Tree tree = makeTree(frequencies);
    if (tree.root < 0)
        return payload;
    std::array<std::vector<bool>, 256> codes;
    std::vector<bool> prefix;
    makeCodes(tree, tree.root, prefix, codes);
    unsigned accumulator = 0;
    int bits = 0;
    // A single-symbol alphabet needs zero bits per symbol: the count table
    // already contains all information. Other alphabets are prefix-free.
    for (char c : input) {
        for (bool bit : codes[Byte(c)]) {
            accumulator = (accumulator << 1) | unsigned(bit);
            if (++bits == 8) {
                payload.append(char(accumulator));
                bits = 0;
                accumulator = 0;
            }
        }
    }
    if (bits)
        payload.append(char(accumulator << (8 - bits)));
    return payload;
}
QByteArray huffmanDecode(const QByteArray &payload, qsizetype expected) {
    // Sum all counts in u64 before constructing the tree. A table that claims
    // more or fewer symbols than the outer header is structurally invalid.
    // An empty alphabet is only valid for zero output; a one-symbol alphabet
    // needs no bitstream because its symbol and repetition count are known.
    // General decoding checks each bit read against the actual payload size.
    // Observed frequencies must match the table, not merely output length.
    // Unused trailing bits must be zero and complete trailing bytes are invalid.
    require(payload.size() >= 1024, "COMPRESSED_FORMAT", "Huffman 频率表不完整");
    std::array<quint32, 256> frequencies{}, observed{};
    quint64 count = 0;
    for (int symbol = 0; symbol < 256; ++symbol) {
        quint32 value = 0;
        for (int j = 0; j < 4; ++j)
            value = (value << 8) | Byte(payload[symbol * 4 + j]);
        frequencies[size_t(symbol)] = value;
        count += value;
    }
    require(count == quint64(expected), "COMPRESSED_FORMAT", "Huffman 频率总数与原长不符");
    const Tree tree = makeTree(frequencies);
    QByteArray output;
    output.reserve(expected);
    if (tree.root < 0) {
        require(payload.size() == 1024, "COMPRESSED_FORMAT", "空 Huffman 块存在多余字节");
        return output;
    }
    if (tree.nodes[size_t(tree.root)].symbol >= 0) {
        require(payload.size() == 1024, "COMPRESSED_FORMAT", "单符号 Huffman 块存在多余字节");
        return QByteArray(expected, char(tree.nodes[size_t(tree.root)].symbol));
    }
    quint64 bitsRead = 0;
    for (qsizetype written = 0; written < expected; ++written) {
        int node = tree.root;
        while (tree.nodes[size_t(node)].symbol < 0) {
            require(bitsRead < quint64(payload.size() - 1024) * 8, "COMPRESSED_FORMAT", "Huffman 位流被截断");
            const bool bit = (Byte(payload[1024 + qsizetype(bitsRead / 8)]) & (0x80u >> (bitsRead % 8))) != 0;
            ++bitsRead;
            node = bit ? tree.nodes[size_t(node)].right : tree.nodes[size_t(node)].left;
        }
        const int symbol = tree.nodes[size_t(node)].symbol;
        ++observed[size_t(symbol)];
        output.append(char(symbol));
    }
    require(observed == frequencies && (bitsRead + 7) / 8 == quint64(payload.size() - 1024),
            "COMPRESSED_FORMAT", "Huffman 计数或结束位置无效");
    if (bitsRead % 8) {
        const unsigned paddingMask = (1u << (8 - (bitsRead % 8))) - 1;
        require((Byte(payload.back()) & paddingMask) == 0, "COMPRESSED_FORMAT", "Huffman 填充位非零");
    }
    return output;
}
unsigned tripleHash(const QByteArray &input, qsizetype offset) {
    // A compact hash only chooses candidate chains; it never establishes a
    // match. Candidate bytes are compared before a reference is emitted.
    // Callers ensure that offset has at least three remaining input bytes.
    return ((unsigned(Byte(input[offset])) * 251u + Byte(input[offset + 1])) * 251u +
            Byte(input[offset + 2])) &
           65535u;
}
QByteArray lzEncode(const QByteArray &input) {
    // Match offsets always point backward, but matches may overlap the current
    // output. Such overlap is essential for efficiently encoding long runs.
    // Positions skipped by a match are still inserted into the hash history.
    // ProbeLimit trades optimal ratio for predictable runtime on collisions.
    // Every group has at most eight tokens and one flag byte; unused bits stay
    // zero. Distance is big-endian u16 and length uses a three-byte bias.
    // The ring stores positions only, keeping memory independent of total file
    // length; each caller-supplied block starts with an empty dictionary.
    // Bounded hash chains provide a 65535-byte sliding window. Chains hold
    // absolute positions; the modulo ring is only dereferenced inside window.
    constexpr qsizetype Window = 65535, MaxMatch = 258, ProbeLimit = 96;
    std::vector<qsizetype> head(65536, -1), previous(65536, -1);
    auto insert = [&](qsizetype position) {
        if (position + 2 >= input.size())
            return;
        const unsigned hash = tripleHash(input, position);
        previous[size_t(position & 65535)] = head[hash];
        head[hash] = position;
    };
    QByteArray payload;
    payload.reserve(input.size());
    qsizetype position = 0;
    while (position < input.size()) {
        const qsizetype flagIndex = payload.size();
        payload.append(char(0));
        unsigned flags = 0;
        for (int token = 0; token < 8 && position < input.size(); ++token) {
            qsizetype bestLength = 0, bestDistance = 0;
            if (position + 2 < input.size()) {
                qsizetype candidate = head[tripleHash(input, position)], probes = 0;
                const qsizetype limit = std::min<qsizetype>(MaxMatch, input.size() - position);
                while (candidate >= 0 && candidate < position && position - candidate <= Window &&
                       probes++ < ProbeLimit) {
                    qsizetype length = 0;
                    while (length < limit && input[candidate + length] == input[position + length])
                        ++length;
                    if (length > bestLength) {
                        bestLength = length;
                        bestDistance = position - candidate;
                    }
                    if (bestLength == limit)
                        break;
                    candidate = previous[size_t(candidate & 65535)];
                }
            }
            if (bestLength >= 3) {
                flags |= 1u << token;
                payload.append(char(bestDistance >> 8));
                payload.append(char(bestDistance));
                payload.append(char(bestLength - 3));
                for (qsizetype j = 0; j < bestLength; ++j)
                    insert(position + j);
                position += bestLength;
            } else {
                payload.append(input[position]);
                insert(position);
                ++position;
            }
        }
        payload[flagIndex] = char(flags);
    }
    return payload;
}
QByteArray lzDecode(const QByteArray &payload, qsizetype expected) {
    // A reference is safe only when its distance is nonzero and no greater
    // than the number of bytes already emitted into this block.
    // Match length must fit the remaining original length before any append.
    // Reading from the growing output implements overlap without an aliasing
    // memcpy, which would be invalid for some source/destination distances.
    // Exactly consuming both original length and payload is required.
    QByteArray output;
    output.reserve(expected);
    qsizetype offset = 0;
    while (output.size() < expected) {
        require(offset < payload.size(), "COMPRESSED_FORMAT", "LZ77 标记被截断");
        const unsigned flags = Byte(payload[offset++]);
        int token = 0;
        for (; token < 8 && output.size() < expected; ++token) {
            if (flags & (1u << token)) {
                require(payload.size() - offset >= 3, "COMPRESSED_FORMAT", "LZ77 匹配被截断");
                const qsizetype distance = (unsigned(Byte(payload[offset])) << 8) | Byte(payload[offset + 1]);
                const qsizetype length = Byte(payload[offset + 2]) + 3;
                offset += 3;
                require(distance > 0 && distance <= output.size() && length <= expected - output.size(),
                        "COMPRESSED_FORMAT", "LZ77 距离或长度无效");
                // Appending one byte at a time intentionally supports overlap,
                // for example distance=1 expands a run of identical bytes.
                for (qsizetype j = 0; j < length; ++j)
                    output.append(output[output.size() - distance]);
            } else {
                require(offset < payload.size(), "COMPRESSED_FORMAT", "LZ77 字面量被截断");
                output.append(payload[offset++]);
            }
        }
        require(token == 8 || (flags >> token) == 0, "COMPRESSED_FORMAT", "LZ77 未使用标记位非零");
    }
    require(offset == payload.size(), "COMPRESSED_FORMAT", "LZ77 存在多余字节");
    return output;
}
} // namespace

QByteArray encode(const QByteArray &input, const QString &algorithm) {
    // IDs describe what is actually stored, not what the user requested.
    // A request for Huffman can therefore produce ID 0 for incompressible data.
    // The 53-byte header is present even for raw storage and empty blocks.
    // Keeping length and digest fields identical across codecs simplifies the
    // archive reader and permits codec changes without changing chunk logic.
    require(input.size() <= MaxBlock, "BLOCK_SIZE", "压缩块超过 64 MiB");
    int id = 0;
    QByteArray payload;
    if (algorithm == "none")
        payload = input;
    else if (algorithm == "huffman") {
        id = 1;
        payload = huffmanEncode(input);
    } else if (algorithm == "lz77") {
        id = 2;
        payload = lzEncode(input);
    } else
        throw Error("COMPRESSION", "未知压缩算法");
    // Raw fallback is an explicit format ID, not a failed compression. It
    // prevents incompressible input from expanding by a Huffman table/window.
    if (payload.size() >= input.size()) {
        id = 0;
        payload = input;
    }
    QByteArray output("CVC1", 4);
    output.append(char(id));
    put64(output, quint64(input.size()));
    put64(output, quint64(payload.size()));
    output += crypto::sha256(input);
    output += payload;
    return output;
}
QByteArray decode(const QByteArray &input, qsizetype maxOutput) {
    // The caller controls its domain-specific bound: ordinary data chunks are
    // small, while a metadata chunk may opt into a larger explicit maximum.
    // Do not cast a forged u64 length to signed qsizetype until it is bounded.
    // Payload length must equal the bytes already supplied by the archive.
    // Returning data is contingent on both parser success and digest equality.
    require(input.size() >= HeaderSize && input.first(4) == "CVC1", "COMPRESSED_FORMAT", "压缩块格式无效");
    require(maxOutput >= 0, "BLOCK_SIZE", "无效解压上限");
    const quint64 expected = get64(input, 5), payloadSize = get64(input, 13);
    require(expected <= quint64(maxOutput) && expected <= quint64(MaxBlock), "BLOCK_SIZE",
            "解压数据超过长度上限");
    require(payloadSize == quint64(input.size() - HeaderSize), "COMPRESSED_FORMAT", "压缩数据长度不符");
    const QByteArray payload = input.mid(HeaderSize);
    QByteArray output;
    switch (Byte(input[4])) {
    case 0:
        require(payloadSize == expected, "COMPRESSED_FORMAT", "原始块长度不符");
        output = payload;
        break;
    case 1:
        output = huffmanDecode(payload, qsizetype(expected));
        break;
    case 2:
        output = lzDecode(payload, qsizetype(expected));
        break;
    default:
        throw Error("COMPRESSED_FORMAT", "未知压缩块算法");
    }
    require(crypto::equal(crypto::sha256(output), input.mid(21, 32)), "CHECKSUM", "解压数据校验失败");
    return output;
}
} // namespace cv::compression
