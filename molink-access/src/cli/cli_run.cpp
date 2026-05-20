#include "cli_utils.h"
#include "../daemon/daemon_app.h"

#include <cstdio>
#include <cstring>
#include <io.h>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

int cmdRun(uint16_t localPort, uint16_t remotePort,
           const std::string& serial) {
    int outFd = _dup(1);

    std::thread daemonThread([&]() {
        DaemonApp app(localPort, remotePort, serial);
        app.run();
    });

    bool ready = false;
    for (int i = 0; i < 20; i++) {
        Sleep(300);
        auto st = sendPipeCmd("status");
        if (!st.empty()) { ready = true; break; }
    }
    if (!ready) {
        _write(outFd, "FAIL: Daemon did not start.\n", 28);
        sendPipeCmd("stop");
        if (daemonThread.joinable()) daemonThread.join();
        _close(outFd);
        return 1;
    }

    auto replOut = [outFd](const char* s) {
        _write(outFd, s, (unsigned)strlen(s));
    };

    replOut("MoLink REPL. Type 'help' for commands.\n\n");

    char input[4096];
    while (true) {
        replOut("molink> ");
        if (!fgets(input, sizeof(input), stdin)) break;

        size_t len = strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
            input[--len] = '\0';
        if (len == 0) continue;

        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0)
            break;
        if (strcmp(input, "stop") == 0) {
            sendPipeCmd("stop");
            break;
        }
        if (strcmp(input, "help") == 0 || strcmp(input, "?") == 0) {
            replOut("status devices forward push pull apush apull install ls shell del adel auth stop exit\n");
            continue;
        }

        std::string firstWord(input, strcspn(input, " \t"));
        bool isLocal = (firstWord == "auth" || firstWord == "apush" ||
                        firstWord == "apull" || firstWord == "adel");

        if (isLocal) {
            std::vector<std::string> words;
            std::istringstream iss(input);
            std::string w;
            while (iss >> w) words.push_back(w);

            std::vector<char*> argvStore;
            std::string prog = "molink";
            argvStore.push_back(&prog[0]);
            for (auto& w : words) argvStore.push_back(&w[0]);
            argvStore.push_back(nullptr);
            int argc2 = (int)argvStore.size() - 1;
            char** argv2 = argvStore.data();

            int saved = _dup(1);
            _dup2(outFd, 1);
            fflush(stdout);

            if (firstWord == "auth")       cmdAuth(argc2, argv2);
            else if (firstWord == "apush") cmdApush(argc2, argv2);
            else if (firstWord == "apull") cmdApull(argc2, argv2);
            else if (firstWord == "adel")  cmdAdel(argc2, argv2);

            fflush(stdout);
            _dup2(saved, 1);
            _close(saved);
            continue;
        }

        auto resp = sendPipeCmd(input);
        replOut(resp.c_str());
        replOut("\n");
    }

    sendPipeCmd("stop");
    if (daemonThread.joinable()) daemonThread.join();
    replOut("MoLink stopped.\n");
    _close(outFd);
    return 0;
}
