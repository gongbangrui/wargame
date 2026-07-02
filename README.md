# 兵器推演 · 本地版本

基于 Qt 6 (Quick + QML) 的连续模拟推演框架。单元独立后端运行、通过 VMF 风格消息总线解耦，红蓝双方对抗与指挥员决策在同一进程内通过顶部「视角」下拉切换实现。

## 目录

- [编译与运行](#编译与运行)
- [项目结构](#项目结构)
- [C++ 后端接口](#c-后端接口)
- [QML 接口（ContextProperty 与组件）](#qml-接口contextproperty-与组件)
- [消息总线（VMF 风格）](#消息总线vmf-风格)
- [单元参数（场景 JSON）](#单元参数场景-json)
- [视角与控件说明](#视角与控件说明)
- [设计要点](#设计要点)
- [典型推演流程](#典型推演流程)
- [与设计文档的对应关系](#与设计文档的对应关系)
- [扩展点](#扩展点)

## 编译与运行

依赖：Qt 6.11+（MinGW）、CMake 3.16+、Ninja。

```powershell
$env:Qt6_DIR = ''D:\qt\6.11.1\mingw_64''
$env:MINGW   = ''D:\qt\Tools\mingw1310_64''
$env:PATH    = "$env:MINGW\bin;$env:Qt6_DIR\bin;D:\qt\Tools\Ninja;$env:PATH"

& ''D:\qt\Tools\CMake_64\bin\cmake.exe'' -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="$env:Qt6_DIR" `
  -DCMAKE_C_COMPILER="$env:MINGW\bin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="$env:MINGW\bin\g++.exe" `
  -DCMAKE_BUILD_TYPE=Debug

& ''D:\qt\Tools\CMake_64\bin\cmake.exe'' --build build --parallel
```

构建产物：`build/appindex.exe`。

Qt Creator 中直接以 `CMakeLists.txt` 作为工程根打开，选择 `D:\qt\Tools\mingw1310_64` 工具链，构建并运行。

## 项目结构

```
index/
├── CMakeLists.txt
├── main.cpp
├── Main.qml                              顶层 ApplicationWindow，加载 SimulationRoot
├── README.md
├── assets/
│   └── sample_map.json
├── qml/
│   ├── SimulationRoot.qml                顶部工具栏 + 视角 Loader
│   ├── components/
│   │   ├── MapCanvas.qml                 地图（Canvas 绘制，滚轮缩放/平移/右键）
│   │   ├── MessageLog.qml                消息流（按颜色区分类型）
│   │   ├── UnitPanel.qml                 单元状态面板
│   │   ├── SectionTitle.qml              小标签样式
│   │   ├── TonalButton.qml               实心按钮（可换 base 色）
│   │   ├── GhostButton.qml               描边按钮
│   │   ├── EventDialog.qml               通用确认/提示弹窗
│   │   └── UnitEditDialog.qml            单元参数表单（新增/编辑）
│   └── views/
│       ├── ScenarioEditorView.qml        场景编辑视角
│       ├── CommandPostView.qml           指挥所视角
│       ├── ReconUavView.qml              侦察机视角
│       ├── AttackUavView.qml             攻击机视角
│       ├── GroundScoutView.qml           地面分队视角
│       └── DirectorView.qml              导演视角
├── scenarios/
│   └── editing.json                      用户编辑保存的 JSON（运行时生成）
└── src/
    ├── core/
    │   ├── Geo.h / .cpp                  局部米坐标系
    │   ├── MessageBus.h / .cpp           VMF 风格消息总线（阵营+距离通联）
    │   ├── UnitBase.h / .cpp             单元基类
    │   ├── Scenario.h / .cpp             场景 IO + 默认场景
    │   ├── MapProvider.h / .cpp          逻辑坐标↔像素投影（缩放/平移）
    │   └── SimulationEngine.h / .cpp     仿真引擎（tick / 扫描 / 时间流）
    ├── units/
    │   ├── CommandPost.h / .cpp          指挥所
    │   ├── ReconUAV.h / .cpp             侦察无人机
    │   ├── AttackUAV.h / .cpp            攻击无人机
    │   └── GroundScout.h / .cpp          地面侦察分队
    └── view/
        ├── SimulationController.h / .cpp 视角切换 + 命令派发
        └── ScenarioEditor.h / .cpp       场景 IO 助手
```

## C++ 后端接口

### 命名空间

所有类在 `gbr` 命名空间内。

### `src/core/Geo.h`

```cpp
struct GeoPos {
    double x, y, alt;
    double distanceTo(const GeoPos&) const;
    GeoPos lerp(target, t) const;
};
```

### `src/core/MessageBus.{h,cpp}`

```cpp
struct Message {
    enum class Type {
        PositionReport, TargetDetect, TargetTrack, TargetDestroyed,
        AttackOrder, FlightPlan, Guidance, Ack, Withdraw,
        CommCheck, EngagementReport, Info
    };
    QString id;
    Type type;
    QString sender, receiver;          // receiver="*" 为广播
    QDateTime timestamp;
    bool requiresAck;
    bool acked;
    QJsonObject payload;
    QJsonObject toJson() const;
};

class MessageBus : public QObject {
    Q_SIGNALS:
        void messagePosted(const QJsonObject& msg);
        void unitStateChanged(const QString& unitId, const QJsonObject& snapshot);

    void send(const Message& msg);
    void subscribe(const QString& unitId, std::function<void(const Message&)> h);
    void unsubscribe(const QString& unitId);
    bool canCommunicate(const QString& aId, const QString& bId) const;
    void updateUnitPosition(const QString& unitId, const QPointF& pos,
                            double commRange, const QString& side = QString());
    void updateUnitSide(const QString& unitId, const QString& side);
    bool    isRegistered(const QString& unitId) const;
    QString unitSide    (const QString& unitId) const;
};
```

通联判定：同阵营且 `distance ≤ min(commRangeA, commRangeB)`；跨阵营或任一未注册返回 false。

### `src/core/UnitBase.{h,cpp}`

```cpp
enum class Side  { Red, Blue };
QString sideName(Side);     // "red"/"blue"
Side    sideFromName(const QString&);

enum class UnitKind { CommandPost, ReconUAV, AttackUAV, GroundScout };
QString kindName(UnitKind);
UnitKind kindFromName(const QString&);

class UnitBase : public QObject {
    Q_PROPERTY(QString id CONSTANT)
    Q_PROPERTY(QString callsign READ callsign WRITE setCallsign NOTIFY callsignChanged)
    Q_PROPERTY(QString side    READ sideStr    NOTIFY sideChanged)
    Q_PROPERTY(QString kind    READ kindStr    CONSTANT)
    Q_PROPERTY(QVariantList position READ position NOTIFY positionChanged)
    Q_PROPERTY(QJsonObject perception READ perceptionJson NOTIFY perceptionChanged)
    Q_PROPERTY(QJsonObject sharedKnowledge READ sharedKnowledgeJson NOTIFY sharedKnowledgeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(double detectRange READ detectRange WRITE setDetectRange NOTIFY paramsChanged)
    Q_PROPERTY(double attackRange READ attackRange WRITE setAttackRange NOTIFY paramsChanged)
    Q_PROPERTY(double commRange   READ commRange   WRITE setCommRange   NOTIFY paramsChanged)
    Q_PROPERTY(double speed       READ speed       WRITE setSpeed       NOTIFY paramsChanged)
    Q_PROPERTY(double maxHp       READ maxHp       WRITE setMaxHp       NOTIFY paramsChanged)
    Q_PROPERTY(double hp  READ hp  NOTIFY hpChanged)
    Q_PROPERTY(bool   alive READ alive NOTIFY hpChanged)

    struct Params {
        double detectRange = 5000, attackRange = 1500, commRange = 20000;
        double speed = 50, maxHp = 100;
        GeoPos pos;
    };

    void setParams(const Params&);
    void setPosition(const GeoPos&);
    virtual void onTick(double simSeconds) = 0;
    bool canDetect(const GeoPos&) const;
    GeoPos pos() const;

    Q_SIGNALS:
        void notifyEvent(const QString& title, const QString& body, const QString& level);
        void strikeOrderRequested(const QString& targetId);
        void routeChangeRequested();
        void attackProgress(double distanceToTarget, double attackRange);

    static std::unique_ptr<UnitBase> create(const QString& id, UnitKind, Side,
                                            MessageBus*, QObject* parent = nullptr);
};
```

子类需实现 `onTick(double)`、`onMessage(const Message&)`（默认空）。

### `src/core/Scenario.{h,cpp}`

```cpp
struct ScenarioUnit { QString id, callsign, kind, side;
                      GeoPos pos;
                      double detectRange, attackRange, commRange, speed, maxHp; };
struct ScenarioMap  { QString name; double widthMeters, heightMeters; QString backgroundResource; };
struct Scenario     { ScenarioMap map; std::vector<ScenarioUnit> units; QString notes; };

class ScenarioIo {
    static Scenario    loadFromFile(const QString& path, QString* err = nullptr);
    static bool        saveToFile  (const Scenario&, const QString& path, QString* err = nullptr);
    static QJsonObject toJson(const Scenario&);
    static Scenario    fromJson(const QJsonObject&);
    static Scenario    defaultScenario();
};
```

### `src/core/MapProvider.{h,cpp}`

```cpp
class MapProvider {
    void setLogicalSizeMeters(double w, double h);
    void setZoom(double z);
    void setCenter(const GeoPos&);
    QPointF toPixel(double vw, double vh, const GeoPos&) const;
    GeoPos  fromPixel(double vw, double vh, const QPointF&) const;
    double  radiusToPixels(double vw, double vh, double meters) const;
    QJsonObject describe() const;
};
```

### `src/core/SimulationEngine.{h,cpp}`

```cpp
class SimulationEngine : public QObject {
    Q_PROPERTY(double simTime  READ simTime  NOTIFY simTimeChanged)
    Q_PROPERTY(double speedMul READ speedMul WRITE setSpeedMul NOTIFY speedMulChanged)
    Q_PROPERTY(bool   running  READ running  WRITE setRunning  NOTIFY runningChanged)
    Q_PROPERTY(QJsonObject mapInfo READ mapInfo NOTIFY mapChanged)
    Q_PROPERTY(QVariantList units    READ unitsForView    NOTIFY unitsChanged)
    Q_PROPERTY(QVariantList messages READ recentMessages NOTIFY messagesChanged)

    void setScenario(const Scenario&);
    void loadDefaultScenario();
    void setRunning(bool);
    void setSpeedMul(double);
    Q_INVOKABLE void stepOnce(double simSeconds = 1.0);

    MessageBus*  bus()  const;
    MapProvider* map()  const;

    Q_INVOKABLE QJsonObject unitSnapshot(const QString& id) const;
    Q_INVOKABLE void        command(const QString& action, const QVariantMap& args);

    void addOrUpdateUnit(const ScenarioUnit&);
    void removeUnit(const QString& id);
    QStringList unitIds() const;
    Scenario scenario() const;
    void persistScenario(const QString& path);

    UnitBase* unit(const QString& id) const;

    QJsonArray collectPerceptionSnapshot(const QString& forSide) const;
    QJsonArray collectAllUnitsSnapshot() const;

    Q_SIGNALS:
        void simTimeChanged();
        void speedMulChanged();
        void runningChanged();
        void unitsChanged();
        void messagesChanged();
        void mapChanged();
        void perceptionUpdated(const QString& unitId, const QJsonObject& perception);
        void eventPosted(const QString& title, const QString& body, const QString& level);
};
```

`unitSnapshot(id)` 返回：

```json
{
  "id": "red_a1", "callsign": "红方攻击1", "kind": "attackuav", "side": "red",
  "position": [x, y, alt],
  "detectRange": 4000, "attackRange": 2500, "commRange": 25000,
  "speed": 100, "maxHp": 120, "hp": 120, "alive": true,
  "status": "沿航路点 2/3 飞行",
  "sharedKnowledge": { "unit:red_cp:last": { } },
  "detections": [
    { "id": "blue_r1", "callsign": "蓝方侦察1", "kind": "reconuav", "side": "blue",
      "distance": 8234.5, "position": [x, y, alt] }
  ]
}
```

`command(action, args)` 支持的动作：

| action | args | 说明 |
|---|---|---|
| `assignTarget` | `attackerId, targetId` | 发送 `AttackOrder`（requiresAck） |
| `setFlightPlan` | `attackerId, waypoints: [{x,y}]` | 发送 `FlightPlan` |
| `engageTarget` | `attackerId, targetId` | 立即开火 |
| `moveTo` | `unitId, pos: {x,y}` | 发送 `Guidance: moveTo` |
| `withdraw` | `unitId` | 发送 `Withdraw` |
| `guideAttack` | `guideId, attackerId, targetId, targetPos: {x,y}` | 地面分队发送 `FlightPlan` 给攻击机 |

### `src/units/CommandPost.{h,cpp}`

```cpp
class CommandPost : public UnitBase {
    Q_INVOKABLE QJsonObject pendingTargets() const;
    Q_INVOKABLE QStringList  knownTargets() const;
    Q_INVOKABLE QJsonObject  knownTarget(const QString& id) const;
    Q_INVOKABLE void orderStrike   (const QString& attackerId, const QString& targetId);
    Q_INVOKABLE void orderWithdraw (const QString& unitId);
    Q_INVOKABLE void setFlightPlan (const QString& attackerId, const QVariantList& waypoints);
};
```

### `src/units/ReconUAV.{h,cpp}`

```cpp
class ReconUAV : public UnitBase {
    Q_INVOKABLE void setPatrol(const QVariantList& waypoints);
    Q_INVOKABLE void clearPatrol();
};
```

### `src/units/AttackUAV.{h,cpp}`

```cpp
class AttackUAV : public UnitBase {
    Q_PROPERTY(QString targetId READ targetId NOTIFY targetChanged)
    Q_PROPERTY(double   distanceToTarget READ distanceToTarget NOTIFY targetChanged)
    Q_PROPERTY(bool     armed   READ armed   NOTIFY armedChanged)

    void setOtherPositions(const std::vector<std::pair<QString, GeoPos>>& others);
    void fireOnTarget(const QString& targetId);
    Q_SIGNALS: void targetChanged(); void armedChanged();
};
```

### `src/units/GroundScout.{h,cpp}`

```cpp
class GroundScout : public UnitBase {
    Q_INVOKABLE void setRoute(const QVariantList& waypoints);
    Q_INVOKABLE void guideAttack(const QString& attackerId, const QString& targetId,
                                 const QPointF& lastSeen);
};
```

### `src/view/SimulationController.{h,cpp}`

QML 顶层可见的 controller，封装 `SimulationEngine` 并转发信号到 QML 直接可绑定属性。

```cpp
class SimulationController : public QObject {
    Q_PROPERTY(QString viewMode      READ viewMode      WRITE setViewMode      NOTIFY viewModeChanged)
    Q_PROPERTY(QString focusedSide   READ focusedSide   WRITE setFocusedSide   NOTIFY focusedSideChanged)
    Q_PROPERTY(QString focusedUnitId READ focusedUnitId WRITE setFocusedUnitId NOTIFY focusedUnitIdChanged)
    Q_PROPERTY(QString focusedKind   READ focusedKind   NOTIFY focusedUnitIdChanged)
    Q_PROPERTY(SimulationEngine* engine READ engine CONSTANT)

    Q_PROPERTY(double        simTime  NOTIFY simTimeForward)
    Q_PROPERTY(bool          running  NOTIFY runningForward)
    Q_PROPERTY(QVariantList  units    NOTIFY unitsForward)
    Q_PROPERTY(QVariantList  messages NOTIFY messagesForward)
    Q_PROPERTY(QJsonObject   mapInfo  NOTIFY mapInfoForward)

    Q_INVOKABLE void loadDefault();
    Q_INVOKABLE void saveScenario(const QString& path);
    Q_INVOKABLE void loadScenario(const QString& path);
    Q_INVOKABLE void setRunning(bool);
    Q_INVOKABLE void setSpeed(double);
    Q_INVOKABLE void stepOnce();
    Q_INVOKABLE void command(const QString& action, const QVariantMap& args);

    Q_INVOKABLE QJsonObject    unitsJson() const;
    Q_INVOKABLE void           upsertUnit(const QVariantMap& data);
    Q_INVOKABLE void           removeUnit(const QString& id);
    Q_INVOKABLE QJsonArray     perceptionForSide(const QString& side) const;
    Q_INVOKABLE QJsonArray     allUnits() const;
    Q_INVOKABLE QJsonObject    unitAt(const QString& id) const;
    Q_INVOKABLE QVariantList   unitOptions(const QString& kindFilter, const QString& sideFilter) const;
    Q_INVOKABLE QStringList    viewModeOptions() const;

    Q_SIGNALS:
        void viewModeChanged(); void focusedSideChanged(); void focusedUnitIdChanged();
        void simTimeForward();  void runningForward();     void unitsForward();
        void messagesForward(); void mapInfoForward();
        void commandExecuted(const QString& action, const QVariantMap& args);
};
```

`viewMode` 取值：`editor`、`commandpost-red`、`commandpost-blue`、`recon`、`attack`、`ground`、`director`。

### `src/view/ScenarioEditor.{h,cpp}`

```cpp
class ScenarioEditor : public QObject {
    Q_INVOKABLE bool    saveJsonText(const QString& path, const QString& text);
    Q_INVOKABLE QString loadJsonText(const QString& path);
    Q_INVOKABLE QString defaultScenarioJson() const;
    Q_INVOKABLE bool    saveFile(const QString& path, const QString& text);
    Q_INVOKABLE QString loadFile(const QString& path);
};
```

## QML 接口（ContextProperty 与组件）

`main.cpp` 注册到 `QQmlApplicationEngine`：

| ContextProperty | 类型 | 作用 |
|---|---|---|
| `controller` | `SimulationController*` | 顶层控制器（视角 / 命令派发） |
| `editor`     | `ScenarioEditor*`       | 场景文件读写辅助 |

### 顶层可绑定属性（直接绑 `controller.*`）

```qml
controller.simTime        // double，单位秒
controller.running        // bool
controller.units          // QVariantList<Map>：{ id, callsign, kind, side, hp, alive, position[3], detectRange, attackRange, commRange }
controller.messages       // QVariantList：最近 200 条消息 JSON
controller.mapInfo        // { name, widthMeters, heightMeters }
controller.viewMode       // 当前视角（读写）
controller.focusedSide    // "red"/"blue"（读写）
controller.focusedUnitId  // 当前选中单元 id（读写）
controller.focusedKind    // 选中单元类型
controller.engine         // 原始 SimulationEngine 指针
```

转发信号（用于显式订阅）：`viewModeChanged`、`focusedSideChanged`、`focusedUnitIdChanged`、`simTimeForward`、`runningForward`、`unitsForward`、`messagesForward`、`mapInfoForward`、`commandExecuted(action, args)`。

### 调用方法

```qml
controller.setRunning(true)
controller.setSpeed(2)               // 0/1/2/4/8
controller.stepOnce()
controller.loadDefault()
controller.command("assignTarget", { attackerId: "red_a1", targetId: "blue_r1" })
controller.upsertUnit({ callsign: "侦察A", kind: "reconuav", side: "red", x: 1000, y: 1000, ... })
controller.removeUnit("red_r1")
controller.unitAt("red_a1")           // QJsonObject
controller.unitOptions("attackuav", "red")  // QVariantList：[{id, callsign, kind, side}]
controller.allUnits()
controller.viewModeOptions()
```

### `qml/SimulationRoot.qml`

顶层内容容器：顶部工具栏 + Loader。提供 `viewMode` 下拉、仿真时间显示、自绘运行开关、速率下拉、单步按钮、加载默认。

### 组件：`qml/components/`

| 组件 | 类型 | 说明 |
|---|---|---|
| `MapCanvas` | `Item` | 地图画布。`property sideFilter / focusUnitId / showDetectRange / showCommRange / showAttackRange`；`signal clickedMap(pos) / rightClickedMap(pos) / unitClicked(id, button)` |
| `MessageLog` | `Rectangle` | 实时消息流。颜色按消息类型区分；自动订阅 `controller.messagesForward` |
| `UnitPanel` | `Rectangle` | 单元状态面板。`property snap: QJsonObject`；自动订阅 `controller.unitsForward` 刷新快照 |
| `SectionTitle` | `Text` | 小标签样式，灰色字、字距 1、像素 11 |
| `TonalButton` | `AbstractButton` | 实心按钮，可换 `base` 色，自定义按下/悬停反馈 |
| `GhostButton` | `AbstractButton` | 描边按钮，hover/按下变色 |
| `EventDialog` | `Dialog` | 通用确认弹窗（`level/title/body/ackClicked/rejectClicked`） |
| `UnitEditDialog` | `Dialog` | 单元参数表单，`openWith(unit)` / `openNew(x,y,side)`；提交时发 `accepted(data)` |

`UnitEditDialog` 表单字段：`id, callsign, kind, side, x, y, alt, detectRange, attackRange, commRange, speed, maxHp`。

### 视图：`qml/views/`

| 视图 | 触发条件 | 主要交互 |
|---|---|---|
| `ScenarioEditorView` | `controller.viewMode = "editor"` | 列表增删改、双击地图 = 新增、右键单元 = 删除、`UnitEditDialog` |
| `CommandPostView` | `"commandpost-red"/"commandpost-blue"` | 单元面板 + 事件队列 + 右键单元下达 AttackOrder / 机动 / 撤离 |
| `ReconUavView` | `"recon"` | 选中侦察机 + 点击地图 = 设置机动点 + 单元面板 |
| `AttackUavView` | `"attack"` | 选择目标 → 下达攻击命令 / 立即开火 / 撤离 |
| `GroundScoutView` | `"ground"` | 选攻击机和目标 → `guideAttack` 引导 |
| `DirectorView` | `"director"` | 全图 + 双方态势列表 + 全局消息流 |

## 消息总线（VMF 风格）

`Message` 在 `MessageBus::send` 时被序列化为 JSON 投递到 QML 与订阅者：

```json
{
  "id": "m_42",
  "type": "TargetDetect",
  "sender": "red_r1",
  "receiver": "red_cp",
  "time": "2026-07-02T00:42:11Z",
  "requiresAck": true,
  "acked": false,
  "payload": {
    "targetId": "blue_r1",
    "callsign": "蓝方侦察1",
    "x": 38211.5, "y": 22310.2, "alt": 3000,
    "distance": 7824.4
  }
}
```

类型与约定：

| Type | 发送方 | 接收方 | payload 关键字段 |
|---|---|---|---|
| `PositionReport` | 单元 | `*` 广播 | `x, y, alt, side` |
| `TargetDetect` | 侦察机 | 己方指挥所 | `targetId, callsign, x, y, alt, distance` |
| `TargetTrack` | 单元 | 友方 | `targetId, x, y, alt` |
| `AttackOrder` | 指挥所 / 地面分队 | 攻击机 | `targetId[, fireNow]` |
| `FlightPlan` | 指挥所 / 地面分队 | 攻击机 | `waypoints: [{x,y,alt}], targetId?` |
| `Guidance` | 指挥所 | 友方单元 | `kind: "moveTo", x, y` |
| `EngagementReport` | 攻击机 | 己方指挥所 | `targetId, distance, hit` |
| `TargetDestroyed` | 攻击机 | 己方指挥所 | `targetId, attackerId` |
| `Withdraw` | 指挥所 | 友方单元 | — |
| `Ack` | 接收方 | 原发送方 | `inReplyTo` |

通联规则：仅同阵营且距离 `≤ min(commRange)` 可达；广播消息绕过通联限制。

## 单元参数（场景 JSON）

每个单元的 JSON 字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | 唯一 ID，留空自动生成 `u_<毫秒>` |
| `callsign` | string | 显示呼号 |
| `kind` | string | `commandpost` / `reconuav` / `attackuav` / `groundscout` |
| `side` | string | `red` / `blue` |
| `x, y, alt` | number (m) | 逻辑坐标（米） |
| `detectRange` | number (m) | 探测半径 |
| `attackRange` | number (m) | 攻击半径（指挥所与侦察/地面为 0） |
| `commRange`   | number (m) | 通信半径 |
| `speed`       | number (m/s) | 移动速度（仿真秒） |
| `maxHp`       | number | 完整度上限 |

默认场景示例（见 `ScenarioIo::defaultScenario()`）：红西蓝东，各 4 类单元 × 1，地图画布 40 km × 30 km。

## 视角与控件说明

| 控件 | 位置 | 功能 |
|---|---|---|
| 视角下拉 | 顶部 | 切换 `editor / commandpost-red / commandpost-blue / recon / attack / ground / director` |
| 时间显示 | 顶部 | `controller.simTime`，仿真秒 |
| 运行开关 | 顶部 | 自绘 Switch，`controller.setRunning(!controller.running)` |
| 速率下拉 | 顶部 | `暂停 / 1x / 2x / 4x / 8x` → `controller.setSpeed(s)` |
| 单步按钮 | 顶部 | `controller.stepOnce()` |
| 加载默认 | 顶部 | 重置为默认 8 单元 |
| 地图 | 中央 | 滚轮缩放、左键选择/移动、右键菜单/新建 |
| 列表 | 侧栏 | 单元列表，点击选中 / 双击编辑 |
| 单元面板 | 侧栏 | 实时显示选中单元的 HP、参数、状态、侦察到的目标 |
| 消息流 | 侧栏 | 最近 200 条消息，颜色按类型区分 |
| 事件队列 | 指挥所 | 侦察报告 / 攻击命令 / 摧毁 / 警告事件 |

## 设计要点

- **单元独立后端**：`UnitBase` 在引擎 tick 中独立驱动；前台 QML 通过信号/属性观察后端状态。
- **消息总线解耦**：发布订阅 + 单点直送；通联依赖距离与阵营；上层单元类型按消息驱动行为，便于后续把 `MessageBus` 替换为 `QLocalSocket/QUdpSocket` 实现跨进程。
- **连续模拟**：50 ms 真实时钟 tick，按 `speedMul` 换算仿真秒；支持单步推进。
- **场景编辑**：双击地图放置 / 右键删除 / 选中后 `UnitEditDialog` 修改参数；保存到 `./scenarios/editing.json`。
- **本地版本**：单进程内通过顶部「视角」下拉切换；后续可拆分为多进程 / 多机。

## 典型推演流程

1. 启动后默认进入「红方指挥所」视角。
2. 顶部「视角」选 `director` 全图观察；点击「运行」开始仿真。
3. 约 6~10 秒后红方侦察机发现蓝方单元，发出 `TargetDetect`，红方指挥所事件队列中出现提示。
4. 切换回「红方指挥所」：右键单元 → 下达攻击命令 → 选攻击机 → 自动派航路。
5. 切换到「攻击无人机」：选目标 → 「立即开火」或等待航路完成。
6. 切换到「地面分队」：当与攻击机通联时使用「发送航路到攻击机」接管引导。
7. 攻击机摧毁目标后自动 `TargetDestroyed` → 指挥所发 `Withdraw`。


