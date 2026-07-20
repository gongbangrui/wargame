pragma Singleton

import QtQml
QtObject {
    // 留作可选的应用级依赖容器；当前页面通过根组件显式传递依赖。
    property var controller: null
    property var editor: null
}
