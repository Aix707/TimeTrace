#include "identity.h"
#include <windows.h>
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
        std::cout << "PASS: " << checks << " identity checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
