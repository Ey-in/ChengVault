#pragma once
#include "common.hpp"

namespace cv::crypto {
// Algorithm implementations are project-owned. OS APIs provide entropy only.
class Sha256 {
  public:
    Sha256();
    // Repeated finish() is stable; update() after finalization is rejected.
    // Instances may be copied to branch an incremental hash state safely.
    void update(const QByteArray &data);
    QByteArray finish();

  private:
    quint32 state_[8];
    quint64 length_ = 0;
    QByteArray pending_;
    QByteArray digest_;
    void transform(const unsigned char *block);
};
QByteArray sha256(const QByteArray &data);
QByteArray hashFile(const QString &path, const Context &context = {});
// KDF inputs are byte strings, not null-terminated text. Password callers
// choose UTF-8 once and persist the work factor and random salt separately.
QByteArray hmac(const QByteArray &key, const QByteArray &data);
QByteArray pbkdf2(const QByteArray &password, const QByteArray &salt, int iterations, int length = 32);
QByteArray hkdf(const QByteArray &key, const QByteArray &salt, const QByteArray &info, int length = 32);
QByteArray randomBytes(int length);
bool equal(const QByteArray &a, const QByteArray &b);
// Primitive entrypoints exist for published known-answer vectors. Storage
// callers use seal/open because CTR and ChaCha alone provide no integrity.
QByteArray aes256Ctr(const QByteArray &input, const QByteArray &key, const QByteArray &iv);
QByteArray chacha20(const QByteArray &input, const QByteArray &key, const QByteArray &nonce,
                    quint32 counter = 1);
// seal/open use a fresh salt and derive separate cipher and authentication keys.
// The complete envelope header is authenticated before any plaintext is returned.
QByteArray seal(const QByteArray &plaintext, const QByteArray &masterKey, const QString &cipher);
QByteArray open(const QByteArray &envelope, const QByteArray &masterKey);
// DPAPI is only for the current user's unattended task credential. It is not
// the archive cipher and does not replace either independently written cipher.
QByteArray protectLocalKey(const QByteArray &key);
QByteArray unprotectLocalKey(const QByteArray &protectedKey);
} // namespace cv::crypto
