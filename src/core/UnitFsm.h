#pragma once

#include <QString>
#include <functional>
#include <map>
#include <vector>

namespace gbr {

/// @brief Simple finite state machine for unit behavior control.
/// @details States are string keys. Each state has an update callback called
/// each tick, and an optional onEnter/onExit callback. Transitions are
/// evaluated before each update.
class UnitFsm {
public:
    /// @brief Callback type for state update: receives delta time.
    using UpdateFn = std::function<void(double dt)>;
    using EnterFn = std::function<void()>;
    using ExitFn = std::function<void()>;
    /// @brief Transition condition: returns true if transition should fire.
    using ConditionFn = std::function<bool()>;

    struct Transition {
        QString toState;
        ConditionFn condition;
    };

    /// @brief Set the initial state and trigger its onEnter callback.
    void setInitialState(const QString& state);
    /// @brief Register a state with update/enter/exit callbacks.
    void addState(const QString& name, UpdateFn update, EnterFn onEnter = nullptr, ExitFn onExit = nullptr);
    /// @brief Add a transition from one state to another with a condition.
    void addTransition(const QString& from, const QString& to, ConditionFn cond);
    /// @brief Evaluate transitions, execute onExit/onEnter, then run update. Call each tick.
    void tick(double dt);
    /// @brief Force a transition to a state, bypassing conditions.
    void goTo(const QString& state);

    QString currentState() const { return m_current; }

private:
    struct StateDef {
        UpdateFn update;
        EnterFn onEnter;
        ExitFn onExit;
        std::vector<Transition> transitions;
    };

    std::map<QString, StateDef> m_states;
    QString m_current;
    bool m_initialized = false;
};

} // namespace gbr
