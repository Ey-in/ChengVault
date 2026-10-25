#include "chengvault/crypto.hpp"
#include "chengvault/network.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char **argv) {
    // The standalone server uses a Core event loop only; Widgets is unnecessary
    // on a machine that hosts storage but never displays the backup client.
    QCoreApplication application(argc, argv);
    application.setApplicationName("ChengVaultServer");
    application.setApplicationVersion("1.0.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "澄匣独立备份服务器。默认仅监听本机，局域网使用 --listen 0.0.0.0。\n"
        "user-add 从密码文件读取密码；device-add 读取或生成32字节十六进制配对密钥文件。\n"
        "管理文件只保存在服务器本机，不通过未认证网络传送。配置完成后请删除明文密码文件。");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "serve | user-add | device-add", "[command]");
    parser.addOptions({{{"r", "root"}, "服务器数据目录", "path", "server-data"},
                       {"listen", "监听地址", "address", "127.0.0.1"},
                       {"port", "监听端口，0表示自动分配", "port", "48484"},
                       {"user", "账户名", "name"},
                       {"password-file", "UTF-8密码文件（末尾换行自动去除）", "path"},
                       {"administrator", "创建管理员账户"},
                       {"quota", "账户存储配额（字节）", "bytes", "10737418240"},
                       {"device", "设备标识", "id"},
                       {"key-file", "设备配对密钥文件；不存在时创建", "path"}});
    parser.process(application);
    try {
        // Administration commands exit after one atomic configuration change.
        // serve retains the Server object for the lifetime of the event loop.
        const auto command = parser.positionalArguments().value(0, "serve");
        cv::network::Server server(parser.value("root"));
        if (command == "user-add") {
            // Read credentials from a file rather than a command-line password,
            // which would otherwise be exposed in shell history/process listings.
            cv::require(parser.isSet("user") && parser.isSet("password-file"), "ARGUMENT",
                        "缺少 --user 或 --password-file");
            auto password = cv::readFile(parser.value("password-file"), 4096);
            while (password.endsWith('\r') || password.endsWith('\n'))
                password.chop(1);
            bool valid = false;
            const auto quota = parser.value("quota").toULongLong(&valid);
            cv::require(valid && quota > 0, "ARGUMENT", "配额必须是正整数");
            server.configureUser(parser.value("user"), QString::fromUtf8(password),
                                 parser.isSet("administrator"), quota);
            QTextStream(stdout) << "账户已创建。" << Qt::endl;
            return 0;
        }
        if (command == "device-add") {
            // Generated keys are written to the explicitly selected local file.
            // stdout confirms success without disclosing the actual credential.
            cv::require(parser.isSet("user") && parser.isSet("device") && parser.isSet("key-file"),
                        "ARGUMENT", "缺少 --user、--device 或 --key-file");
            const auto keyFile = parser.value("key-file");
            const auto key = QFileInfo::exists(keyFile)
                                 ? QByteArray::fromHex(cv::readFile(keyFile, 256).trimmed())
                                 : cv::crypto::randomBytes(32);
            cv::require(key.size() == 32, "DEVICE_KEY", "密钥文件必须包含64位十六进制字符串");
            server.configureDevice(parser.value("user"), parser.value("device"), key);
            if (!QFileInfo::exists(keyFile))
                cv::atomicWrite(keyFile, key.toHex() + '\n');
            QTextStream(stdout) << "设备已授权，配对密钥保存在指定文件中。" << Qt::endl;
            return 0;
        }
        cv::require(command == "serve", "ARGUMENT", "未知服务器命令");
        // Refuse ambiguous ports and invalid addresses before constructing any
        // listener. The default remains loopback unless the operator changes it.
        bool validPort = false;
        const auto port = parser.value("port").toUInt(&validPort);
        QHostAddress address;
        cv::require(address.setAddress(parser.value("listen")), "ARGUMENT", "无效监听地址");
        cv::require(validPort && port <= 65535, "ARGUMENT", "端口必须介于0和65535之间");
        cv::require(server.listen(address, static_cast<quint16>(port)), "NETWORK_LISTEN",
                    server.errorString());
        QTextStream(stdout) << "READY " << server.port() << " " << address.toString() << Qt::endl;
        return application.exec();
    } catch (const cv::Error &error) {
        QTextStream(stderr) << error.code << ": " << error.what() << Qt::endl;
        return 1;
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;
        return 1;
    }
}
