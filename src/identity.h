#pragma once
#include "core.h"
#include <memory>

namespace tt {
struct ResolvedApp {
    AppInfo source;
    AppInfo representative;
    std::string evidence;
};
struct ExecutableMetadata {
    bool exists{};
    std::wstring description, product, publisher;
};
struct IdentityProduct {
    std::string key;
    std::wstring name, root, primaryPath, publisher, appId;
    std::vector<std::wstring> targets;
};
struct IdentityContext {
    ExecutableMetadata metadata;
    std::wstring appId;
    bool liveAppId{};
};
ExecutableMetadata executableMetadata(const std::wstring &path);
std::wstring executableDisplayName(const ExecutableMetadata &metadata, const std::wstring &path);
std::wstring identityPath(const std::wstring &path);
class IdentityCatalog {
  public:
    IdentityCatalog();
    explicit IdentityCatalog(std::vector<IdentityProduct> products);
    ~IdentityCatalog();
    ResolvedApp identify(const AppInfo &source, const IdentityContext &context);
    std::vector<MergeCandidate> mergeCandidates(const std::vector<AppInfo> &catalog,
                                                const std::string &current) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace tt
