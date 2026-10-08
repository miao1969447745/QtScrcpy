#include "wirelessdebugging.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTranslator>
#include <cstdio>
#include <stdexcept>
#include <vector>

static void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

struct FakeAdb {
    struct Call { QString serial; QStringList arguments; WirelessDebugging::Completion done; };
    std::vector<Call> pending;
    std::vector<QStringList> history;
    WirelessDebugging::Runner runner() {
        return [this](const QString &serial, const QStringList &args, WirelessDebugging::Completion done) {
            pending.push_back({serial, args, done});
            history.push_back(args);
        };
    }
    void reply(const QString &serial, bool success, const QString &output) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->serial == serial) {
                const auto callback = it->done;
                pending.erase(it);
                callback(success, output);
                return;
            }
        }
        throw std::runtime_error("No pending request for serial");
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    try {
        using W = WirelessDebugging;
        require(W::parsePort("5555\n") == W::Enabled, "enabled port");
        require(W::parsePort("0") == W::Disabled && W::parsePort("-1") == W::Disabled,
                "USB values");
        require(W::parsePort("") == W::Disabled && W::parsePort("permission denied") == W::Unknown,
                "empty/invalid values");
        require(W::parsePort("65536") == W::Unknown, "port range");

        {
            FakeAdb adb;
            W state(adb.runner());
            state.refresh("A"); state.refresh("A");
            require(adb.pending.size() == 1 && state.state("A").checking, "deduplicate checks");
            adb.reply("A", true, "");
            require(adb.pending.front().arguments.last() == "persist.adb.tcp.port", "persistent fallback");
            adb.reply("A", true, "5555");
            state.refresh("B"); adb.reply("B", true, "-1");
            require(state.state("A").mode == W::Enabled && state.state("B").mode == W::Disabled,
                    "independent serial states and service priority");
        }
        {
            FakeAdb adb;
            W state(adb.runner());
            int finished = 0;
            QObject::connect(&state, &W::operationFinished, &application,
                             [&finished](const QString &, bool, bool ok) { if (ok) ++finished; });
            state.refresh("A"); adb.reply("A", true, "-1");
            require(state.setEnabled("A", true), "enable starts");
            require(adb.pending.front().arguments == (QStringList() << "tcpip" << "5555"), "enable command");
            require(!state.setEnabled("A", true) && state.state("A").changing, "block duplicate mutation");
            require(!state.setEnabled("ALIAS:5555", false), "block simultaneous serial-alias mutation");
            adb.reply("A", true, "restarting in TCP mode");
            adb.reply("A", true, "5555");
            require(state.state("A").mode == W::Enabled && finished == 1, "confirm enabled");
            require(state.setEnabled("A", true) && adb.pending.empty(), "one-click enable is idempotent");
            require(state.setEnabled("A", false), "disable starts");
            require(adb.pending.front().arguments == (QStringList() << "usb"), "disable preserves USB");
            adb.reply("A", true, "restarting in USB mode"); adb.reply("A", true, "-1");
            require(state.state("A").mode == W::Disabled && finished == 2, "confirm disabled");
        }
        {
            FakeAdb adb;
            W state(adb.runner());
            state.setEnabled("A", true); adb.reply("A", false, "failed");
            require(state.state("A").mode == W::Unknown && !state.state("A").changing,
                    "failed mutation does not fake success");
            state.refresh("A"); adb.reply("A", false, "offline");
            require(state.state("A").mode == W::Unknown && !state.state("A").checking,
                    "failed check is unknown, not disabled");
        }
        {
            FakeAdb adb;
            W state(adb.runner());
            state.refresh("A");
            state.setEnabled("A", true);
            adb.reply("A", true, "-1"); // old query generation
            require(state.state("A").changing, "ignore old response during mutation");
            state.refresh("B"); adb.reply("B", true, "-1");
            adb.reply("A", true, "ok"); adb.reply("A", true, "5555");
            require(state.state("A").mode == W::Enabled && state.state("B").mode == W::Disabled,
                    "device selection changes do not retarget commands");
        }
        {
            FakeAdb adb;
            W state(adb.runner());
            state.setEnabled("192.0.2.1:5555", false);
            adb.reply("192.0.2.1:5555", true, "restarting in USB mode");
            require(state.state("192.0.2.1:5555").mode == W::Disabled && adb.pending.empty(),
                    "intentional Wi-Fi disconnect is not treated as failure");
        }
        {
            FakeAdb adb;
            W state(adb.runner());
            state.setEnabled("A", true); adb.reply("A", true, "ok");
            adb.reply("A", false, "restarting");
            QEventLoop wait;
            QTimer::singleShot(350, &wait, &QEventLoop::quit);
            wait.exec();
            require(adb.pending.size() == 1 && adb.pending.front().arguments.first() == "shell",
                    "restart recovery only retries read checks");
            adb.reply("A", true, "5555");
            int mutations = 0;
            for (const auto &args : adb.history) if (args.first() == "tcpip") ++mutations;
            require(mutations == 1 && state.state("A").mode == W::Enabled, "no mutation replay");
        }
        {
            FakeAdb adb;
            auto *state = new W(adb.runner());
            state->refresh("A"); delete state;
            adb.reply("A", true, "5555"); // safely ignored after GUI teardown
        }
        QTranslator translator;
        require(argc > 1 && translator.load(QString::fromLocal8Bit(argv[1])), "load compiled translations");
        require(translator.translate("Widget", "enable wireless debugging") == QStringLiteral("开启无线调试"),
                "initial Chinese label");
        require(translator.translate("Dialog", "Disable wireless debugging") == QStringLiteral("关闭无线调试"),
                "enabled Chinese label");
        require(translator.translate("Dialog", "Checking wireless debugging...") == QStringLiteral("正在检查无线调试..."),
                "checking Chinese label");
        std::puts("Wireless debug state, command, race, retry, lifetime and translation checks passed. No real phone commands executed.");
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
}
