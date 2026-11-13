#include "App.h"

#include "Form.h"
#include "Strings.h"

#include <mkcontract/Files.h>
#include <mkcontract/User.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace mkqa::tool {
namespace form = mktool::form;
namespace {

constexpr double kScanSeconds = 2.0; // new reports appear while the game runs
// Glyphs from engine docs/TOOL_DESIGN.md section 2 (Segoe Fluent Icons / MDL2 Assets)
constexpr const char* kIconRefresh = "\xEE\x9C\xAC";    // U+E72C
constexpr const char* kIconFolderOpen = "\xEE\xA0\xB8"; // U+E838 show in Explorer
constexpr const char* kIconLaunch = "\xEE\xA2\xA7";     // U+E8A7 open in its program
constexpr const char* kIconCopy = "\xEE\xA3\x88";       // U+E8C8
constexpr const char* kIconDelete = "\xEE\x9D\x8D";     // U+E74D delete files
constexpr const char* kIconPlay = "\xEE\x9D\xA8";       // U+E768
constexpr const char* kIconStop = "\xEE\x9C\x9A";       // U+E71A
constexpr const char* kIconLanguage = "\xEF\x8A\xB7";   // U+F2B7 LocaleLanguage

// Checkbox and radio colors (engine docs/TOOL_DESIGN.md 3.1, 3.2): a gray 4 box on a gray 2 panel is about 1.25:1, so an unchecked
// box gets a gray 9 outline (gray 11 hovered) and a checked one the accent fill with a gray 12 mark. Same as MkRender tool/App.cpp
const ImVec4 kControlOutline(0x8A / 255.0f, 0x8A / 255.0f, 0x8A / 255.0f, 1.0f);      // gray 9
const ImVec4 kControlOutlineHover(0xAF / 255.0f, 0xAF / 255.0f, 0xAF / 255.0f, 1.0f); // gray 11
const ImVec4 kControlMark(0xEB / 255.0f, 0xEB / 255.0f, 0xEB / 255.0f, 1.0f);         // gray 12
const ImVec4 kAccent(0x3B / 255.0f, 0x63 / 255.0f, 0xFB / 255.0f, 1.0f);
const ImVec4 kAccentHover(0x4B / 255.0f, 0x75 / 255.0f, 0xFF / 255.0f, 1.0f);
const ImVec4 kAccentDown(0x27 / 255.0f, 0x4D / 255.0f, 0xEA / 255.0f, 1.0f);

// Pushes the fill and mark of a checkbox or radio button in the state it is drawn in; popControlColors after
void pushControlColors(bool on) {
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, on ? kAccent : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, on ? kAccentHover : s.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, on ? kAccentDown : s.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kControlMark);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.FrameRounding * 0.5f); // a 4 px box, not the round 8 px of fields
}

void popControlColors() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

// Every checkbox goes through here, never ImGui::Checkbox (engine docs/TOOL_DESIGN.md 3.2)
[[maybe_unused]] bool checkbox(const char* label, bool* v) {
    const bool on = *v;
    pushControlColors(on);
    const bool changed = ImGui::Checkbox(label, v);
    const float rounding = ImGui::GetStyle().FrameRounding;
    popControlColors();
    if (!on) {
        // The box is the square at the start of the item (ImGui::Checkbox check_bb)
        const ImVec2 p = ImGui::GetItemRectMin();
        const float size = ImGui::GetFrameHeight();
        const float width = std::max(1.0f, ImGui::GetStyle().FontScaleDpi);
        const float inset = width * 0.5f;
        ImGui::GetWindowDrawList()->AddRect({p.x + inset, p.y + inset}, {p.x + size - inset, p.y + size - inset},
                                            ImGui::GetColorU32(ImGui::IsItemHovered() ? kControlOutlineHover : kControlOutline), rounding, 0, width);
    }
    return changed;
}

bool radioButton(const char* label, bool active) {
    pushControlColors(active);
    const bool pressed = ImGui::RadioButton(label, active);
    popControlColors();
    if (!active) {
        // Same circle as ImGui::RadioButton: radius (square - 1) / 2 around the square's center
        const ImVec2 p = ImGui::GetItemRectMin();
        const float size = ImGui::GetFrameHeight();
        const float width = std::max(1.0f, ImGui::GetStyle().FontScaleDpi);
        const ImVec2 center(std::floor(p.x + size * 0.5f + 0.5f), std::floor(p.y + size * 0.5f + 0.5f));
        ImGui::GetWindowDrawList()->AddCircle(center, (size - 1.0f) * 0.5f - width * 0.5f,
                                              ImGui::GetColorU32(ImGui::IsItemHovered() ? kControlOutlineHover : kControlOutline), 0, width);
    }
    return pressed;
}

