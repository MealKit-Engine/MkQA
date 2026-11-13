// MkQA executable: SDL3 window + SDL_Renderer + Dear ImGui (same framework as MkAudio/Manager). The actual screen is App.
//   MkQA [--project <MealKit project folder>] [--select <report name>] [--lang ko|en] [--tests] [--frames N --capture <png>]
//   MkQA --list [--project <folder>]   prints the game's reports and replays without a window (for tests/scripts)
//   MkQA --run-tests [--project <folder>] [--mp-test]   runs tests/*.scenario (and mp_test) without a window, exit code 1 on a failure
//   MkQA --make-scenario <report name> [--project <folder>]   tests/<kind>_<name>.scenario from a report's recording, checks filled
//   MkQA --version
#include "App.h"
#include "Strings.h"

#include <mkcontract/User.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

// Merges a system font + a Korean font (ImGui's default vector font if none)
bool loadFonts(float size) {
    ImGuiIO& io = ImGui::GetIO();
    const char* latin[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    const char* korean[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\malgun.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
#else
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
#endif
    };
    bool loaded = false;
    for (const char* path : latin)
        if (!loaded && SDL_GetPathInfo(path, nullptr) && io.Fonts->AddFontFromFileTTF(path, size)) loaded = true;
    if (!loaded) {
        ImFontConfig config;
        config.SizePixels = size;
        io.Fonts->AddFontDefaultVector(&config);
    }
    ImFontConfig merge;
    merge.MergeMode = true;
    for (const char* path : korean)
        if (SDL_GetPathInfo(path, nullptr) && io.Fonts->AddFontFromFileTTF(path, size, &merge)) break;
#if defined(_WIN32)
    // Icon font (engine docs/TOOL_DESIGN.md section 2): Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on 10. Returns whether it loaded
    for (const char* icons : {"C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf"}) {
        ImFontConfig config;
        config.MergeMode = true;
        config.GlyphOffset = ImVec2(0.0f, 3.0f);
        if (SDL_GetPathInfo(icons, nullptr) && io.Fonts->AddFontFromFileTTF(icons, size, &config)) return true;
    }
#endif
    return false;
}

// The MealKit editor's look (module-debug src/mealkit/debug/ImGuiLayer.cpp applyStyle), copied since MkQA doesn't link the engine
// (engine docs/TOOL_DESIGN.md section 3). Keep it the same as the original.
void applyEditorStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    // Spectrum 2 shape (engine docs/TOOL_DESIGN.md 3.1): 8 px fields, 10 px panels and popups.
    // Panels are separated by space and a darker gutter, not by lines
    s.WindowRounding = 10.0f;
    s.ChildRounding = 10.0f;
    s.PopupRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.ScrollbarSize = 10.0f;
    s.WindowBorderSize = 0.0f; // layers are told apart by color, not outlines
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBarBorderSize = 0.0f;
    s.TabBarOverlineSize = 0.0f;
    s.DockingSeparatorSize = 4.0f;
    s.DockingNodeHasCloseButton = false; // each tab has its own close button
    s.TabCloseButtonMinWidthSelected = 0.0f; // tabs show it only while hovered
    s.TabCloseButtonMinWidthUnselected = 0.0f;
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(7.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.CellPadding = ImVec2(6.0f, 3.0f);
    s.IndentSpacing = 16.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None; // no triangle on every dock tab bar: tabs are all visible and can be dragged
    // Tokens of engine docs/TOOL_DESIGN.md 3.1: Spectrum 2 dark gray-25..gray-900 (gray 1..12) and blue. Pick a step by its job, never a color by eye
    const auto rgb = [](unsigned v, float a = 1.0f) {
        return ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a);
    };
    const auto gray = [&](int step) {
        static constexpr unsigned kGray[13] = {0, 0x111111, 0x1B1B1B, 0x222222, 0x2C2C2C, 0x323232, 0x393939,
                                          0x444444, 0x6D6D6D, 0x8A8A8A, 0x909090, 0xAFAFAF, 0xEBEBEB};
        return rgb(kGray[step]);
    };
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 focus = rgb(0x5D89FF);  // keyboard focus, hovered splitters
    const ImVec4 accent = rgb(0x3B63FB); // selection, check marks, active grabs
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = gray(12);
    c[ImGuiCol_TextDisabled] = gray(11); // secondary text: paths, dates, hints
    c[ImGuiCol_TextSelectedBg] = rgb(0x3B63FB, 0.45f);
    c[ImGuiCol_WindowBg] = gray(2);
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = gray(3);
    c[ImGuiCol_Border] = gray(6);
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_MenuBarBg] = gray(1);
    c[ImGuiCol_TitleBg] = gray(1);
    c[ImGuiCol_TitleBgActive] = gray(1);
    c[ImGuiCol_TitleBgCollapsed] = gray(1);
    // Hover is one neutral step up, pressed and selected one more; the accent never fills a hovered control
    c[ImGuiCol_FrameBg] = gray(4); // fields and buttons share a step, so a combo and its arrow read as one control
    c[ImGuiCol_FrameBgHovered] = gray(5);
    c[ImGuiCol_FrameBgActive] = gray(6);
    c[ImGuiCol_Header] = gray(5);
    c[ImGuiCol_HeaderHovered] = gray(4);
    c[ImGuiCol_HeaderActive] = gray(6);
    c[ImGuiCol_Button] = gray(4);
    c[ImGuiCol_ButtonHovered] = gray(6);
    c[ImGuiCol_ButtonActive] = gray(7);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = gray(9);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Separator] = gray(5);
    c[ImGuiCol_SeparatorHovered] = focus;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = focus;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = gray(6);
    c[ImGuiCol_ScrollbarGrabHovered] = gray(7);
    c[ImGuiCol_ScrollbarGrabActive] = gray(8);
    // The selected tab joins its panel; the others sit on the strip
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = gray(4);
    c[ImGuiCol_TabSelected] = gray(2);
    c[ImGuiCol_TabSelectedOverline] = clear;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = gray(2);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_TableHeaderBg] = clear; // no header band: quiet header text over a line
    c[ImGuiCol_TableBorderStrong] = gray(6);
    c[ImGuiCol_TableBorderLight] = gray(5);
    c[ImGuiCol_TableRowBgAlt] = clear; // no zebra rows
    c[ImGuiCol_DockingPreview] = rgb(0x3B63FB, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = clear;
    c[ImGuiCol_NavCursor] = focus;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// Dark title bar in gray 1 #111111 (editor kEditorCaptionColor, docs/TOOL_DESIGN.md 3.1). Ignored where Windows doesn't support it
void applyEditorCaption(SDL_Window* window) {
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x11, 0x11, 0x11);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
#else
    (void)window;
#endif
}

