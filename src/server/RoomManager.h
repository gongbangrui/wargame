#pragma once

#include <QObject>
#include <memory>

namespace gbr {

class PersistenceStore;
class SimulationRoom;
struct ServerConfig;

class RoomManager : public QObject {
    Q_OBJECT
public:
    RoomManager(const ServerConfig& config, PersistenceStore* persistence,
                QObject* parent = nullptr);
    bool initialize(QString* error = nullptr);
    SimulationRoom* room() const { return m_room.get(); }

private:
    const ServerConfig& m_config;
    PersistenceStore* m_persistence = nullptr;
    std::unique_ptr<SimulationRoom> m_room;
};

} // namespace gbr
