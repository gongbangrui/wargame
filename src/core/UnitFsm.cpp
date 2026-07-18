#include "UnitFsm.h"
#include <QDebug>

namespace gbr {

void UnitFsm::setInitialState(const QString& state) {
    if (m_initialized) {
        qWarning() << "[UnitFsm] already initialized, ignoring setInitialState for" << state;
        return;
    }
    m_current = state;
    m_initialized = true;
    auto it = m_states.find(m_current);
    if (it != m_states.end() && it->second.onEnter) {
        it->second.onEnter();
    } else if (it == m_states.end()) {
        qWarning() << "[UnitFsm] setInitialState with unknown state:" << state;
    }
}

void UnitFsm::addState(const QString& name, UpdateFn update, EnterFn onEnter, ExitFn onExit) {
    m_states[name] = {std::move(update), std::move(onEnter), std::move(onExit), {}};
}

void UnitFsm::addTransition(const QString& from, const QString& to, ConditionFn cond) {
    auto it = m_states.find(from);
    if (it != m_states.end()) {
        it->second.transitions.push_back({to, std::move(cond)});
    }
}

void UnitFsm::tick(double dt) {
    auto it = m_states.find(m_current);
    if (it == m_states.end()) return;

    for (const auto& tr : it->second.transitions) {
        if (tr.condition && tr.condition()) {
            goTo(tr.toState);
            it = m_states.find(m_current);
            if (it == m_states.end()) return;
            break;
        }
    }

    if (it->second.update) {
        it->second.update(dt);
    }
}

void UnitFsm::goTo(const QString& state) {
    if (m_current == state) return;
    auto newIt = m_states.find(state);
    if (newIt == m_states.end()) {
        qWarning() << "[UnitFsm] goTo unknown state:" << state;
        return;
    }
    auto oldIt = m_states.find(m_current);
    if (oldIt != m_states.end() && oldIt->second.onExit) {
        oldIt->second.onExit();
    }
    m_current = state;
    if (newIt->second.onEnter) {
        newIt->second.onEnter();
    }
}

} // namespace gbr
