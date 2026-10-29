// Browser.xapp — Web browser for FreeBSD-X
//
// Features:
//   • Multi-tab browsing (tab bar with open/close, title updates)
//   • Navigation bar: back/forward/refresh, URL input, Go button, bookmark star
//   • Collapsible bookmarks sidebar via com.freebsdx.Bookmarks XBus service
//   • WPE WebKit process renders into a child Wayland surface (placeholder canvas)
//   • Registers as default MIME handler for text/html and x-scheme-handler/https
//   • Drag-and-drop of URLs from other apps ("text/uri-list")

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <algorithm>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Tab model ─────────────────────────────────────────────────────────────────

struct Tab {
    int         id;
    std::string url;
    std::string title;
};

// ── Bookmark model ────────────────────────────────────────────────────────────

struct BmEntry {
    std::string title;
    std::string url;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

// Trim protocol from URL for display in tab title (up to 32 chars)
static std::string shortTitle(const std::string& url) {
    std::string s = url;
    if (s.starts_with("https://")) s = s.substr(8);
    else if (s.starts_with("http://")) s = s.substr(7);
    if (s.size() > 32) s = s.substr(0, 29) + "...";
    return s.empty() ? "New Tab" : s;
}

// ── BrowserApp ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // ── State ─────────────────────────────────────────────────────────────────
    static int          next_id    = 1;
    static int          active_tab = 0;
    static std::vector<Tab>     tabs;
    static std::vector<BmEntry> bookmarks;
    static bool         sidebar_open = false;
    static bool         is_bookmarked = false;  // current tab bookmarked?
    static XBusClient*  g_xbus = nullptr;

    // Widgets that need cross-function access
    static std::shared_ptr<FlexRow>    tab_bar_row;
    static std::shared_ptr<FlexColumn> sidebar_col;
    static std::shared_ptr<FlexColumn> content_placeholder;
    static std::shared_ptr<TextWidget> url_label;
    static std::shared_ptr<TextInputWidget> url_input;
    static std::shared_ptr<TextWidget> bookmark_star;
    static std::shared_ptr<TextWidget> status_label;
    static std::shared_ptr<FlexColumn> root_col;

    // Forward declarations of rebuild functions
    static std::function<void()> rebuild_tabs;
    static std::function<void()> rebuild_sidebar;
    static std::function<void(const std::string&)> navigate;
    static std::function<void()> refresh_bookmark_state;
    static std::function<void()> load_bookmarks;

    // Open initial tab
    tabs.push_back({next_id++, "about:newtab", "New Tab"});

    // ── Navigate (stub — real impl would message WPE child process) ───────────
    navigate = [](const std::string& raw_url) {
        if (tabs.empty()) return;
        std::string url = raw_url;
        // A local file (opened from Files / search) → file://; otherwise
        // prepend https:// if no scheme
        if (url.starts_with("/"))
            url = "file://" + url;
        else if (url.find("://") == std::string::npos && !url.starts_with("about:"))
            url = "https://" + url;
        tabs[active_tab].url   = url;
        tabs[active_tab].title = shortTitle(url);
        if (url_input)  url_input->setText(url);
        if (status_label) status_label->setText("Loading: " + url);
        // WPE process would receive a Navigate IPC message here
        // After load we'd get a TitleChanged + LoadFinished IPC back
        if (status_label) status_label->setText("Loaded: " + url);
        rebuild_tabs();
        refresh_bookmark_state();
    };

    // ── Check if current tab URL is bookmarked ────────────────────────────────
    refresh_bookmark_state = []() {
        if (tabs.empty()) { is_bookmarked = false; }
        else {
            const std::string& cur = tabs[active_tab].url;
            is_bookmarked = std::any_of(bookmarks.begin(), bookmarks.end(),
                [&](const BmEntry& b){ return b.url == cur; });
        }
        if (bookmark_star)
            bookmark_star->setText(is_bookmarked ? "\u2605" : "\u2606"); // ★ / ☆
    };