bool openProject(const std::string& projectArg, mkqa::Project& project) {
    std::string directory = projectArg;
    if (directory.empty()) {
        const std::vector<std::string> recent = mkcontract::recentProjects();
        if (!recent.empty()) directory = recent[0];
    }
    if (mkqa::readProject(directory, project)) return true;
    std::printf("No project (use --project <folder>)\n");
    return false;
}

// One command line, waiting for it
mkqa::tool::Runner::Job runNow(std::vector<std::string> args) {
    mkqa::tool::Runner runner;
    runner.add(std::move(args));
    for (;;) {
        std::vector<mkqa::tool::Runner::Job> done = runner.takeFinished();
        if (!done.empty()) return done[0];
        SDL_Delay(20);
    }
}

int runTests(const std::string& projectArg, bool mpTest) {
    mkqa::Project project;
    if (!openProject(projectArg, project)) return 1;
    const std::string headless = mkqa::headlessExe(project);
    if (headless.empty()) {
        std::printf("Build the game first: no %s_headless in %s/build/Release/bin\n", project.name.c_str(), mkqa::buildRoot(project).c_str());
        return 1;
    }
    int failed = 0;
    for (const std::string& file : mkqa::listScenarios(project)) {
        const mkqa::tool::Runner::Job job = runNow(mkqa::scenarioArgs(headless, file, false));
        const mkqa::ScenarioResult r = mkqa::parseScenarioOutput(job.output, job.exitCode);
        const bool ok = r.state == mkqa::ScenarioResult::State::Passed;
        failed += !ok;
        std::printf("%s %s checks=%u\n", ok ? "ok  " : "FAIL", file.c_str(), r.checks);
        for (const std::string& line : r.failures) std::printf("     %s\n", line.c_str());
    }
    if (mpTest) {
        const mkqa::tool::Runner::Job job = runNow(mkqa::mpTestArgs(project, 3, 900));
        const mkqa::MpTestResult r = mkqa::parseMpTestOutput(job.output, job.exitCode);
        failed += !r.passed;
        std::printf("mp_test %s\n", r.line.c_str());
    }
    std::fflush(stdout);
    return failed ? 1 : 0;
}