// Suite language (engine docs/TOOL_DESIGN.md section 4 "Language"): the "language" line of editor_settings.txt, which MealKit Manager
// and every Mk tool read and write. Re-read once a second when the file changed, so a change anywhere shows here too
std::filesystem::file_time_type settingsTime() {
    std::error_code ec;
    const std::string path = mkcontract::userPath(mkcontract::kEditorSettings);
    if (path.empty()) return {};
    const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

void loadSuiteLanguage(std::filesystem::file_time_type& time) {
    const std::string code = mkcontract::editorSetting("language");
    setLanguage(code.empty() ? "ko" : code);
    time = settingsTime();
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool contains(const std::string& text, const char* query) { return !*query || lower(text).find(lower(query)) != std::string::npos; }

const char* kindLabel(Kind kind) {
    switch (kind) {
    case Kind::Bug: return tr("Bug report");
    case Kind::Crash: return tr("Crash");
    case Kind::Replay: return tr("Replay");
    }
    return "?";
}

ImVec4 kindColor(Kind kind) {
    switch (kind) {
    case Kind::Bug: return ImVec4(0.949f, 0.580f, 0.137f, 1.0f);
    case Kind::Crash: return ImVec4(1.0f, 0.384f, 0.314f, 1.0f);
    case Kind::Replay: return ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

std::string listKey(const std::vector<Entry>& entries) {
    std::string key;
    for (const Entry& e : entries) key += e.path + "|" + std::to_string(e.bytes) + "\n";
    return key;
}

std::string sizeText(uint64_t bytes) {
    char s[32];
    if (bytes >= 1024 * 1024) std::snprintf(s, sizeof(s), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(s, sizeof(s), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return s;
}

std::string folderOf(const std::string& path) { return mkcontract::utf8Of(mkcontract::pathOf(path).parent_path()); }

} // namespace

bool App::iconButton(const char* glyph, const char* name) {
    const std::string label = std::string(iconFont ? glyph : name) + "##" + name;
    // Flat like the editor's icon buttons: clear until hovered
    if (iconFont) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    }
    const bool pressed = iconFont ? ImGui::Button(label.c_str(), {ImGui::GetFrameHeight(), 0}) : ImGui::Button(label.c_str());
    if (iconFont) ImGui::PopStyleColor(2);
    if (iconFont && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", name);
    return pressed;
}


void App::init(const std::string& project, const std::string& select) {
    loadSuiteLanguage(m_settingsTime);
    if (!languageOverride.empty()) setLanguage(languageOverride);
    m_selectName = select;
    std::vector<std::string> folders;
    if (!project.empty()) folders.push_back(project);
    for (const std::string& f : mkcontract::recentProjects()) folders.push_back(f);
    for (const std::string& f : folders) {
        Project p;
        if (!readProject(f, p)) continue;
        bool seen = false;
        for (const Project& q : m_projects) seen = seen || mkcontract::sameDirectory(q.directory, p.directory);
        if (!seen) m_projects.push_back(p);
    }
    if (!m_projects.empty()) useProject(m_projects[0].directory);
    else if (!project.empty()) setStatus(trf("%s has no mealkit.project", project.c_str()), true);
}

void App::shutdown() {
    if (m_shot) SDL_DestroyTexture(m_shot);
    m_shot = nullptr;
}

std::string App::title() const {
    return m_project.valid ? "MkQA - " + (m_project.name.empty() ? std::string("MealKitGame") : m_project.name) : "MkQA";
}

void App::useProject(const std::string& directory) {
    for (const Project& p : m_projects)
        if (mkcontract::sameDirectory(p.directory, directory)) m_project = p;
    m_folder = gameFolder(m_project.name);
    m_listKey.clear();
    m_reports.clear();
    select(-1);
    refresh();
    m_results.clear();
    m_mp = {};
    m_testSelected.clear();
    refreshScenarios();
}

void App::refresh() {
    m_sinceScan = 0.0;
    const std::vector<Entry> entries = listEntries(m_folder);
    const std::string key = listKey(entries);
    if (key == m_listKey) return;
    m_listKey = key;
    const std::string selectedPath = m_selected >= 0 ? m_reports[static_cast<size_t>(m_selected)].entry.path : std::string();
    m_reports.clear();
    for (const Entry& e : entries) {
        Report r;
        loadReport(e, r); // a broken one stays in the list with its error
        m_reports.push_back(std::move(r));
    }
    int index = -1;
    for (size_t i = 0; i < m_reports.size(); ++i) {
        const Entry& e = m_reports[i].entry;
        if ((!selectedPath.empty() && e.path == selectedPath) || (!m_selectName.empty() && e.name == m_selectName)) index = static_cast<int>(i);
    }
    m_selectName.clear();
    if (index < 0 && !m_reports.empty()) index = 0;
    m_selected = -1; // reload the screenshot
    select(index);
}

void App::select(int index) {
    if (index == m_selected) return;
    m_selected = index;
    if (m_shot) SDL_DestroyTexture(m_shot);
    m_shot = nullptr;
    m_logFilter[0] = 0;
    if (index < 0 || !renderer) return;
    const Report& r = m_reports[static_cast<size_t>(index)];
    if (r.screenshot.empty()) return;
    SDL_IOStream* io = SDL_IOFromConstMem(r.screenshot.data(), r.screenshot.size());
    if (SDL_Surface* surface = io ? SDL_LoadPNG_IO(io, true) : nullptr) {
        m_shot = SDL_CreateTextureFromSurface(renderer, surface);
        m_shotW = static_cast<float>(surface->w);
        m_shotH = static_cast<float>(surface->h);
        SDL_DestroySurface(surface);
    }
}

void App::setStatus(const std::string& text, bool error) {
    m_status = text;
    m_statusError = error;
}

void App::frame(double dt) {
    m_sinceScan += dt;
    if (m_sinceScan >= kScanSeconds && m_project.valid) {
        refresh();
        refreshScenarios();
    }
    pollJobs();
    m_sinceLanguageCheck += dt;
    if (languageOverride.empty() && m_sinceLanguageCheck >= 1.0) { // MealKit Manager or another Mk tool changed the suite language
        m_sinceLanguageCheck = 0.0;
        if (settingsTime() != m_settingsTime) loadSuiteLanguage(m_settingsTime);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##qa", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawTop();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * form::dpi()));
    // The two pages as a segmented control (MealKit Manager's page switch), not an ImGui tab strip
    if (m_selectTests) m_testsPage = true;
    m_selectTests = false;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 3.0f * form::dpi()); // the track's padding
    form::beginSegments();
    if (form::segment(tr("Reports"), !m_testsPage)) m_testsPage = false;
    const std::string testsLabel = trf("Tests (%u)", static_cast<unsigned>(m_scenarios.size())) + "###tests";
    if (form::segment(testsLabel.c_str(), m_testsPage)) m_testsPage = true;
    form::endSegments();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * form::dpi()));
    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("##split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - statusHeight))) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (m_testsPage) drawTests();
        else drawList();
        ImGui::TableNextColumn();
        if (m_testsPage) drawTestDetail();
        else drawDetail();
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * form::dpi())); // space, not a line, above the status text
    if (m_status.empty()) ImGui::TextDisabled("%s", m_folder.c_str());
    else if (m_statusError) ImGui::TextColored(ImVec4(1.0f, 0.384f, 0.314f, 1.0f), "%s", m_status.c_str());
    else ImGui::TextUnformatted(m_status.c_str());
    drawDeletePopup();
    ImGui::End();
}

void App::drawTop() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Game"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    const std::string current = m_project.valid ? (m_project.name.empty() ? std::string("MealKitGame") : m_project.name) : tr("(no project)");
    if (ImGui::BeginCombo("##game", current.c_str())) {
        for (const Project& p : m_projects) {
            const std::string label = (p.name.empty() ? std::string("MealKitGame") : p.name) + "  " + p.directory;
            if (ImGui::Selectable(label.c_str(), mkcontract::sameDirectory(p.directory, m_project.directory))) useProject(p.directory);
        }
        if (m_projects.empty()) ImGui::TextDisabled("%s", tr("No recent projects. Open a project in the editor first."));
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (iconButton(kIconRefresh, tr("Refresh"))) {
        m_listKey.clear();
        refresh();
    }
    ImGui::SameLine();
    if (iconButton(kIconFolderOpen, tr("Open Folder")) && !m_folder.empty()) SDL_OpenURL(("file:///" + m_folder).c_str());
    if (m_testsPage) {
        drawLanguageButton();
        return; // the filters below are for the report list
    }
    // Kind filters as chips (tool/Form.h), not a row of checkboxes
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
    form::chip(tr("Bug reports"), &m_showBug);
    ImGui::SameLine();
    form::chip(tr("Crashes"), &m_showCrash);
    ImGui::SameLine();
    form::chip(tr("Replays"), &m_showReplay);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
    ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x)); // room for the language button
    ImGui::InputTextWithHint("##search", tr("Search note, scene, version"), m_search, sizeof(m_search));
    drawLanguageButton();
}