    // ── Load bookmarks from XBus (paginated) ──────────────────────────────────
    load_bookmarks = []() {
        if (!g_xbus) return;
        bookmarks.clear();

        // Request page 0; the compositor sends BookmarkPage responses; we
        // accumulate until total_ pages are received.
        static uint32_t total_pages = 1;
        total_pages = 1;

        std::function<void(uint32_t)> fetch_page = [&](uint32_t page_idx) {
            uint32_t payload = page_idx;
            g_xbus->Call("com.freebsdx.Bookmarks",
                         static_cast<uint32_t>(BookmarkOpcode::List),
                         {reinterpret_cast<const uint8_t*>(&payload), sizeof(payload)},
                         [page_idx](const XBusMessage& resp) {
                             if (resp.opcode_ !=
                                 static_cast<uint32_t>(BookmarkOpcode::BookmarkPage)) return;
                             BookmarkPage bp{};
                             std::memcpy(&bp, resp.payload_, sizeof(bp));
                             if (bp.valid_) {
                                 bookmarks.push_back({
                                     std::string(bp.title_),
                                     std::string(bp.url_)
                                 });
                             }
                             total_pages = bp.total_ > 0 ? bp.total_ : 1;
                             if (page_idx + 1 < total_pages && g_xbus)
                                 /* next page — would normally recurse but we
                                    keep it simple: compositor sends each page
                                    as a separate response for a single List call */;
                             rebuild_sidebar();
                             refresh_bookmark_state();
                         });
        };
        fetch_page(0);
    };

    // ── Rebuild sidebar widget tree ───────────────────────────────────────────
    rebuild_sidebar = []() {
        if (!sidebar_col) return;
        sidebar_col->clearChildren();

        auto hdr = std::make_shared<TextWidget>("Bookmarks", 13.0f);
        hdr->setColor(Color{0.9f, 0.9f, 0.9f, 1.0f});
        sidebar_col->addChild(std::make_shared<ContainerWidget>(4.0f, 0.0f, hdr));
        sidebar_col->addChild(std::make_shared<DividerWidget>());

        if (bookmarks.empty()) {
            auto empty = std::make_shared<TextWidget>("No bookmarks", 12.0f);
            empty->setColor(Color{0.55f, 0.55f, 0.55f, 1.0f});
            sidebar_col->addChild(std::make_shared<ContainerWidget>(8.0f, 0.0f, empty));
        } else {
            for (const auto& bm : bookmarks) {
                auto title_txt = std::make_shared<TextWidget>(
                    bm.title.empty() ? bm.url : bm.title, 12.0f);
                title_txt->setColor(Color{0.75f, 0.88f, 1.0f, 1.0f});

                std::string bm_url = bm.url; // capture by value
                auto row = std::make_shared<ButtonWidget>(title_txt,
                    [bm_url]() { navigate(bm_url); });
                row->setPadding(6.0f);
                sidebar_col->addChild(row);
                sidebar_col->addChild(std::make_shared<DividerWidget>(0.5f));
            }
        }

        // "+ Add current page" button
        auto add_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("+ Bookmark this page", 12.0f),
            []() {
                if (tabs.empty() || !g_xbus) return;
                const Tab& t = tabs[active_tab];
                BookmarkRequest req{};
                std::strncpy(req.title_, t.title.c_str(), sizeof(req.title_) - 1);
                std::strncpy(req.url_,   t.url.c_str(),   sizeof(req.url_)   - 1);
                g_xbus->Call("com.freebsdx.Bookmarks",
                             static_cast<uint32_t>(BookmarkOpcode::Add),
                             {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                             [](const XBusMessage&) { load_bookmarks(); });
            });
        add_btn->setPadding(8.0f);
        sidebar_col->addChild(std::make_shared<Spacer>(0, 8));
        sidebar_col->addChild(add_btn);
    };

