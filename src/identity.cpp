#include "identity.h"
#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <unordered_map>
#include <winver.h>

namespace tt {
namespace {
std::wstring folded(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    while (!value.empty() && iswspace(value.back()))
        value.pop_back();
    auto first = value.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : value.substr(first);
}
bool localPath(const std::wstring &path) {
    if (path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/'))
        return false;
    wchar_t drive[]{path[0], L':', L'\\', 0};
    auto type = GetDriveTypeW(drive);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE;
}
bool inside(const std::wstring &path, const std::wstring &root) {
    return !root.empty() && path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
           path[root.size()] == L'\\';
}
bool hasTarget(const IdentityProduct &product, const std::wstring &path) {
    return !path.empty() &&
           (product.primaryPath == path || std::find(product.targets.begin(), product.targets.end(),
                                                     path) != product.targets.end());
}
bool specificRoot(const std::wstring &root) {
    if (root.size() < 6)
        return false;
    static const wchar_t *shared[]{L"windows",     L"program files", L"program files (x86)",
                                   L"programdata", L"users",         L"appdata",
                                   L"local",       L"roaming",       L"programs",
                                   L"common",      L"steamapps",     L"applications"};
    auto leaf = std::filesystem::path(root).filename().wstring();
    return std::none_of(std::begin(shared), std::end(shared),
                        [&](auto name) { return leaf == name; });
}
bool genericName(const std::wstring &name) {
    auto n = folded(name);
    static const wchar_t *generic[]{L"electron",
                                    L"ue4",
                                    L"ue5",
                                    L"unreal engine",
                                    L"chromium",
                                    L"cef",
                                    L"cefclient",
                                    L"microsoft windows operating system",
                                    L"microsoft® windows® operating system",
                                    L"webview2",
                                    L"microsoft edge webview2",
                                    L"application",
                                    L"app",
                                    L"setup",
                                    L"update",
                                    L"updater"};
    return n.empty() || std::any_of(std::begin(generic), std::end(generic),
                                    [&](auto item) { return n == item; });
}
bool sameProduct(const std::wstring &a, const std::wstring &b) {
    auto first = folded(a), second = folded(b);
    if (genericName(first) || genericName(second))
        return false;
    if (first == second)
        return true;
    auto prefix = [](const std::wstring &longer, const std::wstring &shorter) {
        return longer.size() > shorter.size() && longer.compare(0, shorter.size(), shorter) == 0 &&
               std::wstring(L" .-_([{").find(longer[shorter.size()]) != std::wstring::npos;
    };
    return prefix(first, second) || prefix(second, first);
}
bool componentExecutable(const std::wstring &path) {
    auto name = folded(std::filesystem::path(path).stem().wstring());
    const wchar_t *roles[]{L"helper",    L"sdk",     L"webview", L"webclient",
                           L"cefclient", L"install", L"updater", L"crashhandler"};
    return std::any_of(std::begin(roles), std::end(roles),
                       [&](auto role) { return name.find(role) != std::wstring::npos; });
}
std::wstring regText(HKEY key, const wchar_t *name) {
    DWORD size{}, type{};
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr,
                     &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t) || size > 65536)
        return {};
    std::wstring value(size / sizeof(wchar_t), 0);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                     value.data(), &size) != ERROR_SUCCESS)
        return {};
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    return value;
}
std::wstring iconPath(std::wstring value) {
    if (value.empty())
        return {};
    if (value.front() == L'"') {
        auto end = value.find(L'"', 1);
        return end == std::wstring::npos ? L"" : value.substr(1, end - 1);
    }
    auto comma = value.find_last_of(L',');
    if (comma != std::wstring::npos)
        value.resize(comma);
    return value;
}
void registeredProducts(std::vector<IdentityProduct> &products, HKEY hive, REGSAM view,
                        const std::string &scope) {
    HKEY base{};
    if (RegOpenKeyExW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                      KEY_READ | view, &base) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; i < 2048; ++i) {
        wchar_t name[512]{};
        DWORD size = static_cast<DWORD>(std::size(name));
        auto result = RegEnumKeyExW(base, i, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS)
            continue;
        HKEY entry{};
        if (RegOpenKeyExW(base, name, 0, KEY_QUERY_VALUE | view, &entry) != ERROR_SUCCESS)
            continue;
        DWORD hidden{}, bytes = sizeof(hidden);
        RegGetValueW(entry, nullptr, L"SystemComponent", RRF_RT_REG_DWORD, nullptr, &hidden,
                     &bytes);
        IdentityProduct p;
        p.name = regText(entry, L"DisplayName");
        p.publisher = regText(entry, L"Publisher");
        p.primaryPath = iconPath(regText(entry, L"DisplayIcon"));
        p.root = identityPath(regText(entry, L"InstallLocation"));
        RegCloseKey(entry);
        if (hidden || p.name.empty() || p.name.size() > 200)
            continue;
        p.primaryPath = identityPath(p.primaryPath);
        if (p.root.empty() && !p.primaryPath.empty())
            p.root = std::filesystem::path(p.primaryPath).parent_path().wstring();
        if (!specificRoot(p.root))
            p.root.clear();
        if (p.root.empty() && p.primaryPath.empty())
            continue;
        p.key = scope + ":" + utf8(folded(name));
        products.push_back(std::move(p));
    }
    RegCloseKey(base);
}
std::wstring propertyString(IPropertyStore *properties, REFPROPERTYKEY key) {
    PROPVARIANT value{};
    if (FAILED(properties->GetValue(key, &value)))
        return {};
    PWSTR text{};
    std::wstring result;
    if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text) {
        result = text;
        CoTaskMemFree(text);
    }
    PropVariantClear(&value);
    return result.size() <= 512 ? result : L"";
}
void shortcutProducts(std::vector<IdentityProduct> &products, REFKNOWNFOLDERID folder) {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &path)))
        return;
    std::filesystem::path start(path);
    CoTaskMemFree(path);
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        start, std::filesystem::directory_options::skip_permission_denied, error),
        end;
    size_t inspected{};
    while (!error && it != end && inspected++ < 4096 && products.size() < 4096) {
        auto current = it->path();
        auto attributes = GetFileAttributesW(current.c_str());
        bool reparse =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (it.depth() >= 5 || reparse)
            it.disable_recursion_pending();
        if (!reparse && folded(current.extension().wstring()) == L".lnk" &&
            localPath(current.wstring())) {
            IShellLinkW *link{};
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&link)))) {
                IPersistFile *file{};
                if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
                    if (SUCCEEDED(file->Load(current.c_str(), STGM_READ))) {
                        wchar_t target[32768]{};
                        // Reading raw link metadata never resolves or launches its target.
                        link->GetPath(target, static_cast<int>(std::size(target)), nullptr,
                                      SLGP_RAWPATH);
                        auto targetPath = identityPath(target);
                        IPropertyStore *properties{};
                        std::wstring appId;
                        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))) {
                            appId = propertyString(properties, PKEY_AppUserModel_ID);
                            properties->Release();
                        }
                        if ((!targetPath.empty() &&
                             folded(std::filesystem::path(targetPath).extension().wstring()) ==
                                 L".exe") ||
                            !appId.empty()) {
                            auto existing =
                                std::find_if(products.begin(), products.end(), [&](const auto &p) {
                                    return !targetPath.empty() && p.primaryPath == targetPath &&
                                           p.appId == appId;
                                });
                            if (existing != products.end()) {
                                existing->targets.push_back(targetPath);
                            } else {
                                IdentityProduct p;
                                p.key = "shortcut:" + utf8(identityPath(current.wstring()));
                                p.name = current.stem().wstring();
                                p.primaryPath = targetPath;
                                p.appId = appId;
                                if (!targetPath.empty())
                                    p.targets.push_back(targetPath);
                                products.push_back(std::move(p));
                            }
                        }
                    }
                    file->Release();
                }
                link->Release();
            }
        }
        it.increment(error);
    }
}
std::wstring shellName(const std::wstring &appId) {
    if (appId.empty() || appId.size() > 512)
        return {};
    IShellItem *item{};
    std::wstring name;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, appId.c_str(),
                                            IID_PPV_ARGS(&item)))) {
        PWSTR text{};
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) && text) {
            name = text;
            CoTaskMemFree(text);
        }
        item->Release();
    }
    return name.size() <= 200 ? name : L"";
}
} // namespace
std::wstring identityPath(const std::wstring &path) {
    if (!localPath(path))
        return {};
    std::wstring buffer(32768, 0);
    auto count =
        GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!count || count >= buffer.size())
        return {};
    buffer.resize(count);
    std::replace(buffer.begin(), buffer.end(), L'/', L'\\');
    while (buffer.size() > 3 && buffer.back() == L'\\')
        buffer.pop_back();
    return folded(buffer);
}
ExecutableMetadata executableMetadata(const std::wstring &path) {
    ExecutableMetadata result;
    auto normalized = identityPath(path);
    if (normalized.empty())
        return result;
    auto attributes = GetFileAttributesW(normalized.c_str());
    result.exists =
        attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!result.exists)
        return result;
    DWORD ignored{}, bytes = GetFileVersionInfoSizeW(normalized.c_str(), &ignored);
    if (!bytes || bytes > 2 * 1024 * 1024)
        return result;
    std::vector<BYTE> buffer(bytes);
    if (!GetFileVersionInfoW(normalized.c_str(), 0, bytes, buffer.data()))
        return result;
    struct Translation {
        WORD language, codepage;
    };
    Translation *translations{};
    UINT size{};
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void **>(&translations), &size))
        return result;
    auto read = [&](const wchar_t *field) {
        std::wstring value;
        for (UINT i = 0; i < std::min<UINT>(size / sizeof(Translation), 16); ++i) {
            wchar_t key[160]{};
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", translations[i].language,
                       translations[i].codepage, field);
            wchar_t *text{};
            UINT length{};
            if (VerQueryValueW(buffer.data(), key, reinterpret_cast<void **>(&text), &length) &&
                text && length > 1 && length <= 512) {
                value.assign(text, length - 1);
                break;
            }
        }
        return value;
    };
    result.description = read(L"FileDescription");
    result.product = read(L"ProductName");
    result.publisher = read(L"CompanyName");
    return result;
}
std::wstring executableDisplayName(const ExecutableMetadata &metadata, const std::wstring &path) {
    if (!genericName(metadata.product) &&
        (genericName(metadata.description) || sameProduct(metadata.description, metadata.product) ||
         componentExecutable(path) || componentExecutable(metadata.description)))
        return metadata.product;
    if (!genericName(metadata.description))
        return metadata.description;
    return std::filesystem::path(path).stem().wstring();
}
struct IdentityCatalog::Impl {
    std::vector<IdentityProduct> products;
    std::unordered_map<std::wstring, std::wstring> shellNames;
    std::unordered_map<std::wstring, ExecutableMetadata> primaryMetadata;
    bool system{};
    explicit Impl(std::vector<IdentityProduct> entries, bool load)
        : products(std::move(entries)), system(load) {
        if (load) {
            registeredProducts(products, HKEY_CURRENT_USER, KEY_WOW64_64KEY, "user");
            registeredProducts(products, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, "machine64");
            registeredProducts(products, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, "machine32");
            shortcutProducts(products, FOLDERID_Programs);
            shortcutProducts(products, FOLDERID_CommonPrograms);
        }
        for (auto &p : products) {
            p.root = identityPath(p.root);
            p.primaryPath = identityPath(p.primaryPath);
            for (auto &target : p.targets)
                target = identityPath(target);
        }
    }
};
IdentityCatalog::IdentityCatalog()
    : impl_(std::make_unique<Impl>(std::vector<IdentityProduct>{}, true)) {}
