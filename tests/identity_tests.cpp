#include "identity.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace tt;
namespace {
int checks{};
void require(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
AppInfo source(const wchar_t *path) {
    return {"exe:" + utf8(identityPath(path)), L"Helper", path};
}
IdentityContext metadata(const wchar_t *product, const wchar_t *publisher) {
    return {{true, L"Helper", product, publisher}, {}, false};
}
} // namespace
int main() {
    try {
        IdentityCatalog catalog(
            {{"suite",
              L"Suite",
              L"C:\\Apps\\Suite",
              L"C:\\Apps\\Suite\\Suite.exe",
              L"Vendor",
              {},
              {}},
             {"editor",
              L"Editor",
              L"C:\\Apps\\Suite\\Editor",
              L"C:\\Apps\\Suite\\Editor\\Editor.exe",
              L"Vendor",
              {},
              {}},
             {"launcher",
              L"Launcher",
              L"C:\\Games\\Launcher",
              L"C:\\Games\\Launcher\\Launcher.exe",
              L"Launcher Inc",
              {},
              {}},
             {"broad", L"Broad", L"C:\\Program Files", {}, L"Vendor", {}, {}},
             {"browser",
              L"Browser",
              L"C:\\Browser",
              L"C:\\Browser\\browser.exe",
              L"Browser Vendor",
              {},
              {}},
             {"pwa-one", L"Web app one", {}, L"C:\\Browser\\browser.exe", {}, L"Pwa.One", {}},
             {"pwa-two", L"Web app two", {}, L"C:\\Browser\\browser.exe", {}, L"Pwa.Two", {}}});
        auto exact = catalog.identify(source(L"C:\\Apps\\Suite\\Suite.exe"), metadata(L"", L""));
        require(exact.representative.id == "app:install:suite", "exact registered target");
        auto helper = catalog.identify(source(L"C:\\Apps\\Suite\\bin\\helper.exe"),
                                       metadata(L"Suite Helper", L"Vendor"));
        require(helper.representative.id == exact.representative.id,
                "publisher and concrete install root associate helper");
        auto nested = catalog.identify(source(L"C:\\Apps\\Suite\\Editor\\bin\\helper.exe"),
                                       metadata(L"Editor Helper", L"Vendor"));
        require(nested.representative.id == "app:install:editor",
                "longest verified product root wins");
        auto game = catalog.identify(source(L"C:\\Games\\Launcher\\games\\game.exe"),
                                     metadata(L"UE4", L"Game Studio"));
        require(game.representative.id.empty(),
                "launcher does not absorb a separately published game");
        game = catalog.identify(source(L"C:\\Games\\Launcher\\games\\game.exe"),
                                metadata(L"Independent Game", L"Launcher Inc"));
        require(game.representative.id.empty(),
                "launcher does not absorb a separately named game from the same publisher");
        game = catalog.identify(source(L"C:\\Games\\Launcher\\games\\game.exe"),
                                metadata(L"UE4", L""));
        require(game.representative.id.empty(), "generic engine is insufficient evidence");
        auto broad = catalog.identify(source(L"C:\\Program Files\\Another\\helper.exe"),
                                      metadata(L"Electron", L"Vendor"));
        require(broad.representative.id.empty(), "shared roots are excluded");
        auto prefix = catalog.identify(source(L"C:\\Apps\\SuiteOther\\helper.exe"),
                                       metadata(L"Suite", L"Vendor"));
        require(prefix.representative.id.empty(), "root matching respects directory boundaries");
        auto missingContext = metadata(L"Suite", L"Vendor");
        missingContext.metadata.exists = false;
        auto missing = catalog.identify(source(L"C:\\Apps\\Suite\\Suite.exe"), missingContext);
        require(missing.representative.id.empty(),
                "missing historical executable stays unverified");
        auto sameProcess = source(L"C:\\Browser\\browser.exe");
        auto first = catalog.identify(sameProcess, {{true}, L"Pwa.One", true});
        auto second = catalog.identify(sameProcess, {{true}, L"Pwa.Two", true});
        require(first.representative.id == "app:aumid:Pwa.One" &&
                    second.representative.id == "app:aumid:Pwa.Two",
                "distinct window identities sharing a process stay separate");
        require(first.representative.name == L"Web app one",
                "AppID supplies friendly shortcut name");
        auto ambiguous = catalog.identify(sameProcess, metadata(L"Browser", L"Browser Vendor"));
        require(ambiguous.representative.id.empty(),
                "shared executable with conflicting AppIDs is ambiguous");
        auto explicitId = catalog.identify(source(L"C:\\Apps\\Suite\\Suite.exe"),
                                           {{true}, L"Separate.App", true});
        require(explicitId.representative.id == "app:aumid:Separate.App",
                "window identity precedes executable install root");
        auto stale =
            catalog.identify({"aumid:Removed.App", L"Removed", L"C:\\Apps\\Suite\\Suite.exe"},
                             metadata(L"Suite", L"Vendor"));
        require(stale.representative.id.empty(),
                "unverified historical AppID never falls back to shared host");
        auto logical =
            catalog.identify({"app:aumid:Pwa.One", L"Web app one", L"C:\\Browser\\browser.exe"},
                             metadata(L"Browser", L"Browser Vendor"));
        require(logical.representative.id.empty() && logical.evidence == "logical-group",
                "logical groups are never re-identified as shared executables");
        auto versionA = catalog.identify(source(L"C:\\Apps\\Suite\\1.0\\helper.exe"),
                                         metadata(L"Suite", L"Vendor"));
        auto versionB = catalog.identify(source(L"C:\\Apps\\Suite\\2.0\\helper.exe"),
                                         metadata(L"Suite", L"Vendor"));
        require(versionA.representative.id == versionB.representative.id &&
                    !versionA.representative.id.empty(),
                "version directories retain product identity");
        auto unknown = catalog.identify({"unknown:42:1", L"Unknown", {}}, {});
        require(unknown.representative.id.empty() && unknown.source.id == "unknown:42:1",
                "unknown identity retains raw source");
        require(identityPath(L"\\\\server\\share\\app.exe").empty(),
                "network targets are excluded");
        require(executableDisplayName({true, L"UE4", L"UE4", {}}, L"C:\\Games\\Client.exe") ==
                    L"Client",
                "generic engine descriptions use executable fallback");
        require(executableDisplayName({true, L"Ereader", L"Electron", {}}, L"C:\\Apps\\app.exe") ==
                    L"Ereader",
                "specific descriptions are retained");
        IdentityCatalog launchCatalog(
            {{"product",
              L"Product",
              L"C:\\Apps\\Product",
              L"C:\\Apps\\Product\\uninstall.exe",
              L"Vendor",
              {},
              {}},
             {"shortcut:product", L"Product", {}, L"C:\\Apps\\Product\\main.exe", {}, {}, {}}});
        auto launch = launchCatalog.identify(source(L"C:\\Apps\\Product\\main.exe"),
                                             metadata(L"Product", L"Vendor"));
        auto child = launchCatalog.identify(source(L"C:\\Apps\\Product\\bin\\helper.exe"),
                                            metadata(L"Product Helper", L"Vendor"));
        require(launch.representative.id == "app:install:product" &&
                    child.representative.id == launch.representative.id,
                "registered uninstaller icon and launch shortcut share product identity");
        wchar_t windowsDirectory[MAX_PATH]{};
        GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
        auto shellPath = std::wstring(windowsDirectory) + L"\\explorer.exe";
        IdentityCatalog sharedHost(
            {{"shortcut:folder", L"Product samples", {}, shellPath, {}, {}, {}}});
        require(sharedHost.identify(source(shellPath.c_str()), metadata(L"Windows", L"Microsoft"))
                    .representative.id.empty(),
                "folder shortcuts cannot rename a shared Windows host");
        std::vector<AppInfo> mergeApps{
            {"app:install:suite", L"Suite", L"C:\\Apps\\Suite\\Suite.exe"},
            {"other", L"Other", L"C:\\Apps\\Other\\app.exe"},
            {"old-helper", L"Helper", L"C:\\Apps\\Suite\\1.0\\helper.exe"},
            {"game", L"Game client", L"C:\\Apps\\Suite\\Game\\Binaries\\client.exe"},
            {"nested", L"Editor helper", L"C:\\Apps\\Suite\\Editor\\bin\\helper.exe"},
            {"prefix", L"Prefix", L"C:\\Apps\\SuiteOther\\helper.exe"},
            {"app:install:separate", L"Separate", L"C:\\Apps\\Suite\\separate.exe"},
            {"aumid:separate", L"Packaged application", L"C:\\Apps\\Suite\\host.exe"},
            {"unused", L"Unused", L"C:\\Apps\\Suite\\unused.exe", false},
            {"app:unidentified", L"Unknown", {}}};
        auto suggestions = catalog.mergeCandidates(mergeApps, "app:install:suite");
        require(suggestions.size() == 7,
                "Merge choices exclude current, unidentified and unused applications");
        require(suggestions[0].app.id == "old-helper" && suggestions[0].related &&
                    suggestions[1].app.id == "game" && suggestions[1].related,
                "Concrete install directory prioritizes old versions and unsigned clients");
        require(
            std::count_if(suggestions.begin(), suggestions.end(),
                          [](const auto &candidate) { return candidate.related; }) == 2,
            "Nested products, explicit identities and directory prefixes stay ordinary choices");
        require(suggestions[2].app.id == "other" && suggestions[2].app.path == mergeApps[1].path,
                "Unrelated choices retain original paths and stable order");
        auto rawMain = source(L"C:\\Apps\\Suite\\Suite.exe");
        auto rawCatalog = mergeApps;
        rawCatalog[0] = rawMain;
        require(catalog.mergeCandidates(rawCatalog, rawMain.id).front().app.id == "old-helper",
                "Raw main executable reuses its registered installation directory");
        auto unchangedGame = catalog.identify(
            source(L"C:\\Apps\\Suite\\Game\\Binaries\\client.exe"), metadata(L"UE4", L""));
        require(unchangedGame.representative.id.empty(),
                "Directory suggestions never change automatic application attribution");
        std::vector<AppInfo> unknownApps{
            {"current", L"App", L"C:\\Portable\\App\\main.exe"},
            {"peer", L"Peer", L"C:\\Portable\\App\\peer.exe"},
            {"subfolder", L"Child", L"C:\\Portable\\App\\Other\\child.exe"}};
        auto unknownSuggestions = catalog.mergeCandidates(unknownApps, "current");
        require(
            unknownSuggestions.size() == 2 && unknownSuggestions[0].related &&
                !unknownSuggestions[1].related,
            "Unregistered paths suggest only direct siblings, without widening directory scope");
        auto pwaSuggestions =
            catalog.mergeCandidates({{"app:aumid:Pwa.One", L"Web app", L"C:\\Browser\\browser.exe"},
                                     {"browser-helper", L"Helper", L"C:\\Browser\\helper.exe"}},
                                    "app:aumid:Pwa.One");
        require(pwaSuggestions.size() == 1 && !pwaSuggestions[0].related,
                "A browser app cannot suggest sibling programs from its shared host directory");
        auto nativeBrowserSuggestions =
            catalog.mergeCandidates({{"browser", L"Browser", L"C:\\Browser\\browser.exe"},
                                     {"browser-helper", L"Helper", L"C:\\Browser\\helper.exe"}},
                                    "browser");
        require(!nativeBrowserSuggestions[0].related,
                "Known shared browser hosts do not provide directory-wide merge suggestions");
        auto systemSuggestions = sharedHost.mergeCandidates(
            {{"shell", L"Explorer", shellPath},
             {"system-peer", L"Peer", std::wstring(windowsDirectory) + L"\\peer.exe"}},
            "shell");
        require(systemSuggestions.size() == 1 && !systemSuggestions[0].related,
                "System executables never suggest shared system-directory records");
        auto missingSuggestions = catalog.mergeCandidates(mergeApps, "missing");
        require(std::none_of(missingSuggestions.begin(), missingSuggestions.end(),
                             [](const auto &candidate) { return candidate.related; }),
                "Missing selection does not guess a directory");
        auto namedClient = mergeApps;
        namedClient[3].name = L"Suite";
        auto namedClientSuggestions = catalog.mergeCandidates(namedClient, "game");
        require(namedClientSuggestions[0].app.id == "app:install:suite" &&
                    namedClientSuggestions[0].related,
                "A same-name client inside a concrete install root can suggest its launcher");
        require(std::count_if(namedClientSuggestions.begin(), namedClientSuggestions.end(),
                              [](const auto &candidate) { return candidate.related; }) == 2,
                "Same-name directory scope retains old helper while excluding nested products");
        auto merelySimilar = mergeApps;
        merelySimilar[3].name = L"Suite Helper";
        auto merelySimilarSuggestions = catalog.mergeCandidates(merelySimilar, "game");
        require(std::none_of(merelySimilarSuggestions.begin(), merelySimilarSuggestions.end(),
                             [](const auto &candidate) { return candidate.related; }),
                "Similar names cannot widen an unregistered client's directory scope");
        auto nestedNamed = mergeApps;
        nestedNamed[4].name = L"Suite";
        auto nestedNamedSuggestions = catalog.mergeCandidates(nestedNamed, "nested");
        require(std::none_of(nestedNamedSuggestions.begin(), nestedNamedSuggestions.end(),
                             [](const auto &candidate) { return candidate.related; }),
                "A matching parent product name cannot cross a known nested product boundary");
        std::cout << "PASS: " << checks << " identity checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
