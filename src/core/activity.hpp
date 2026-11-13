// DynamicIsland-Win 核心域：Activity 与其支撑组件（C++17，header-only）。
// 语义与 core/*.py 参考实现一一对应；常量来自 contracts/Types.hpp，不重复定义。
#ifndef DIW_SRC_CORE_ACTIVITY_HPP
#define DIW_SRC_CORE_ACTIVITY_HPP

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "Types.hpp"

namespace diw {

// ---------- 过期与可见性：纯函数，不修改实例 ----------

inline bool IsExpiredAt(const Activity& activity, std::int64_t now_ms) {
    if (activity.state != ActivityState::Active) return true;
    return activity.stale_at_ms.has_value() && now_ms >= *activity.stale_at_ms;
}

inline bool IsVisibleAt(const Activity& activity, std::int64_t now_ms) {
    return !IsExpiredAt(activity, now_ms);
}

// ---------- ActivityStore：幂等写入、终止态保护、过期标记 ----------

class ActivityStore {
public:
    bool Upsert(const Activity& activity) {
        if (activity.id.empty() || activity.kind.empty()) {
            throw std::invalid_argument("activity.id / kind must be non-empty");
        }
        if (activity.stale_at_ms && *activity.stale_at_ms < activity.created_at_ms) {
            throw std::invalid_argument("stale_at_ms must not precede created_at_ms");
        }
        const auto it = items_.find(activity.id);
        if (it != items_.end() &&
            (it->second.state == ActivityState::Ended ||
             it->second.state == ActivityState::Dismissed)) {
            throw std::logic_error("terminated activity must be removed first");
        }
        const bool is_new = (it == items_.end());
        items_[activity.id] = activity;
        return is_new;
    }

    bool Mark(const std::string& id, ActivityState state) {
        auto it = items_.find(id);
        if (it == items_.end()) return false;
        it->second.state = state;
        return true;
    }

    bool Remove(const std::string& id) { return items_.erase(id) > 0; }

    // 瞬时活动重新触发前调用：清掉上一次遗留的 Stale 记录。
    //
    // 为什么必须有这个：PurgeExpired 只把过期活动标成 Stale、不物理删除
    //（退场动画还要用），于是 Get(id) 依然能拿到一条已经过期的记录。
    // 而主循环用「Get(id)==nullptr」判断要不要插入新提示 —— 固定 id 的事件
    //（live-clipboard / live-notif / live-file …）就会被这条残留挡住，
    // 表现为「同类事件在同一进程里只提示第一次」。
    // 这不是理论问题：实测第二次复制剪贴板序列号从 180 走到 185，
    // 日志里却没有任何 +clipboard。
    bool RemoveIfStale(const std::string& id) {
        auto it = items_.find(id);
        if (it == items_.end()) return false;
        if (it->second.state != ActivityState::Stale) return false;
        items_.erase(it);
        return true;
    }

    const Activity* Get(const std::string& id) const {
        auto it = items_.find(id);
        return it == items_.end() ? nullptr : &it->second;
    }

    // 按 id 排序返回，保证遍历顺序确定。
    std::vector<Activity> All() const {
        std::vector<Activity> out;
        out.reserve(items_.size());
        for (const auto& kv : items_) out.push_back(kv.second);
        return out;
    }

    // 只标记，不物理删除，供渲染层做退场动画；返回本次新标记的 id。
    std::vector<std::string> PurgeExpired(std::int64_t now_ms) {
        std::vector<std::string> changed;
        for (auto& kv : items_) {
            if (kv.second.state == ActivityState::Active && IsExpiredAt(kv.second, now_ms)) {
                kv.second.state = ActivityState::Stale;
                changed.push_back(kv.first);
            }
        }
        return changed;  // std::map 已按 key 排序
    }

    std::size_t Size() const { return items_.size(); }
    void Clear() { items_.clear(); }

private:
    std::map<std::string, Activity> items_;
};

// ---------- Scheduler：确定性排序与三态分配 ----------

struct ScheduleResult {
    Frame frame;
    std::vector<std::string> expired_ids;

    bool IsIdle() const { return frame.visible.empty(); }
};

class Scheduler {
public:
    explicit Scheduler(int max_visible = kMaxVisibleActivities) : max_visible_(max_visible) {
        if (max_visible < 1) throw std::invalid_argument("max_visible must be >= 1");
    }

