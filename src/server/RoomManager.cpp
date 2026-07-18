#include "RoomManager.h"

#include "ServerConfig.h"
#include "SimulationRoom.h"

namespace gbr {

RoomManager::RoomManager(const ServerConfig& config, PersistenceStore* persistence,
                         QObject* parent)
    : QObject(parent), m_config(config), m_persistence(persistence) {}

bool RoomManager::initialize(QString* error) {
    m_room = std::make_unique<SimulationRoom>(m_config, m_persistence, this);
    if (m_room->initialize(error)) return true;
    m_room.reset();
    return false;
}

} // namespace gbr