IdentityCatalog::IdentityCatalog(std::vector<IdentityProduct> products)
    : impl_(std::make_unique<Impl>(std::move(products), false)) {}
IdentityCatalog::~IdentityCatalog() = default;
ResolvedApp IdentityCatalog::identify(const AppInfo &source, const IdentityContext &context) {
    ResolvedApp result{source, {}, "unverified"};
    result.source.identified = source.identified && !source.id.starts_with("unknown:") &&
                               !source.id.starts_with("unknown-frame:");
    if (context.metadata.exists)
        result.source.applicationName = executableDisplayName(context.metadata, source.path);
    else if (result.source.applicationName.empty())
        result.source.applicationName = source.name;
    if (source.id.rfind("app:", 0) == 0) {
        result.evidence = "logical-group";
        return result;
    }
    auto path = identityPath(source.path);
    if (context.metadata.exists && genericName(source.name) && !path.empty())
        result.source.name = executableDisplayName(context.metadata, source.path);
    auto appId = context.appId;
    if (appId.empty() && source.id.rfind("aumid:", 0) == 0)
        appId = wide(source.id.substr(6));
    const IdentityProduct *selected{};
    if (!appId.empty()) {
        auto match = std::find_if(impl_->products.begin(), impl_->products.end(),
                                  [&](const auto &p) { return p.appId == appId; });
        std::wstring name;
        if (match != impl_->products.end()) {
            selected = &*match;
            name = selected->name;
        }
        if (name.empty() && impl_->system) {
            auto found = impl_->shellNames.find(appId);
            if (found == impl_->shellNames.end()) {
                if (impl_->shellNames.size() >= 256)
                    impl_->shellNames.erase(impl_->shellNames.begin());
                found = impl_->shellNames.emplace(appId, shellName(appId)).first;
            }
            name = found->second;
        }
        if (context.liveAppId || !name.empty()) {
            result.representative = {
                "app:aumid:" + utf8(appId), name.empty() ? source.name : name,
                selected && !selected->primaryPath.empty() ? selected->primaryPath : source.path};
            result.evidence = context.liveAppId ? "window-or-process-appid" : "shell-appid";
        }
        // An unverified historical AppID may refer to a separate browser app.
        return result;
    }
    wchar_t windowsDirectory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) &&
        inside(path, identityPath(windowsDirectory))) {
        // Folder and script shortcuts can target shared Windows executables.
        // Their shortcut labels do not identify the foreground host application.
        result.evidence = "system-executable";
        return result;
    }
    if (!context.metadata.exists || path.empty()) {
        result.evidence = path.empty() ? "no-local-path" : "missing-executable";
        return result;
    }
    int best{};
    bool ambiguous{};
    for (auto &product : impl_->products) {
        bool exact = hasTarget(product, path);
        int score{};
        if (exact) {
            score = 100000;
        } else if (specificRoot(product.root) && inside(path, product.root)) {
            auto company = folded(context.metadata.publisher),
                 publisher = folded(product.publisher);
            ExecutableMetadata primary;
            if (impl_->system && !product.primaryPath.empty()) {
                auto found = impl_->primaryMetadata.find(product.primaryPath);
                if (found == impl_->primaryMetadata.end()) {
                    if (impl_->primaryMetadata.size() >= 256)
                        impl_->primaryMetadata.erase(impl_->primaryMetadata.begin());
                    found =
                        impl_->primaryMetadata
                            .emplace(product.primaryPath, executableMetadata(product.primaryPath))
                            .first;
                }
                primary = found->second;
            }
            auto primaryPublisher = folded(primary.publisher);
            bool companyMatch =
                !company.empty() && (company == publisher || company == primaryPublisher);
            bool productMatch = sameProduct(context.metadata.product, product.name) ||
                                sameProduct(context.metadata.product, primary.product);
            bool componentMatch = companyMatch && componentExecutable(path);
            // Install roots can contain independent products from the same publisher.
            if ((!productMatch && !componentMatch) ||
                (!company.empty() && (!publisher.empty() || !primaryPublisher.empty()) &&
                 !companyMatch))
                continue;
            score = static_cast<int>(product.root.size());
        }
        if (!score)
            continue;
        if (score > best) {
            selected = &product;
            best = score;
            ambiguous = false;
        } else if (score == best && selected && selected->key != product.key &&
                   (selected->primaryPath != product.primaryPath ||
                    selected->appId != product.appId)) {
            ambiguous = true;
        }
    }
    if (ambiguous) {
        result.evidence = "ambiguous-products";
        return result;
    }
    if (!selected)
        return result;
    // A product's DisplayIcon may point to its uninstaller. Link its verified
    // launch shortcut to the same registered identity used by helper binaries.
    if (selected->key.starts_with("shortcut:") && selected->appId.empty()) {
        const IdentityProduct *installation{};
        bool conflicting{};
        for (const auto &product : impl_->products) {
            if (product.key.starts_with("shortcut:") || !product.appId.empty() ||
                !specificRoot(product.root) || !inside(path, product.root) ||
                !sameProduct(selected->name, product.name) ||
                !sameProduct(context.metadata.product, product.name))
                continue;
            auto company = folded(context.metadata.publisher);
            auto publisher = folded(product.publisher);
            if (!company.empty() && !publisher.empty() && company != publisher)
                continue;
            if (!installation || product.root.size() > installation->root.size()) {
                installation = &product;
                conflicting = false;
            } else if (product.root.size() == installation->root.size() &&
                       product.key != installation->key) {
                conflicting = true;
            }
        }
        if (installation && !conflicting) {
            result.representative = {"app:install:" + installation->key, installation->name,
                                     selected->primaryPath};
            result.evidence = "shortcut-and-install-metadata";
            return result;
        }
    }
    result.representative = {selected->appId.empty() ? "app:install:" + selected->key
                                                     : "app:aumid:" + utf8(selected->appId),
                             selected->name,
                             selected->primaryPath.empty() ? source.path : selected->primaryPath};
    result.evidence =
        best == 100000 ? "registered-or-shortcut-target" : "install-root-and-metadata";
    return result;
}
std::vector<MergeCandidate> IdentityCatalog::mergeCandidates(const std::vector<AppInfo> &catalog,
                                                             const std::string &current) const {
    auto chosen = std::find_if(catalog.begin(), catalog.end(),
                               [&](const auto &app) { return app.id == current; });
    std::wstring root;
    const IdentityProduct *owner{};
    bool recursive{};
    if (chosen != catalog.end() && !chosen->id.starts_with("aumid:") &&
        !chosen->id.starts_with("app:aumid:")) {
        auto path = identityPath(chosen->path);
        for (const auto &product : impl_->products)
            if (chosen->id == "app:install:" + product.key) {
                owner = &product;
                break;
            }
        if (!owner)
            for (const auto &product : impl_->products)
                if (hasTarget(product, path) &&
                    (!owner || product.root.size() > owner->root.size()))
                    owner = &product;
        if (!owner)
            for (const auto &product : impl_->products) {
                if (product.key.starts_with("shortcut:") || !product.appId.empty() ||
                    !specificRoot(product.root) || !inside(path, product.root) ||
                    genericName(chosen->name) || folded(chosen->name) != folded(product.name))
                    continue;
                bool nestedProduct = std::any_of(
                    impl_->products.begin(), impl_->products.end(), [&](const auto &other) {
                        return other.key != product.key && specificRoot(other.root) &&
                               inside(other.root, product.root) && inside(path, other.root);
                    });
                if (!nestedProduct && (!owner || product.root.size() > owner->root.size()))
                    owner = &product;
            }
        bool shared =
            std::any_of(impl_->products.begin(), impl_->products.end(), [&](const auto &product) {
                return !product.appId.empty() && hasTarget(product, path);
            });
        if (!shared && (!owner || owner->appId.empty())) {
            if (owner && specificRoot(owner->root)) {
                root = owner->root;
                recursive = true;
            } else if (owner) {
                // A launch shortcut can identify a product whose registered icon is
                // an uninstaller. Reuse that product's installation directory.
                for (const auto &product : impl_->products)
                    if (!product.key.starts_with("shortcut:") && product.appId.empty() &&
                        specificRoot(product.root) && inside(path, product.root) &&
                        sameProduct(owner->name, product.name) &&
                        product.root.size() > root.size()) {
                        root = product.root;
                        recursive = true;
                    }
            }
            if (root.empty() && !path.empty())
                root = std::filesystem::path(path).parent_path().wstring();
        }
        wchar_t windowsDirectory[MAX_PATH]{};
        if (!specificRoot(root) || (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) &&
                                    inside(path, identityPath(windowsDirectory))))
            root.clear();
    }
    std::vector<MergeCandidate> result;
    for (const auto &app : catalog) {
        if (app.id == current || app.id == "app:unidentified" || !app.hasUsage)
            continue;
        auto path = identityPath(app.path);
        bool related = !root.empty() && !path.empty() &&
                       (recursive ? inside(path, root)
                                  : std::filesystem::path(path).parent_path().wstring() == root);
        // Known application identities and explicit product boundaries remain separate
        // choices even when a shared parent folder contains their executables.
        bool sameProductIdentity = owner && app.id == "app:install:" + owner->key;
        if (related && !sameProductIdentity &&
            (app.id.starts_with("app:") || app.id.starts_with("aumid:")))
            related = false;
        if (related)
            related = std::none_of(
                impl_->products.begin(), impl_->products.end(), [&](const auto &product) {
                    if (owner && product.key == owner->key)
                        return false;
                    bool sameHost = owner && product.primaryPath == owner->primaryPath &&
                                    product.appId == owner->appId;
                    return (!sameHost && hasTarget(product, path)) ||
                           (specificRoot(product.root) && product.root != root &&
                            inside(product.root, root) && inside(path, product.root));
                });
        result.push_back({app, related});
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const auto &a, const auto &b) { return a.related > b.related; });
    return result;
}
} // namespace tt