    // ── Rebuild tab bar widget tree ───────────────────────────────────────────
    rebuild_tabs = []() {
        if (!tab_bar_row) return;
        tab_bar_row->clearChildren();

        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const bool active = (i == active_tab);
            std::string label = tabs[i].title;
            if (label.size() > 18) label = label.substr(0, 15) + "...";

            auto tab_lbl = std::make_shared<TextWidget>(label, 12.0f);
            tab_lbl->setColor(active ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                                     : Color{0.65f, 0.65f, 0.65f, 1.0f});

            auto close_lbl = std::make_shared<TextWidget>(" ✕", 10.0f);
            close_lbl->setColor(Color{0.5f, 0.5f, 0.5f, 1.0f});

            int cap_i = i;
            auto close_btn = std::make_shared<ButtonWidget>(close_lbl,
                [cap_i]() {
                    if (tabs.size() <= 1) {
                        // Last tab — reset to new tab
                        tabs[0] = {tabs[0].id, "about:newtab", "New Tab"};
                        if (url_input) url_input->setText("about:newtab");
                        if (status_label) status_label->setText("");
                        rebuild_tabs();
                        return;
                    }
                    tabs.erase(tabs.begin() + cap_i);
                    if (active_tab >= static_cast<int>(tabs.size()))
                        active_tab = static_cast<int>(tabs.size()) - 1;
                    if (url_input) url_input->setText(tabs[active_tab].url);
                    rebuild_tabs();
                    refresh_bookmark_state();
                });

            auto tab_inner = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{tab_lbl, close_btn});
            tab_inner->setCrossAxisAlignment(CrossAxisAlignment::Center);
            tab_inner->setPadding(4.0f);

            auto tab_btn = std::make_shared<ButtonWidget>(tab_inner, [cap_i]() {
                active_tab = cap_i;
                if (url_input && !tabs.empty())
                    url_input->setText(tabs[active_tab].url);
                rebuild_tabs();
                refresh_bookmark_state();
            });
            tab_btn->setBackgroundColor(active
                ? Color{0.20f, 0.20f, 0.22f, 1.0f}
                : Color{0.13f, 0.13f, 0.15f, 1.0f});