void App::drawLanguageButton() {
    // The name in English too while another language is on, so someone who can't read it still knows the button
    const std::string name = std::strcmp(language(), "en") == 0 ? std::string("Language") : std::string(tr("Language")) + " (Language)";
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()); // the right end, inside the padding
    if (iconButton(kIconLanguage, name.c_str())) ImGui::OpenPopup("##language");
    if (ImGui::BeginPopup("##language")) {
        for (const Language& l : kLanguages) {
            if (ImGui::MenuItem(l.name, nullptr, std::strcmp(language(), l.code) == 0)) {
                languageOverride.clear();
                setLanguage(l.code);
                if (mkcontract::setEditorSetting("language", l.code)) m_settingsTime = settingsTime();
                else {
                    setStatus(trf("Cannot save the language to %s", mkcontract::userPath(mkcontract::kEditorSettings).c_str()), true);
                }
            }
        }
        ImGui::EndPopup();
    }
}


void App::drawList() {
    if (!m_project.valid) {
        ImGui::TextWrapped("%s", tr("Pick a game. MkQA lists the projects you opened in the editor."));
        return;
    }
    if (m_reports.empty()) {
        ImGui::TextWrapped("%s", tr("No reports yet. Press F9 in the game to save a bug report; crashes and finished matches show up here too."));
        return;
    }
    if (!ImGui::BeginTable("##reports", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("When"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.5f);
    ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
    ImGui::TableSetupColumn(tr("Note"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < m_reports.size(); ++i) {
        const Report& r = m_reports[i];
        const Kind kind = r.entry.kind;
        if ((kind == Kind::Bug && !m_showBug) || (kind == Kind::Crash && !m_showCrash) || (kind == Kind::Replay && !m_showReplay)) continue;
        const std::string haystack = r.entry.name + " " + r.value("note") + " " + r.value("scene") + " " + r.value("gameVersion") + " " +
                                     r.value("engine") + " " + r.value("commit");
        if (!contains(haystack, m_search)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        const std::string when = r.entry.stamp.empty() ? r.entry.name : r.entry.stamp;
        if (ImGui::Selectable(when.c_str(), m_selected == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
            select(static_cast<int>(i));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openInEditor(r);
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextColored(kindColor(kind), "%s", kindLabel(kind));
        ImGui::TableNextColumn();
        if (!r.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.384f, 0.314f, 1.0f), "%s", r.error.c_str());
        else if (kind == Kind::Replay)
            ImGui::TextDisabled("%s", trf("%u player(s), %u s", r.recording.players, r.recording.ticks / std::max(1u, m_project.tickRate)).c_str());
        else if (!r.value("note").empty()) ImGui::TextUnformatted(r.value("note").c_str());
        else ImGui::TextDisabled("%s", trf("tick %s", r.value("tick").c_str()).c_str());
    }
    ImGui::EndTable();
}

void App::drawDetail() {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_reports.size())) return;
    const Report& r = m_reports[static_cast<size_t>(m_selected)];
    // Heading: when, large; the kind and size under it in the secondary step
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted((r.entry.stamp.empty() ? r.entry.name : r.entry.stamp).c_str());
    ImGui::PopFont();
    ImGui::TextColored(kindColor(r.entry.kind), "%s", kindLabel(r.entry.kind));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", sizeText(r.entry.bytes).c_str());
    ImGui::Dummy(ImVec2(0.0f, 4.0f * form::dpi()));

    const bool playable = r.recording.valid;
    ImGui::BeginDisabled(!playable);
    if (iconButton(kIconLaunch, tr("Open in Editor"))) openInEditor(r);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", playable ? tr("Plays the recording in the editor and pauses where the report was made")
                                         : tr("This report has no input recording (made outside a match)"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!playable);
    if (ImGui::Button(tr("Save as Scenario"))) saveAsScenario(r);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Turns the recording into a scenario test in the project's tests/ folder, with every state value at the report's tick as checks. Keep the checks that show the bug."));
    ImGui::SameLine();
    if (iconButton(kIconCopy, tr("Copy Info"))) {
        SDL_SetClipboardText(r.summaryText().c_str());
        setStatus(tr("Copied the report info. Paste it into the issue."), false);
    }
    ImGui::SameLine();
    if (iconButton(kIconFolderOpen, tr("Show File"))) SDL_OpenURL(("file:///" + folderOf(r.entry.path)).c_str());
    ImGui::SameLine();
    if (iconButton(kIconDelete, tr("Delete"))) m_confirmDelete = true;

    if (!r.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.384f, 0.314f, 1.0f), "%s", trf("Cannot read: %s", r.error.c_str()).c_str());
        return;
    }
    if (!r.value("note").empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("\"%s\"", r.value("note").c_str());
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("##info", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        const auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        for (const auto& [k, v] : r.info)
            if (k != "note") row(k.c_str(), v);
        if (r.recording.valid) {
            char seed[32];
            std::snprintf(seed, sizeof(seed), "%llu", static_cast<unsigned long long>(r.recording.seed));
            row(tr("recording"), trf("%u player(s), %u tick(s) (%.1f s), seed %s", r.recording.players, r.recording.ticks,
                                    static_cast<double>(r.recording.ticks) / std::max(1u, m_project.tickRate), seed));
        } else if (r.entry.kind != Kind::Replay) {
            row(tr("recording"), tr("none (made outside a match)"));
        }
        if (r.hasDump) row("crash.dmp", sizeText(r.dumpBytes));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (!ImGui::BeginTabBar("##tabs")) return;
    if (m_shot && ImGui::BeginTabItem(tr("Screenshot"))) {
        const float w = ImGui::GetContentRegionAvail().x;
        const float scale = std::min(1.0f, w / std::max(1.0f, m_shotW));
        ImGui::Image(reinterpret_cast<ImTextureID>(m_shot), ImVec2(m_shotW * scale, m_shotH * scale));
        ImGui::EndTabItem();
    }
    if (!r.crash.empty() && ImGui::BeginTabItem(tr("Crash"))) {
        ImGui::InputTextMultiline("##crash", const_cast<char*>(r.crash.c_str()), r.crash.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndTabItem();
    }
    if (!r.log.empty() && ImGui::BeginTabItem(tr("Log"))) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##logfilter", tr("Filter lines (e.g. WARN, ERROR)"), m_logFilter, sizeof(m_logFilter));
        if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            size_t start = 0;
            while (start < r.log.size()) {
                size_t end = r.log.find('\n', start);
                if (end == std::string::npos) end = r.log.size();
                const std::string line = r.log.substr(start, end - start);
                start = end + 1;
                if (!contains(line, m_logFilter)) continue;
                if (line.find("ERROR") != std::string::npos) ImGui::TextColored(ImVec4(1.0f, 0.384f, 0.314f, 1.0f), "%s", line.c_str());
                else if (line.find("WARN") != std::string::npos) ImGui::TextColored(ImVec4(0.949f, 0.580f, 0.137f, 1.0f), "%s", line.c_str());
                else ImGui::TextUnformatted(line.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void App::drawDeletePopup() {
    if (m_confirmDelete) {
        ImGui::OpenPopup("##delete");
        m_confirmDelete = false;
    }
    if (!ImGui::BeginPopupModal("##delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) return;
    if (m_selected < 0) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Entry entry = m_reports[static_cast<size_t>(m_selected)].entry;
    ImGui::TextUnformatted(trf("Delete %s? This can't be undone.", entry.name.c_str()).c_str());
    if (ImGui::Button(tr("Delete"))) {
        std::string error;
        if (removeEntry(entry, error)) setStatus(trf("Deleted %s", entry.name.c_str()), false);
        else setStatus(trf("Cannot delete %s: %s", entry.name.c_str(), error.c_str()), true);
        refresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::openInEditor(const Report& report) {
    if (!report.recording.valid) return;
    const std::string editor = editorExe(m_project.engine);
    if (editor.empty()) {
        setStatus(trf("The editor isn't built in %s (build\\Editor\\bin)", m_project.engine.c_str()), true);
        return;
    }
    const std::vector<std::string> args = editorReplayArgs(editor, m_project, report.entry.path);
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(argv.data(), false);
    if (!process) {
        setStatus(trf("Cannot start the editor: %s", SDL_GetError()), true);
        return;
    }
    SDL_DestroyProcess(process); // keeps running
    setStatus(trf("Opened %s in the editor", report.entry.name.c_str()), false);
}

namespace {

const char* stateLabel(ScenarioResult::State state) {
    switch (state) {
    case ScenarioResult::State::NotRun: return tr("not run");
    case ScenarioResult::State::Running: return tr("running");
    case ScenarioResult::State::Passed: return tr("passed");
    case ScenarioResult::State::Failed: return tr("failed");
    }
    return "?";
}

ImVec4 stateColor(ScenarioResult::State state) {
    switch (state) {
    case ScenarioResult::State::Passed: return ImVec4(0.176f, 0.710f, 0.455f, 1.0f);
    case ScenarioResult::State::Failed: return ImVec4(1.0f, 0.384f, 0.314f, 1.0f);
    case ScenarioResult::State::Running: return ImVec4(0.949f, 0.580f, 0.137f, 1.0f);
    case ScenarioResult::State::NotRun: break;
    }
    return ImVec4(0.686f, 0.686f, 0.686f, 1.0f);
}

std::string baseName(const std::string& path) { return mkcontract::utf8Of(mkcontract::pathOf(path).stem()); }

void outputBox(const char* id, const std::string& text) {
    ImGui::InputTextMultiline(id, const_cast<char*>(text.c_str()), text.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
}

} // namespace

void App::refreshScenarios() { m_scenarios = m_project.valid ? listScenarios(m_project) : std::vector<std::string>(); }

bool App::needBuild(const std::string& exe) {
    if (!exe.empty()) return false;
    setStatus(trf("Build the game first (build.bat in %s): no executables in build\\Release\\bin", buildRoot(m_project).c_str()), true);
    return true;
}

void App::runScenario(const std::string& file, bool updateExpected) {
    const std::string headless = headlessExe(m_project);
    if (needBuild(headless)) return;
    ScenarioResult& r = m_results[file];
    r = {};
    r.state = ScenarioResult::State::Running;
    m_scenarioJobs[m_runner.add(scenarioArgs(headless, file, updateExpected))] = file;
}

void App::runMpTest() {
    if (needBuild(gameExe(m_project))) return;
    m_mp = {};
    m_mp.line = tr("running...");
    m_mpJob = m_runner.add(mpTestArgs(m_project, static_cast<uint32_t>(m_mpClients), static_cast<uint32_t>(m_mpTicks)));
    m_testSelected = "mp_test";
}

void App::pollJobs() {
    for (Runner::Job& job : m_runner.takeFinished()) {
        if (job.id == m_mpJob) {
            m_mp = parseMpTestOutput(job.output, job.exitCode);
            m_mpJob = 0;
            continue;
        }
        const auto it = m_scenarioJobs.find(job.id);
        if (it == m_scenarioJobs.end()) continue;
        m_results[it->second] = parseScenarioOutput(job.output, job.exitCode);
        m_scenarioJobs.erase(it);
    }
}

void App::saveAsScenario(const Report& report) {
    if (needBuild(headlessExe(m_project))) return;
    std::string path, error;
    if (!writeScenarioFromReport(report, m_project, path, error)) {
        setStatus(trf("Cannot save the scenario: %s", error.c_str()), true);
        return;
    }
    runScenario(path, true); // writes the checks
    refreshScenarios();
    m_testSelected = path;
    m_selectTests = true;
    setStatus(trf("Saved tests/%s.scenario. Keep the checks that show the bug.", baseName(path).c_str()), false);
}

void App::drawTests() {
    if (!m_project.valid) {
        ImGui::TextWrapped("%s", tr("Pick a game. MkQA lists the projects you opened in the editor."));
        return;
    }
    ImGui::BeginDisabled(m_scenarios.empty());
    if (iconButton(kIconPlay, tr("Run All")))
        for (const std::string& f : m_scenarios) runScenario(f, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_runner.busy());
    if (iconButton(kIconStop, tr("Stop"))) {
        m_runner.cancelPending();
        for (auto& [id, file] : m_scenarioJobs)
            if (m_results[file].state == ScenarioResult::State::Running) m_results[file].state = ScenarioResult::State::NotRun;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Cancels the waiting runs (the running one finishes)"));
    unsigned passed = 0, failed = 0;
    for (const std::string& f : m_scenarios) {
        const auto it = m_results.find(f);
        if (it == m_results.end()) continue;
        passed += it->second.state == ScenarioResult::State::Passed;
        failed += it->second.state == ScenarioResult::State::Failed;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", trf("%u passed, %u failed", passed, failed).c_str());

    if (m_scenarios.empty())
        ImGui::TextWrapped("%s", tr("No scenario tests in tests/. Save one from a report ([Save as Scenario]) or from editor play."));
    else if (ImGui::BeginTable("##scenarios", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                               ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Scenario"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Result"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableHeadersRow();
        for (const std::string& f : m_scenarios) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(f.c_str());
            if (ImGui::Selectable(baseName(f).c_str(), m_testSelected == f, ImGuiSelectableFlags_SpanAllColumns)) m_testSelected = f;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) runScenario(f, false);
            ImGui::PopID();
            ImGui::TableNextColumn();
            const auto it = m_results.find(f);
            const ScenarioResult::State state = it == m_results.end() ? ScenarioResult::State::NotRun : it->second.state;
            ImGui::TextColored(stateColor(state), "%s", stateLabel(state));
        }
        ImGui::EndTable();
    }

    form::section(tr("Multiplayer test (mp_test)"));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputInt(tr("clients"), &m_mpClients);
    m_mpClients = std::clamp(m_mpClients, 1, 7);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputInt(tr("ticks"), &m_mpTicks, 300);
    m_mpTicks = std::clamp(m_mpTicks, 60, 100000);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_mpJob != 0);
    if (ImGui::Button(tr("Run mp_test"))) runMpTest();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Host + clients with bot input on this PC, headless: passes when every peer has the same state hash"));
    if (!m_mp.line.empty()) {
        const ScenarioResult::State state = m_mpJob != 0 ? ScenarioResult::State::Running
                                            : m_mp.passed ? ScenarioResult::State::Passed
                                                          : ScenarioResult::State::Failed;
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor(state));
        if (ImGui::Selectable(m_mp.line.c_str(), m_testSelected == "mp_test")) m_testSelected = "mp_test";
        ImGui::PopStyleColor();
    }
}

void App::drawTestDetail() {
    if (m_testSelected == "mp_test") {
        ImGui::TextUnformatted(m_mp.line.c_str());
        outputBox("##mpout", m_mp.output);
        return;
    }
    if (m_testSelected.empty()) {
        ImGui::TextDisabled("%s", tr("Pick a scenario. Double-click runs it."));
        return;
    }
    const std::string f = m_testSelected;
    const auto it = m_results.find(f);
    const ScenarioResult r = it == m_results.end() ? ScenarioResult{} : it->second;
    ImGui::TextUnformatted(baseName(f).c_str());
    ImGui::SameLine();
    ImGui::TextColored(stateColor(r.state), "%s", stateLabel(r.state));
    if (r.state == ScenarioResult::State::Passed || r.state == ScenarioResult::State::Failed) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", trf("%u check(s), %u failed", r.checks, r.failed).c_str());
    }
    ImGui::BeginDisabled(r.state == ScenarioResult::State::Running);
    if (iconButton(kIconPlay, tr("Run"))) runScenario(f, false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Update Expected"))) runScenario(f, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Rewrites the == values in the file with the current results (when the rules changed on purpose)"));
    ImGui::SameLine();
    if (iconButton(kIconFolderOpen, tr("Show File"))) SDL_OpenURL(("file:///" + folderOf(f)).c_str());
    for (const std::string& line : r.failures) ImGui::TextColored(ImVec4(1.0f, 0.384f, 0.314f, 1.0f), "%s", line.c_str());
    if (!ImGui::BeginTabBar("##testtabs")) return;
    if (ImGui::BeginTabItem(tr("File"))) {
        std::string text;
        mkcontract::readFile(f, text);
        outputBox("##file", text);
        ImGui::EndTabItem();
    }
    if (!r.output.empty() && ImGui::BeginTabItem(tr("Output"))) {
        outputBox("##out", r.output);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace mkqa::tool
