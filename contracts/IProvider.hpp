// DynamicIsland-Win / IProvider.hpp
// Provider 插件接口。每个 Provider 独立启停；单个 Provider 异常不得影响其他 Provider。
//
// 状态：接口契约（阶段 S3），已通过 MSVC C++17 编译验证。
#ifndef DIW_CONTRACTS_I_PROVIDER_HPP
#define DIW_CONTRACTS_I_PROVIDER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Types.hpp"

namespace diw {

struct ProviderUpdate {
    std::optional<Activity> upsert;
    std::optional<std::string> end_activity_id;
};

class IProvider {
public:
    virtual ~IProvider() = default;

    virtual std::string Name() const = 0;
    virtual bool IsSensitive() const = 0;  // 敏感项默认关闭

    virtual void SetEnabled(bool enabled) = 0;
    virtual bool IsEnabled() const = 0;

    // 无变化时返回空向量，避免无效重绘（ZenDesktop perf.md 规则 3）。
    virtual std::vector<ProviderUpdate> Poll(std::int64_t now_ms) = 0;
};

}  // namespace diw

#endif  // DIW_CONTRACTS_I_PROVIDER_HPP