int makeScenario(const std::string& projectArg, const std::string& name) {
    mkqa::Project project;
    if (!openProject(projectArg, project)) return 1;
    const std::string headless = mkqa::headlessExe(project);
    for (const mkqa::Entry& e : mkqa::listEntries(mkqa::gameFolder(project.name))) {
        if (e.name != name) continue;
        mkqa::Report report;
        mkqa::loadReport(e, report);
        std::string path, error;
        if (headless.empty()) error = "the game's headless executable isn't built";
        else if (mkqa::writeScenarioFromReport(report, project, path, error)) {
            const mkqa::tool::Runner::Job job = runNow(mkqa::scenarioArgs(headless, path, true));
            const mkqa::ScenarioResult r = mkqa::parseScenarioOutput(job.output, job.exitCode);
            std::printf("%s %s checks=%u\n", r.state == mkqa::ScenarioResult::State::Passed ? "saved" : "FAIL", path.c_str(), r.checks);
            return r.state == mkqa::ScenarioResult::State::Passed ? 0 : 1;
        }
        std::printf("Cannot save the scenario: %s\n", error.c_str());
        return 1;
    }
    std::printf("No report named %s\n", name.c_str());
    return 1;
}

// --list: the project's (else the most recent project's) game folder
int printList(const std::string& projectArg) {
    std::string directory = projectArg;
    if (directory.empty()) {
        const std::vector<std::string> recent = mkcontract::recentProjects();
        if (!recent.empty()) directory = recent[0];
    }
    mkqa::Project project;
    if (!mkqa::readProject(directory, project)) {
        std::printf("No project (use --project <folder>)\n");
        return 1;
    }
    const std::string folder = mkqa::gameFolder(project.name);
    std::printf("game: %s %s\n", project.name.c_str(), folder.c_str());
    for (const mkqa::Entry& e : mkqa::listEntries(folder)) {
        mkqa::Report r;
        mkqa::loadReport(e, r);
        std::printf("%s %s", mkqa::kindName(e.kind), e.name.c_str());
        if (!r.error.empty()) std::printf(" error: %s", r.error.c_str());
        if (r.recording.valid) std::printf(" players %u ticks %u", r.recording.players, r.recording.ticks);
        if (!r.value("tick").empty()) std::printf(" tick %s", r.value("tick").c_str());
        if (!r.value("note").empty()) std::printf(" note \"%s\"", r.value("note").c_str());
        std::printf("\n");
    }
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string project, capture, select, language;
    long frames = 0; // --frames N: quit after N frames (--capture saves the last screen as PNG, for docs/tests)
    bool list = false, runTestsArg = false, mpTest = false, testsPage = false;
    std::string makeScenarioArg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) select = argv[++i];
        else if (std::strcmp(argv[i], "--lang") == 0 && i + 1 < argc) language = argv[++i];
        else if (std::strcmp(argv[i], "--list") == 0) list = true;
        else if (std::strcmp(argv[i], "--run-tests") == 0) runTestsArg = true;
        else if (std::strcmp(argv[i], "--tests") == 0) testsPage = true;
        else if (std::strcmp(argv[i], "--mp-test") == 0) mpTest = true;
        else if (std::strcmp(argv[i], "--make-scenario") == 0 && i + 1 < argc) makeScenarioArg = argv[++i];
        else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("MkQA %s\n", MKQA_VERSION);
            return 0;
        }
    }
    if (list) return printList(project);
    if (runTestsArg) return runTests(project, mpTest);
    if (!makeScenarioArg.empty()) return makeScenario(project, makeScenarioArg);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("MkQA", static_cast<int>(1300 * scale), static_cast<int>(820 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_Log("Cannot create the window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    applyEditorCaption(window);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Window layout goes in the user folder
    static std::string ini;
    if (char* pref = SDL_GetPrefPath("MealKit", "MealKit QA") /* kept: settings folder from before the MkQA rename */) {
        ini = std::string(pref) + "imgui.ini";
        SDL_free(pref);
        io.IniFilename = ini.c_str();
    }
    applyEditorStyle(scale);
    const bool iconFont = loadFonts(16.0f);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    mkqa::tool::App app;
    app.window = window;
    app.iconFont = iconFont;
    app.renderer = renderer;
    app.languageOverride = language; // --lang: this run only, the suite setting isn't followed then
    app.init(project, select);
    if (testsPage) app.showTests();

    Uint64 last = SDL_GetPerformanceCounter();
    std::string shownTitle;
    long frame = 0;
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) running = false;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - last) / static_cast<double>(SDL_GetPerformanceFrequency());
        last = now;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame(dt);
        ImGui::Render();

        const std::string title = app.title();
        if (title != shownTitle) {
            SDL_SetWindowTitle(window, title.c_str());
            shownTitle = title;
        }
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, 1.0f); // gray 1
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (frames > 0 && ++frame >= frames) {
            if (!capture.empty()) {
                SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
                if (!shot || !SDL_SavePNG(shot, capture.c_str())) SDL_Log("Cannot save %s: %s", capture.c_str(), SDL_GetError());
                SDL_DestroySurface(shot);
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
    }

    app.shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