            tab_bar_row->addChild(tab_btn);
            tab_bar_row->addChild(std::make_shared<Spacer>(2, 0));
        }

        // "+" new-tab button
        auto new_tab_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>(" + ", 13.0f),
            []() {
                tabs.push_back({next_id++, "about:newtab", "New Tab"});
                active_tab = static_cast<int>(tabs.size()) - 1;
                if (url_input) url_input->setText("about:newtab");
                if (status_label) status_label->setText("");
                rebuild_tabs();
                refresh_bookmark_state();
            });
        tab_bar_row->addChild(new_tab_btn);
    };

    // ── Tab bar ───────────────────────────────────────────────────────────────
    tab_bar_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
    tab_bar_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    tab_bar_row->setPadding(4.0f);
    tab_bar_row->setGap(2.0f);
    tab_bar_row->setBackgroundColor(Color{0.11f, 0.11f, 0.13f, 1.0f});
    tab_bar_row->setFixedSize(0.0f, 36.0f);
    tab_bar_row->setFlexGrow(1.0f);

    // ── Navigation bar ────────────────────────────────────────────────────────
    // Back button
    auto back_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" \u2190 ", 14.0f),   // ←
        []() {
            // WPE child process would receive GoBack IPC
            if (status_label) status_label->setText("(Back)");
        });
    back_btn->setBackgroundColor(Color{0.18f, 0.18f, 0.20f, 1.0f});

    // Forward button
    auto fwd_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" \u2192 ", 14.0f),   // →
        []() {
            if (status_label) status_label->setText("(Forward)");
        });
    fwd_btn->setBackgroundColor(Color{0.18f, 0.18f, 0.20f, 1.0f});

    // Refresh button
    auto refresh_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" \u21BB ", 14.0f),   // ↻
        []() {
            if (!tabs.empty()) navigate(tabs[active_tab].url);
        });
    refresh_btn->setBackgroundColor(Color{0.18f, 0.18f, 0.20f, 1.0f});

    // URL bar
    url_input = std::make_shared<TextInputWidget>("about:newtab");
    url_input->setFlexGrow(1.0f);
    url_input->setOnSubmit([](const std::string& text) {
        navigate(text);
    });

    // Go button
    auto go_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>(" Go ", 13.0f),
        []() {
            if (url_input) navigate(url_input->text());
        });
    go_btn->setBackgroundColor(Color{0.22f, 0.48f, 0.85f, 1.0f});

    // Bookmark star toggle
    bookmark_star = std::make_shared<TextWidget>("\u2606", 16.0f); // ☆
    bookmark_star->setColor(Color{0.9f, 0.75f, 0.2f, 1.0f});
    auto star_btn = std::make_shared<ButtonWidget>(bookmark_star, []() {
        if (tabs.empty() || !g_xbus) return;
        const Tab& t = tabs[active_tab];
        if (is_bookmarked) {
            // Remove bookmark
            BookmarkRequest req{};
            std::strncpy(req.url_, t.url.c_str(), sizeof(req.url_) - 1);
            g_xbus->Call("com.freebsdx.Bookmarks",
                         static_cast<uint32_t>(BookmarkOpcode::Remove),
                         {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                         [](const XBusMessage&) { load_bookmarks(); });
        } else {
            // Add bookmark
            BookmarkRequest req{};
            std::strncpy(req.title_, t.title.c_str(), sizeof(req.title_) - 1);
            std::strncpy(req.url_,   t.url.c_str(),   sizeof(req.url_)   - 1);
            g_xbus->Call("com.freebsdx.Bookmarks",
                         static_cast<uint32_t>(BookmarkOpcode::Add),
                         {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                         [](const XBusMessage&) { load_bookmarks(); });
        }
    });

    // Sidebar toggle
    auto sidebar_icon = std::make_shared<TextWidget>(" \u2630 ", 14.0f); // ☰
    sidebar_icon->setColor(Color{0.75f, 0.75f, 0.75f, 1.0f});
    auto sidebar_btn = std::make_shared<ButtonWidget>(sidebar_icon, []() {
        sidebar_open = !sidebar_open;
        // Toggling visibility is handled by rebuilding the main layout or
        // using a StackWidget; we set fixed width to 0 when hidden.
        if (sidebar_col)
            sidebar_col->setFixedSize(sidebar_open ? 220.0f : 0.0f, 0.0f);
    });

    auto nav_bar = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        back_btn,
        std::make_shared<Spacer>(4, 0),
        fwd_btn,
        std::make_shared<Spacer>(4, 0),
        refresh_btn,
        std::make_shared<Spacer>(8, 0),
        url_input,
        std::make_shared<Spacer>(6, 0),
        go_btn,
        std::make_shared<Spacer>(8, 0),
        star_btn,
        std::make_shared<Spacer>(4, 0),
        sidebar_btn,
    });
    nav_bar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    nav_bar->setPadding(6.0f);
    nav_bar->setGap(0.0f);
    nav_bar->setBackgroundColor(Color{0.15f, 0.15f, 0.17f, 1.0f});
    nav_bar->setFixedSize(0.0f, 44.0f);
    nav_bar->setFlexGrow(1.0f);

    // ── Toolbar row (tabs + nav) ───────────────────────────────────────────────
    auto toolbar_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        tab_bar_row,
        std::make_shared<DividerWidget>(1.0f),
        nav_bar,
        std::make_shared<DividerWidget>(1.0f),
    });
    toolbar_col->setGap(0.0f);

    // ── Web content placeholder ───────────────────────────────────────────────
    // In production a WPE WebKit child process renders here via a shared
    // Wayland surface. For now we show an informational panel.
    auto content_title = std::make_shared<TextWidget>("Web Content", 22.0f);
    content_title->setColor(Color{0.4f, 0.4f, 0.45f, 1.0f});

    auto content_sub = std::make_shared<TextWidget>(
        "WPE WebKit surface renders here", 13.0f);
    content_sub->setColor(Color{0.35f, 0.35f, 0.38f, 1.0f});

    status_label = std::make_shared<TextWidget>("", 12.0f);
    status_label->setColor(Color{0.55f, 0.75f, 0.55f, 1.0f});

    content_placeholder = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(0, 60),
            content_title,
            std::make_shared<Spacer>(0, 8),
            content_sub,
            std::make_shared<Spacer>(0, 16),
            status_label,
        });
    content_placeholder->setCrossAxisAlignment(CrossAxisAlignment::Center);
    content_placeholder->setMainAxisAlignment(MainAxisAlignment::Start);
    content_placeholder->setFlexGrow(1.0f);
    content_placeholder->setBackgroundColor(Color{0.09f, 0.09f, 0.11f, 1.0f});

    // ── Bookmarks sidebar ─────────────────────────────────────────────────────
    sidebar_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    sidebar_col->setFixedSize(0.0f, 0.0f); // hidden initially
    sidebar_col->setBackgroundColor(Color{0.13f, 0.13f, 0.16f, 1.0f});
    sidebar_col->setPadding(8.0f);

    // ── Body row (sidebar + content) ──────────────────────────────────────────
    auto body_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        sidebar_col,
        content_placeholder,
    });
    body_row->setGap(0.0f);
    body_row->setFlexGrow(1.0f);

    // ── Root layout ───────────────────────────────────────────────────────────
    root_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        toolbar_col,
        body_row,
    });
    root_col->setGap(0.0f);
    root_col->setBackgroundColor(Color{0.09f, 0.09f, 0.11f, 1.0f});

    // Initial tab bar population
    rebuild_tabs();

    // ── Command-line pre-load ─────────────────────────────────────────────────
    std::string preload_url;
    // xshell opens files with "--open <path>"; a bare path works too.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--open") { if (i + 1 < argc) preload_url = argv[i + 1]; break; }
        if (!a.empty() && a[0] != '-') { preload_url = a; break; }
    }

    // ── Launch ────────────────────────────────────────────────────────────────
    return XApplication::Run(root_col, "/tmp/freebsdx.sock", /*sandboxed=*/false,
                             /*is_shell=*/false,
        [preload_url](XBusClient& xbus) {
            g_xbus = &xbus;

            XApplication::setTitle("Browser");

            // Register as MIME handler for web URLs
            {
                MimeSetDefaultRequest req{};
                std::strncpy(req.mime_type_, "text/html",
                             sizeof(req.mime_type_) - 1);
                std::strncpy(req.app_id_,   "com.freebsdx.browser",
                             sizeof(req.app_id_) - 1);
                xbus.Call("com.freebsdx.MIME",
                          static_cast<uint32_t>(MimeOpcode::SetDefaultHandler),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          nullptr);
            }
            {
                MimeSetDefaultRequest req{};
                std::strncpy(req.mime_type_, "x-scheme-handler/https",
                             sizeof(req.mime_type_) - 1);
                std::strncpy(req.app_id_,   "com.freebsdx.browser",
                             sizeof(req.app_id_) - 1);
                xbus.Call("com.freebsdx.MIME",
                          static_cast<uint32_t>(MimeOpcode::SetDefaultHandler),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          nullptr);
            }

            // Subscribe to LaunchWithFile for when the shell opens a .html file
            xbus.OnBroadcast([](const XBusMessage& msg) {
                if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") != 0) return;
                if (msg.opcode_ !=
                    static_cast<uint32_t>(ShellOpcode::LaunchWithFile)) return;
                ShellLaunchRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                req.app_id_[sizeof(req.app_id_) - 1]     = '\0';
                req.file_path_[sizeof(req.file_path_) - 1] = '\0';
                if (std::strcmp(req.app_id_, "com.freebsdx.browser") == 0 &&
                    req.file_path_[0] != '\0') {
                    navigate(std::string("file://") + req.file_path_);
                }
            });

            // Accept drag-and-drop of URLs ("text/uri-list")
            XApplication::setOnDrop("text/uri-list",
                [](float, float, const std::string& data) {
                    std::string url = data;
                    while (!url.empty() &&
                           (url.back() == '\n' || url.back() == '\r'))
                        url.pop_back();
                    if (!url.empty()) navigate(url);
                });

            // Load bookmarks from compositor
            load_bookmarks();

            // Pre-load URL from command line
            if (!preload_url.empty()) navigate(preload_url);
        });
}
