// Browser — the desktop's web browser.
//
// Pages open in our own engine, sdk/xweb, built in from its viewer: sandboxed like
// the other built-in apps, fetching through the compositor's broker. What it can't do
// yet (scripts, forms, cookies) is a click away: "Open in Chromium" asks the shell to
// start this program again with a "chromium:" target, and then it runs Chromium as a
// Wayland app through xwayland-bridge, or says what's missing.
//
//   browser [--open] <path or URL>            our engine
//   browser [--open] chromium:<path or URL>   Chromium
//
// Chromium runs with its own process sandbox, not under the app sandbox: it needs
// the network and files to do its job. So it's started before ours would close.

#include "xkit/application.h"
#include "xkit/widget.h"
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace fbx::xkit;

namespace {

// What Chromium may be asked to open. This comes from whoever asked to open a
// file, so nothing else gets through: an argument like --renderer-cmd-prefix=…
// would have it run a command.
bool openable(std::string_view target) {
    return target.starts_with("/") || target.starts_with("https://") ||
           target.starts_with("http://") || target.starts_with("file://");
}

constexpr std::string_view kChromium = "chromium:";

std::string rawTarget(int argc, char** argv) {
    std::string target;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--open") {
            if (i + 1 < argc) target = argv[++i];
        } else if (target.empty()) {
            target = arg;
        }
    }
    return target;
}

std::string findEngine() {
    if (const char* chosen = std::getenv("FBX_BROWSER_ENGINE"))
        return ::access(chosen, X_OK) == 0 ? chosen : "";
    for (const char* candidate : {"/usr/local/bin/chrome", "/usr/local/bin/chromium"})
        if (::access(candidate, X_OK) == 0) return candidate;
    return {};
}

void runEngine(const std::string& engine, const std::string& target) {
    std::vector<std::string> args{
        engine,
        "--ozone-platform=wayland",
        "--disable-gpu",            // frames go through shared memory: the bridge's tested path
        "--no-first-run",
        "--no-default-browser-check",
    };
    if (const char* extra = std::getenv("FBX_BROWSER_ARGS")) args.emplace_back(extra);
    if (!target.empty()) {
        args.emplace_back("--");    // whatever follows is a page to open, never a switch
        args.push_back(target);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    ::setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    ::unsetenv("DISPLAY");
    ::execv(engine.c_str(), argv.data());
}

}  // namespace

int webViewMain(int argc, char** argv);   // sdk/xweb/tools/xweb_view.cc

int main(int argc, char** argv) {
    std::string target = rawTarget(argc, argv);
    if (!target.starts_with(kChromium)) return webViewMain(argc, argv);

    target = target.substr(kChromium.size());
    if (!openable(target)) target.clear();
    const std::string engine = findEngine();
    if (!engine.empty()) runEngine(engine, target);

    // Still here: no engine, or it couldn't start.
    auto message = std::make_shared<TextWidget>(
        engine.empty() ? "The web browser isn't installed." : "The web browser couldn't start.", 16.0f);
    auto hint = std::make_shared<TextWidget>(
        engine.empty() ? "Install the chromium package to browse the web." : engine, 13.0f);
    hint->setColor(Color{0.65f, 0.65f, 0.70f, 1.0f});
    auto root = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        message,
        std::make_shared<Spacer>(1, 8),
        hint,
    });
    root->setBackgroundColor(Color{0.09f, 0.09f, 0.11f, 1.0f});
    root->setPadding(24);
    return XApplication::Run(root, "/tmp/freebsdx.sock", false, false, [](XBusClient&) {
        XApplication::setTitle("Web Browser");
    });
}
