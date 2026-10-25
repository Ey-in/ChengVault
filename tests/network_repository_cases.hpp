// Included inside the test translation unit's anonymous namespace. These cases
// operate on real repositories, authenticated sessions and on-disk staging files.
// Assertions distinguish intended error codes from an unrelated generic failure.
template <class F> void rejects(const QString &code, F &&function) {
    try {
        function();
    } catch (const cv::Error &error) {
        check(error.code == code, "expected " + code + ", got " + error.code);
        return;
    }
    check(false, "operation unexpectedly accepted: " + code);
}

void repositoryAdditionalTests(cv::test::Suite &suite, const QString &root) {
    const auto folder = root + "/repository-boundaries";
    cv::BackupPlan plan;
    plan.sources = {folder + "/source"};
    plan.destination = folder + "/store";
    plan.cipher = "none";
    plan.compression = "none";
    QDir().mkpath(plan.sources.first());
    for (const auto &name : {"one", "two", "three"})
        cv::atomicWrite(plan.sources.first() + '/' + name, QByteArray(cv::ChunkSize, 'R'));
    cv::Repository repository(plan.destination, "boundary password");
    QString snapshot;
    suite.run("repository.auto-init-none-and-intrasnapshot-dedup", "集成", [&] {
        const auto result = repository.backup(plan);
        snapshot = result.details["snapshotId"].toString();
        check(result.details["newChunks"].toInt() == 1 && result.details["reusedChunks"].toInt() == 2);
        check(cv::readJson(plan.destination + "/repository.json")["cipher"] == "chacha20");
        check(repository.verify().details["chunks"].toInt() == 1);
    });
    suite.run("repository.initialization-refuses-existing-and-foreign-directory", "安全", [&] {
        rejects("REPOSITORY_EXISTS", [&] { repository.init(); });
        const auto foreign = folder + "/foreign";
        cv::atomicWrite(foreign + "/user.txt", "must remain");
        cv::Repository other(foreign, "password");
        rejects("REPOSITORY_NOT_EMPTY", [&] { other.init(); });
        check(cv::readFile(foreign + "/user.txt") == "must remain");
        cv::Repository invalid(folder + "/invalid", "password");
        rejects("REPOSITORY_CIPHER", [&] { invalid.init("none"); });
    });
    suite.run("repository.closed-operations-and-empty-aes-repository", "异常健壮性", [&] {
        cv::Repository closed(folder + "/closed", "password");
        rejects("REPOSITORY_CLOSED", [&] { closed.manifest("snapshot-0001"); });
        rejects("REPOSITORY_CLOSED", [&] { closed.changePassword("next"); });
        closed.init("aes256-ctr");
        check(closed.list().isEmpty());
        check(closed.verify().details["chunks"].toInt() == 0);
        check(closed.prune(2, 1).files == 0);
    });
    const auto configPath = plan.destination + "/repository.json";
    const auto config = cv::readJson(configPath);
    const auto key = repository.masterKey();
    const auto badConfig = [&](const QString &field, const QJsonValue &value, const QString &code) {
        auto changed = config;
        changed[field] = value;
        cv::writeJson(configPath, changed);
        try {
            rejects(code, [&] { cv::Repository::withMasterKey(plan.destination, key); });
        } catch (...) {
            cv::writeJson(configPath, config);
            throw;
        }
        cv::writeJson(configPath, config);
    };
    suite.run("repository.config-rejects-unsupported-version-and-cipher", "安全", [&] {
        badConfig("version", 2, "REPOSITORY_VERSION");
        badConfig("cipher", "unsupported", "REPOSITORY_CIPHER");
    });
    suite.run("repository.config-kdf-work-limit-and-invalid-id", "安全", [&] {
        badConfig("iterations", 9999, "REPOSITORY_KDF");
        badConfig("iterations", 2000001, "REPOSITORY_KDF");
        badConfig("id", "short", "REPOSITORY_ID");
        badConfig("id", "unsafe/identifier", "REPOSITORY_ID");
    });
    suite.run("repository.config-identity-is-bound-to-master-proof", "安全", [&] {
        badConfig("id", "different-valid-id", "REPOSITORY_KEY");
        rejects("REPOSITORY_KEY",
                [&] { cv::Repository::withMasterKey(plan.destination, QByteArray(31, 'x')); });
    });
    suite.run("repository.config-invalid-password-salt", "安全", [&] {
        auto changed = config;
        changed["salt"] = "01";
        cv::writeJson(configPath, changed);
        try {
            rejects("REPOSITORY_KDF", [&] { cv::Repository other(plan.destination, "boundary password"); });
        } catch (...) {
            cv::writeJson(configPath, config);
            throw;
        }
        cv::writeJson(configPath, config);
    });
    const auto manifestPath = plan.destination + "/snapshots/" + snapshot + ".cvs";
    const auto originalManifest = cv::readFile(manifestPath);
    const auto malformedManifest = [&](const QByteArray &plaintext, const QString &code) {
        cv::atomicWrite(manifestPath, cv::crypto::seal(plaintext, key, "chacha20"));
        try {
            rejects(code, [&] { repository.manifest(snapshot); });
        } catch (...) {
            cv::atomicWrite(manifestPath, originalManifest);
            throw;
        }
        cv::atomicWrite(manifestPath, originalManifest);
    };
    suite.run("repository.authenticated-manifest-json-and-identity-rejected", "安全", [&] {
        malformedManifest("{broken", "MANIFEST_JSON");
        malformedManifest("[]", "MANIFEST_JSON");
        auto document = repository.manifest(snapshot);
        auto identity = document["snapshot"].toObject();
        identity["id"] = "other-snapshot";
        document["snapshot"] = identity;
        malformedManifest(QJsonDocument(document).toJson(), "SNAPSHOT_ID");
    });
    suite.run("repository.corrupt-retained-block-prevents-prune-and-new-snapshot", "异常健壮性", [&] {
        repository.backup(plan);
        const auto path = filesBelow(plan.destination + "/objects").first();
        const auto saved = cv::readFile(path);
        auto corrupt = saved;
        corrupt.back() ^= 1;
        cv::atomicWrite(path, corrupt);
        const auto before = repository.list().size();
        try {
            throws([&] { repository.prune(1); });
            throws([&] { repository.backup(plan); });
            check(repository.list().size() == before, "failure removed or published a snapshot");
        } catch (...) {
            cv::atomicWrite(path, saved);
            throw;
        }
        cv::atomicWrite(path, saved);
    });
    suite.run("repository.age-retention-protects-newest-expired-version", "系统功能", [&] {
        cv::atomicWrite(plan.sources.first() + "/unique", "old-data");
        repository.backup(plan);
        cv::atomicWrite(plan.sources.first() + "/unique", "new-data");
        const auto latest = repository.backup(plan).details["snapshotId"].toString();
        const auto all = repository.list();
        for (auto value : all) {
            const auto id = value.toObject()["id"].toString();
            auto document = repository.manifest(id);
            auto identity = document["snapshot"].toObject();
            identity["createdUtc"] =
                QDateTime::currentDateTimeUtc().addDays(id == latest ? -4 : -10).toString(Qt::ISODateWithMs);
            document["snapshot"] = identity;
            cv::atomicWrite(plan.destination + "/snapshots/" + id + ".cvs",
                            cv::crypto::seal(QJsonDocument(document).toJson(), key, "chacha20"));
        }
        const auto result = repository.prune(100, 1);
        check(result.files == static_cast<quint64>(all.size() - 1));
        check(result.details["removedChunks"].toInt() >= 1);
        check(repository.list().size() == 1 && repository.list().first().toObject()["id"] == latest);
        repository.restore(latest, folder + "/age-restored");
        check(cv::readFile(folder + "/age-restored/source/unique") == "new-data");
        rejects("RETENTION_RULE", [&] { repository.prune(1, -1); });
    });
}