    ScheduleResult Select(const std::vector<Activity>& activities,
                          std::int64_t now_ms,
                          const std::string& expand_target = "") const {
        ScheduleResult result;
        std::vector<Activity> candidates;
        for (const auto& a : activities) {
            if (IsExpiredAt(a, now_ms)) {
                result.expired_ids.push_back(a.id);
            } else if (IsVisibleAt(a, now_ms)) {
                candidates.push_back(a);
            }
        }
        std::sort(result.expired_ids.begin(), result.expired_ids.end());

        // 规则显式：分数降序 → 创建时间升序 → id 升序
        std::sort(candidates.begin(), candidates.end(),
                  [](const Activity& lhs, const Activity& rhs) {
                      return std::tie(rhs.relevance_score, lhs.created_at_ms, lhs.id) <
                             std::tie(lhs.relevance_score, rhs.created_at_ms, rhs.id);
                  });

        result.frame.suppressed_count =
            static_cast<int>(candidates.size()) -
            static_cast<int>(std::min(candidates.size(),
                                      static_cast<std::size_t>(max_visible_)));
        result.frame.now_ms = now_ms;
        result.frame.expired_ids = result.expired_ids;

        const std::size_t count =
            std::min(candidates.size(), static_cast<std::size_t>(max_visible_));
        for (std::size_t i = 0; i < count; ++i) {
            ScheduledActivity item;
            item.activity = candidates[i];
            if (!expand_target.empty() && candidates[i].id == expand_target) {
                item.presentation = Presentation::Expanded;
                item.region = "expanded";
            } else if (i == 0) {
                item.presentation = Presentation::Compact;
                item.region = "compactLeading";
            } else {
                item.presentation = Presentation::Minimal;
                item.region = "minimal";
            }
            result.frame.visible.push_back(std::move(item));
        }
        return result;
    }

private:
    int max_visible_;
};

// ---------- CapsuleStateMachine：显式转移表 ----------

enum class CapsuleState { Idle, Compact, Minimal, Expanded, Dismissed };
enum class CapsuleEvent {
    ActivityStarted, SecondActivity, Expand, Collapse, Dismiss, AllEnded, Reset
};

class InvalidTransition : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class CapsuleStateMachine {
public:
    using Key = std::pair<CapsuleState, CapsuleEvent>;

    CapsuleStateMachine() = default;

    bool Can(CapsuleEvent event) const {
        return transitions_.count({state_, event}) > 0;
    }

    CapsuleState Send(CapsuleEvent event) {
        auto it = transitions_.find({state_, event});
        if (it == transitions_.end()) {
            throw InvalidTransition("illegal transition from current state");
        }
        history_.emplace_back(state_, event, it->second);
        state_ = it->second;
        return state_;
    }

    CapsuleState State() const { return state_; }
    const std::vector<std::tuple<CapsuleState, CapsuleEvent, CapsuleState>>& History() const {
        return history_;
    }

private:
    CapsuleState state_ = CapsuleState::Idle;
    std::vector<std::tuple<CapsuleState, CapsuleEvent, CapsuleState>> history_;
    const std::map<Key, CapsuleState> transitions_ = {
        {{CapsuleState::Idle, CapsuleEvent::ActivityStarted}, CapsuleState::Compact},
        {{CapsuleState::Idle, CapsuleEvent::Dismiss}, CapsuleState::Dismissed},
        {{CapsuleState::Compact, CapsuleEvent::SecondActivity}, CapsuleState::Minimal},
        {{CapsuleState::Compact, CapsuleEvent::Expand}, CapsuleState::Expanded},
        {{CapsuleState::Compact, CapsuleEvent::Dismiss}, CapsuleState::Dismissed},
        {{CapsuleState::Compact, CapsuleEvent::AllEnded}, CapsuleState::Idle},
        {{CapsuleState::Minimal, CapsuleEvent::Expand}, CapsuleState::Expanded},
        {{CapsuleState::Minimal, CapsuleEvent::Collapse}, CapsuleState::Compact},
        {{CapsuleState::Minimal, CapsuleEvent::Dismiss}, CapsuleState::Dismissed},
        {{CapsuleState::Minimal, CapsuleEvent::AllEnded}, CapsuleState::Idle},
        {{CapsuleState::Expanded, CapsuleEvent::Collapse}, CapsuleState::Compact},
        {{CapsuleState::Expanded, CapsuleEvent::Dismiss}, CapsuleState::Dismissed},
        {{CapsuleState::Expanded, CapsuleEvent::AllEnded}, CapsuleState::Idle},
        {{CapsuleState::Dismissed, CapsuleEvent::Reset}, CapsuleState::Idle},
        {{CapsuleState::Dismissed, CapsuleEvent::AllEnded}, CapsuleState::Idle},
    };
};

// ---------- DesignTokens：确定性裁剪 ----------

inline int AnimationDurationMs(int requested_ms, bool reduce_motion = false) {
    if (reduce_motion) return 0;
    if (requested_ms < 0) throw std::invalid_argument("duration must be non-negative");
    return std::min(requested_ms, kAnimationMaxMs);
}

inline double InnerCornerRadius() {
    return std::max(0.0, kCornerRadius - kConcentricMargin);
}

inline bool IsValidExpandedHeight(double height) {
    return height >= kExpandedMinHeight && height <= kExpandedMaxHeight;
}

// ---------- ActivityRuntime：组合门面与降级链 ----------

class ActivityRuntime {
public:
    explicit ActivityRuntime(int max_visible = kMaxVisibleActivities)
        : scheduler_(max_visible) {}

    void StartActivity(const Activity& activity) {
        // 同一个 id 重新开始是常规路径，不是异常：
        // 音乐暂停 → EndActivity 标记 Ended → 用户再按播放，还是 live-media。
        // 而 Upsert 对终止态明确抛 logic_error（这条校验本身有价值，别动它），
        // 所以必须先把这个已终止的记录清掉再插入 —— 否则「暂停一下再播放」
        // 会从主循环一路抛到 wWinMain，整个胶囊直接消失。
        const Activity* existing = store_.Get(activity.id);
        if (existing != nullptr && (existing->state == ActivityState::Ended ||
                                    existing->state == ActivityState::Dismissed)) {
            store_.Remove(activity.id);
        }
        store_.Upsert(activity);
        DriveMachine();
    }

    bool EndActivity(const std::string& id) {
        const bool ok = store_.Mark(id, ActivityState::Ended);
        if (ok) DriveMachine();
        return ok;
    }

    void SetBridgeOnline(bool online) { bridge_online_ = online; }
    bool IsBridgeOnline() const { return bridge_online_; }
    void SetSuspended(bool suspended) { suspended_ = suspended; }
    bool IsSuspended() const { return suspended_; }

    ScheduleResult Tick(std::int64_t now_ms, const std::string& expand_target = "") {
        const auto expired = store_.PurgeExpired(now_ms);
        has_ticked_ = true;

        if (suspended_) {
            // 全屏/反作弊：隐藏胶囊并暂停采集，但活动状态保留在 store 中。
            ScheduleResult hidden;
            hidden.expired_ids = expired;
            hidden.frame.expired_ids = expired;
            hidden.frame.now_ms = now_ms;
            last_ = hidden;
            return hidden;
        }

        auto candidates = store_.All();
        if (!bridge_online_) {
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(),
                               [](const Activity& a) {
                                   return a.kind.rfind("bridge.", 0) == 0;
                               }),
                candidates.end());
        }

        last_ = scheduler_.Select(candidates, now_ms, expand_target);
        last_.frame.expired_ids = expired;
        last_.expired_ids = expired;
        DriveMachine();
        return last_;
    }

    const ScheduleResult& LastResult() const { return last_; }
    ActivityStore& Store() { return store_; }
    const ActivityStore& Store() const { return store_; }
    CapsuleStateMachine& StateMachine() { return machine_; }
    const CapsuleStateMachine& StateMachine() const { return machine_; }

private:
    void DriveMachine() {
        const bool idle = last_.IsIdle();
        if (idle && !has_ticked_) return;  // 尚未 tick 时不改变状态
        if (idle) {
            TrySend(CapsuleEvent::AllEnded);
            return;
        }
        const auto state = machine_.State();
        if (state == CapsuleState::Idle || state == CapsuleState::Dismissed) {
            TrySend(CapsuleEvent::ActivityStarted);
        }
        if (last_.frame.visible.size() >= 2) {
            TrySend(CapsuleEvent::SecondActivity);
        }
    }

    void TrySend(CapsuleEvent event) {
        if (machine_.Can(event)) machine_.Send(event);
    }

    ActivityStore store_;
    Scheduler scheduler_;
    CapsuleStateMachine machine_;
    ScheduleResult last_;
    bool bridge_online_ = true;
    bool suspended_ = false;
    bool has_ticked_ = false;
};

}  // namespace diw

#endif  // DIW_SRC_CORE_ACTIVITY_HPP