void networkAdditionalTests(cv::test::Suite &suite, cv::network::Server &server,
                            const cv::network::RemoteOptions &alice, cv::Repository &repository,
                            const QString &root) {
    const cv::network::Client client(alice);
    const auto serverRepository = root + "/server/users/alice/repositories/" + repository.id();
    const auto index = cv::readJson(serverRepository + "/index.json");
    const auto entries = index["inventory"].toArray();
    const auto remoteSize = index["size"].toVariant().toULongLong();
    suite.run("network.download-cancel-resume-reuses-completed-and-partial-objects", "异常健壮性", [&] {
        const auto destination = root + "/download-resume";
        std::atomic_bool cancelled{false};
        cv::Context context;
        context.cancelled = &cancelled;
        context.progress = [&](quint64, quint64, const QString &path) {
            if (path.startsWith("objects/"))
                cancelled = true;
        };
        throws([&] { worker([&] { return client.download(repository.id(), destination, context); }); });
        check(!QFileInfo::exists(destination + "/repository.json"));
        const auto result = worker([&] { return client.download(repository.id(), destination); });
        check(result.bytes < remoteSize, "resumption retransferred every byte");
        cv::Repository downloaded(destination, "new backup password");
        check(downloaded.verify().details["snapshots"].toInt() >= 1);
    });
    suite.run("network.download-restarts-invalid-full-part-and-cleans-stale-artifacts", "异常健壮性", [&] {
        const auto destination = root + "/download-stale";
        QDir().mkpath(destination);
        const auto stage = destination + ".cv-download-" + repository.id();
        const auto first = entries.first().toObject();
        const auto part = stage + '/' + first["path"].toString() + '.' + first["hash"].toString() + ".part";
        cv::atomicWrite(part, QByteArray(first["size"].toInt(), 'x'));
        cv::atomicWrite(stage + "/snapshots/obsolete-snapshot.cvs", "obsolete");
        const auto stalePart = stage + "/snapshots/obsolete-snapshot.cvs." + QString(64, 'a') + ".part";
        cv::atomicWrite(stalePart, "old partial bytes");
        worker([&] { return client.download(repository.id(), destination); });
        check(!QFileInfo::exists(destination + "/snapshots/obsolete-snapshot.cvs"));
        for (const auto &file : filesBelow(destination))
            check(!file.endsWith(".part"));
        check(cv::Repository(destination, "new backup password").verify().success);
    });
    suite.run("network.download-refuses-unknown-staging-file-without-publication", "安全", [&] {
        const auto destination = root + "/download-unknown";
        const auto stage = destination + ".cv-download-" + repository.id();
        cv::atomicWrite(stage + "/unknown.part", "must not be deleted");
        rejects("DOWNLOAD_STAGE",
                [&] { worker([&] { return client.download(repository.id(), destination); }); });
        check(!QFileInfo::exists(destination + "/repository.json"));
        check(cv::readFile(stage + "/unknown.part") == "must not be deleted");
    });
    suite.run("network.download-destination-changed-before-publish-is-preserved", "并发稳定性", [&] {
        const auto destination = root + "/download-conflict";
        cv::Context context;
        context.progress = [&](quint64, quint64, const QString &) {
            cv::atomicWrite(destination + "/user.txt", "new user data");
        };
        rejects("DOWNLOAD_DESTINATION",
                [&] { worker([&] { return client.download(repository.id(), destination, context); }); });
        check(cv::readFile(destination + "/user.txt") == "new user data");
        check(!QFileInfo::exists(destination + "/repository.json"));
    });
    suite.run("network.download-lock-prevents-parallel-materialization", "并发稳定性", [&] {
        const auto destination = root + "/download-locked";
        const auto stage = destination + ".cv-download-" + repository.id();
        QDir().mkpath(stage);
        QLockFile lock(stage + "/download.lock");
        check(lock.tryLock());
        rejects("DOWNLOAD_BUSY", [&] { client.download(repository.id(), destination); });
    });
    suite.run("network.corrupt-server-blob-reupload-repairs-one-object", "异常健壮性", [&] {
        const auto first = entries.first().toObject();
        const auto path = serverRepository + "/blobs/" + first["hash"].toString();
        const auto saved = cv::readFile(path);
        auto corrupt = saved;
        corrupt[0] ^= 1;
        cv::atomicWrite(path, corrupt);
        const auto result = worker([&] { return client.upload(repository.path()); });
        check(result.files == 1 && cv::readFile(path) == saved);
    });
    suite.run("network.commit-cleans-orphan-blobs-but-retains-resumable-parts", "系统功能", [&] {
        const auto orphan = serverRepository + "/blobs/" + QString(64, 'f');
        cv::atomicWrite(orphan, "unreferenced");
        cv::atomicWrite(orphan + ".part", "resumable");
        worker([&] { return client.upload(repository.path()); });
        check(!QFileInfo::exists(orphan) && QFileInfo::exists(orphan + ".part"));
    });
    const QByteArray data("opaque-object");
    const auto digest = QString::fromLatin1(cv::crypto::sha256(data).toHex());
    const QJsonObject object{{"path", "repository.json"}, {"size", data.size()}, {"hash", digest}};
    int fixture = 0;
    const auto invalidInventory = [&](const QJsonArray &inventory, const QString &code) {
        const auto id = "inventory-fixture-" + QString::number(++fixture);
        worker([&] {
            ProtocolProbe peer(alice);
            peer.login(alice);
            const auto response = peer.query({{"op", "begin"}, {"repository", id}, {"inventory", inventory}});
            check(!response["ok"].toBool() && response["code"] == code, QJsonDocument(response).toJson());
        });
    };
    suite.run("network.inventory-empty-duplicate-and-missing-config-rejected", "安全", [&] {
        invalidInventory({}, "NETWORK_INVENTORY");
        invalidInventory({object, object}, "NETWORK_INVENTORY");
        auto missing = object;
        missing["path"] = "snapshots/valid-snapshot.cvs";
        invalidInventory({missing}, "NETWORK_INVENTORY");
    });
    suite.run("network.inventory-path-hash-size-and-prefix-checked", "安全", [&] {
        auto changed = object;
        changed["path"] = "../outside";
        invalidInventory({changed}, "NETWORK_PATH");
        changed["path"] = "objects/aa/" + QString(64, 'b') + ".cvo";
        invalidInventory({changed}, "NETWORK_PATH");
        changed = object;
        changed["hash"] = "invalid";
        invalidInventory({changed}, "NETWORK_HASH");
        changed = object;
        changed["size"] = 0;
        invalidInventory({changed}, "NETWORK_OBJECT");
        changed["size"] = 67108865;
        invalidInventory({changed}, "NETWORK_OBJECT");
    });
    suite.run("network.inventory-same-hash-different-length-rejected", "安全", [&] {
        auto other = object;
        other["path"] = "snapshots/duplicate-snapshot.cvs";
        other["size"] = data.size() + 1;
        invalidInventory({object, other}, "NETWORK_INVENTORY");
    });
    suite.run("network.inventory-deduplicates-identical-ciphertext-hashes", "系统功能", [&] {
        worker([&] {
            ProtocolProbe peer(alice);
            peer.login(alice);
            auto other = object;
            other["path"] = "snapshots/duplicate-snapshot.cvs";
            const QString id = "duplicate-hashes";
            const auto begin =
                peer.query({{"op", "begin"}, {"repository", id}, {"inventory", QJsonArray{object, other}}});
            check(begin["ok"].toBool() && begin["missing"].toArray().size() == 1);
            check(peer.query({{"op", "put"},
                              {"repository", id},
                              {"hash", digest},
                              {"offset", 0},
                              {"data", QString::fromLatin1(data.toBase64())}})["complete"]
                      .toBool());
            const auto commit = peer.query({{"op", "commit"}, {"repository", id}});
            check(commit["ok"].toBool() && commit["snapshots"].toInt() == 1);
        });
    });
    const auto rejectedRequest = [&](const QJsonObject &request, const QString &code, bool begin = false) {
        worker([&] {
            ProtocolProbe peer(alice);
            peer.login(alice);
            if (begin)
                check(peer.query({{"op", "begin"},
                                  {"repository", "request-boundary"},
                                  {"inventory", QJsonArray{object}}})["ok"]
                          .toBool());
            const auto response = peer.query(request);
            check(!response["ok"].toBool() && response["code"] == code, QJsonDocument(response).toJson());
        });
    };
    suite.run("network.upload-state-incomplete-commit-and-unknown-operation", "安全", [&] {
        rejectedRequest({{"op", "put"}, {"repository", "not-started"}}, "UPLOAD_STATE");
        rejectedRequest({{"op", "commit"}, {"repository", "not-started"}}, "UPLOAD_STATE");
        rejectedRequest({{"op", "commit"}, {"repository", "request-boundary"}}, "UPLOAD_INCOMPLETE", true);
        rejectedRequest({{"op", "unknown"}, {"repository", "request-boundary"}}, "NETWORK_OPERATION");
    });
    suite.run("network.put-rejects-foreign-hash-invalid-range-and-digest", "安全", [&] {
        QJsonObject put{{"op", "put"},
                        {"repository", "request-boundary"},
                        {"hash", QString(64, 'a')},
                        {"offset", 0},
                        {"data", QString::fromLatin1(data.toBase64())}};
        rejectedRequest(put, "UPLOAD_OBJECT", true);
        put["hash"] = digest;
        put["offset"] = -1;
        rejectedRequest(put, "UPLOAD_RANGE", true);
        put["offset"] = 1;
        put["data"] = QString::fromLatin1(data.left(1).toBase64());
        rejectedRequest(put, "UPLOAD_OFFSET", true);
        put["offset"] = 0;
        put["data"] = QString::fromLatin1(QByteArray(data.size(), 'x').toBase64());
        rejectedRequest(put, "UPLOAD_HASH", true);
    });
    suite.run("network.active-upload-and-competing-session-locks", "并发稳定性", [&] {
        worker([&] {
            ProtocolProbe first(alice);
            first.login(alice);
            const QJsonObject begin{
                {"op", "begin"}, {"repository", "competing-upload"}, {"inventory", QJsonArray{object}}};
            check(first.query(begin)["ok"].toBool());
            ProtocolProbe second(alice);
            second.login(alice);
            check(second.query(begin)["code"] == "UPLOAD_BUSY");
            check(first.query(begin)["code"] == "UPLOAD_ACTIVE");
        });
    });
    suite.run("network.get-rejects-foreign-object-and-out-of-range-offset", "安全", [&] {
        rejectedRequest(
            {{"op", "get"}, {"repository", repository.id()}, {"hash", QString(64, 'e')}, {"offset", 0}},
            "REMOTE_OBJECT");
        const auto first = entries.first().toObject();
        rejectedRequest({{"op", "get"},
                         {"repository", repository.id()},
                         {"hash", first["hash"]},
                         {"offset", first["size"]}},
                        "DOWNLOAD_RANGE");
    });
    suite.run("network.admin-refuses-self-disable-unknown-action-and-zero-quota", "安全", [&] {
        rejects("ADMIN_SELF",
                [&] { worker([&] { return client.admin("disable-user", {{"user", "alice"}}); }); });
        rejects("ADMIN_ACTION",
                [&] { worker([&] { return client.admin("invalid-action", {{"user", "alice"}}); }); });
        rejects("QUOTA", [&] {
            worker([&] { return client.admin("set-quota", {{"user", "alice"}, {"quota", "0"}}); });
        });
        rejects("USER_MISSING", [&] {
            worker([&] { return client.admin("set-quota", {{"user", "absent"}, {"quota", "1"}}); });
        });
    });
    suite.run("network.existing-session-revoked-after-device-key-rotation", "安全", [&] {
        worker([&] {
            ProtocolProbe peer(alice);
            peer.login(alice);
            server.configureDevice(alice.user, alice.deviceId, cv::crypto::randomBytes(32));
            const auto response = peer.query({{"op", "list"}});
            server.configureDevice(alice.user, alice.deviceId, alice.deviceKey);
            check(response["code"] == "DEVICE_REVOKED");
        });
    });
    suite.run("network.account-configuration-validates-credentials", "异常健壮性", [&] {
        rejects("USER_EXISTS", [&] { server.configureUser("alice", "duplicate"); });
        rejects("USER_PASSWORD", [&] { server.configureUser("empty-password", ""); });
        rejects("QUOTA", [&] { server.configureUser("zero-quota", "password", false, 0); });
        rejects("DEVICE_KEY", [&] { server.configureDevice("alice", "invalid", QByteArray(31, 'x')); });
        rejects("USER_MISSING", [&] { server.configureDevice("absent", "device", QByteArray(32, 'x')); });
        auto bad = alice;
        bad.timeoutMs = 99;
        rejects("NETWORK_TIMEOUT", [&] { cv::network::Client(bad).list(); });
    });
    suite.run("network.existing-session-invalidated-by-password-reset", "安全", [&] {
        auto live = alice;
        live.user = "live-password";
        live.password = "before reset";
        live.deviceKey = cv::crypto::randomBytes(32);
        server.configureUser(live.user, live.password);
        server.configureDevice(live.user, live.deviceId, live.deviceKey);
        worker([&] {
            ProtocolProbe peer(live);
            peer.login(live);
            client.admin("reset-password", {{"user", live.user}, {"password", "after reset"}});
            check(peer.query({{"op", "list"}})["code"] == "LOGIN_EXPIRED");
        });
    });
    suite.run("network.existing-session-invalidated-by-account-disable", "安全", [&] {
        auto live = alice;
        live.user = "live-disable";
        live.password = "enabled login";
        live.deviceKey = cv::crypto::randomBytes(32);
        server.configureUser(live.user, live.password);
        server.configureDevice(live.user, live.deviceId, live.deviceKey);
        worker([&] {
            ProtocolProbe peer(live);
            peer.login(live);
            client.admin("disable-user", {{"user", live.user}, {"disabled", true}});
            check(peer.query({{"op", "list"}})["code"] == "NETWORK_AUTH");
        });
    });
    suite.run("network.default-user-quota-and-multiple-repository-accounting", "系统功能", [&] {
        worker([&] {
            return client.admin("create-user", {{"user", "quota-test"}, {"password", "quota login"}});
        });
        auto quota = alice;
        quota.user = "quota-test";
        quota.password = "quota login";
        quota.deviceKey = cv::crypto::randomBytes(32);
        server.configureDevice(quota.user, quota.deviceId, quota.deviceKey);
        const cv::network::Client quotaClient(quota);
        worker([&] { return quotaClient.upload(repository.path()); });
        worker([&] { return client.admin("set-quota", {{"user", quota.user}, {"quota", "1"}}); });
        worker([&] {
            ProtocolProbe peer(quota);
            peer.login(quota);
            const auto response = peer.query(
                {{"op", "begin"}, {"repository", "second-repository"}, {"inventory", QJsonArray{object}}});
            check(response["code"] == "QUOTA_EXCEEDED");
        });
        // Existing committed data survives a quota reduction and rejected upload.
        check(worker([&] { return quotaClient.list(); }).size() == 1);
    });
    suite.run("network.authenticated-malformed-json-and-session-binding", "安全", [&] {
        worker([&] {
            ProtocolProbe malformed(alice);
            malformed.login(alice);
            QByteArray sequence(8, '\0');
            qToBigEndian<quint64>(1, sequence.data());
            malformed.send(
                cv::crypto::seal(malformed.transcript + sequence + "[]", malformed.tx, "chacha20"));
            check(malformed.response()["code"] == "NETWORK_JSON");
            ProtocolProbe wrongSession(alice);
            wrongSession.login(alice);
            wrongSession.send(
                cv::crypto::seal(QByteArray(32, 'x') + sequence + "{}", wrongSession.tx, "chacha20"));
            check(wrongSession.response()["code"] == "NETWORK_SESSION");
        });
    });
    suite.run("network.connection-limit-rejects-excess-and-recovers", "并发稳定性", [&] {
        worker([&] {
            std::vector<std::unique_ptr<QTcpSocket>> peers;
            // Each socket deliberately stays in the bounded unauthenticated
            // phase. The 33rd real connection must be rejected before a worker
            // is allocated; closing our sockets then releases the whole load.
            for (int i = 0; i < 32; ++i) {
                auto peer = std::make_unique<QTcpSocket>();
                peer->connectToHost(alice.host, alice.port);
                check(peer->waitForConnected(10000));
                peers.push_back(std::move(peer));
                QThread::msleep(5);
            }
            QTcpSocket excess;
            excess.connectToHost(alice.host, alice.port);
            excess.waitForConnected(10000);
            if (excess.state() != QAbstractSocket::UnconnectedState)
                excess.waitForDisconnected(10000);
            check(excess.state() == QAbstractSocket::UnconnectedState);
            for (auto &peer : peers)
                peer->abort();
            QThread::msleep(30);
            check(!client.list().isEmpty(), "connection limit did not release after disconnection");
        });
    });
}
