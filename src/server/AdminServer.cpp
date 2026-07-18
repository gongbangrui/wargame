#include "AdminServer.h"

#include "PersistenceStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QWebSocket>
#include <QUrl>
#include <algorithm>

namespace gbr {

namespace {

QByteArray randomHex(int bytes) {
    QByteArray value(bytes, '\0');
    auto* begin = reinterpret_cast<quint32*>(value.data());
    QRandomGenerator::global()->generate(begin, begin + (bytes + 3) / 4);
    return value.toHex();
}

QByteArray htmlEscape(const QString& text) {
    return text.toHtmlEscaped().toUtf8();
}

}

AdminServer::AdminServer(AuthPolicy* auth, PersistenceStore* persistence, QObject* parent)
    : QObject(parent), m_auth(auth), m_persistence(persistence) {
    connect(&m_server, &QTcpServer::newConnection, this, &AdminServer::onConnection);
}

bool AdminServer::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (m_server.listen(address, port)) return true;
    if (error) *error = m_server.errorString();
    return false;
}

void AdminServer::close() {
    m_server.close();
    m_sessions.clear();
}

QByteArray AdminServer::response(int status, const QByteArray& type, const QByteArray& body,
                                 const QByteArray& extraHeaders) const {
    const QByteArray text = status == 200 ? "OK" : status == 201 ? "Created" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" : "Internal Server Error";
    QByteArray result = "HTTP/1.1 " + QByteArray::number(status) + " " + text + "\r\n";
    result += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    result += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n";
    result += extraHeaders;
    result += "Connection: close\r\n\r\n" + body;
    return result;
}

QByteArray AdminServer::jsonResponse(int status, const QJsonObject& body,
                                      const QByteArray& cookieHeader) const {
    return response(status, "application/json; charset=utf-8",
                    QJsonDocument(body).toJson(QJsonDocument::Compact), cookieHeader);
}

QString AdminServer::header(const QByteArray& request, const QByteArray& name) const {
    const QList<QByteArray> lines = request.split('\n');
    for (const QByteArray& line : lines) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
            return QString::fromUtf8(line.mid(colon + 1).trimmed());
    }
    return {};
}

QByteArray AdminServer::cookie(const QByteArray& request, const QByteArray& name) const {
    const QByteArray value = header(request, "Cookie").toUtf8();
    for (const QByteArray& part : value.split(';')) {
        const int equal = part.indexOf('=');
        if (equal > 0 && part.left(equal).trimmed() == name) return part.mid(equal + 1).trimmed();
    }
    return {};
}

QJsonObject AdminServer::requestJson(const QByteArray& request) const {
    const int split = request.indexOf("\r\n\r\n");
    if (split < 0) return {};
    return QJsonDocument::fromJson(request.mid(split + 4)).object();
}

bool AdminServer::authorized(QTcpSocket* socket, const QByteArray& request, QByteArray* csrf) {
    Q_UNUSED(socket);
    cleanupSessions();
    const QByteArray sessionId = cookie(request, "wargame_admin");
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) return false;
    const QByteArray supplied = header(request, "X-CSRF-Token").toUtf8();
    if (csrf) *csrf = it->csrf;
    return !supplied.isEmpty() && supplied == it->csrf;
}

void AdminServer::cleanupSessions() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->expiresAt <= now) it = m_sessions.erase(it);
        else ++it;
    }
}

QByteArray AdminServer::page() const {
    return R"HTML(<!doctype html><meta charset="utf-8"><title>兵器推演服务器管理</title>
<style>body{font:14px system-ui;background:#0b1020;color:#e8edf5;max-width:960px;margin:32px auto;padding:0 18px}input,select,button{padding:9px;margin:4px;background:#121a2d;color:inherit;border:1px solid #304465;border-radius:4px}button{cursor:pointer;background:#2568a8}table{width:100%;border-collapse:collapse;margin-top:16px}td,th{padding:9px;border-bottom:1px solid #263653;text-align:left}.muted{color:#91a0be}.error{color:#ff6b7c}</style>
<h1>兵器推演服务器管理</h1><section id="login"><p class="muted">管理员登录</p><input id="au" placeholder="管理员账号"><input id="ap" type="password" placeholder="管理员密码"><button onclick="login()">登录</button><p id="le" class="error"></p></section>
<section id="app" hidden><h2>账号管理</h2><form onsubmit="add(event)"><input id="u" placeholder="账号（字母、数字、._-）" pattern="[A-Za-z0-9_.-]{1,64}" required><input id="p" type="password" placeholder="初始密码（至少8位）" minlength="8" required><input id="id" placeholder="用户ID" required><select id="r"><option>red</option><option>blue</option><option>director</option><option>editor</option><option>observer</option></select><select id="s"><option value="">无阵营</option><option>red</option><option>blue</option></select><button>创建账号</button></form><p id="msg" class="muted"></p><table><thead><tr><th>账号</th><th>用户ID</th><th>角色</th><th>阵营</th><th>状态</th><th>操作</th></tr></thead><tbody id="rows"></tbody></table></section>
<script>let csrf='';async function api(path,opt={}){opt.headers=Object.assign({'Content-Type':'application/json','X-CSRF-Token':csrf},opt.headers||{});let r=await fetch(path,opt);let j=await r.json().catch(()=>({}));if(!r.ok)throw Error(j.message||'请求失败');return j}function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}async function login(){try{let j=await api('/admin/api/login',{method:'POST',headers:{'X-CSRF-Token':''},body:JSON.stringify({username:au.value,password:ap.value})});csrf=j.csrf;login.hidden=true;app.hidden=false;load()}catch(e){le.textContent=e.message}}async function load(){let j=await api('/admin/api/accounts');rows.innerHTML=j.accounts.map(a=>`<tr><td>${esc(a.username)}</td><td>${esc(a.userId)}</td><td>${esc(a.role)}</td><td>${esc(a.side||'-')}</td><td>${a.disabled?'已禁用':'正常'}</td><td><button onclick="toggle('${encodeURIComponent(a.username)}',${!a.disabled})">${a.disabled?'启用':'禁用'}</button><button onclick="resetPassword('${encodeURIComponent(a.username)}')">重置密码</button><button onclick="del('${encodeURIComponent(a.username)}')">删除</button></td></tr>`).join('')}async function add(e){e.preventDefault();try{await api('/admin/api/accounts',{method:'POST',body:JSON.stringify({username:u.value,password:p.value,userId:id.value,role:r.value,side:s.value,roomId:'main',expiresAt:'2027-12-31T23:59:59Z'})});msg.textContent='账号已创建';e.target.reset();load()}catch(x){msg.textContent=x.message}}async function toggle(name,disabled){try{await api('/admin/api/accounts/'+name,{method:'PATCH',body:JSON.stringify({disabled})});load()}catch(e){msg.textContent=e.message}}async function resetPassword(name){let password=prompt('请输入新密码（至少8位）');if(password===null)return;try{await api('/admin/api/accounts/'+name,{method:'PATCH',body:JSON.stringify({password})});msg.textContent='密码已重置'}catch(e){msg.textContent=e.message}}async function del(name){if(!confirm('删除账号？'))return;try{await api('/admin/api/accounts/'+name,{method:'DELETE'});load()}catch(e){msg.textContent=e.message}}</script>)HTML";
}

void AdminServer::onConnection() {
    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void AdminServer::handle(QTcpSocket* socket) {
    QByteArray request = socket->property("adminRequest").toByteArray();
    request += socket->readAll();
    if (request.size() > 65536) { socket->disconnectFromHost(); return; }
    const int bodyOffset = request.indexOf("\r\n\r\n");
    if (bodyOffset < 0) {
        socket->setProperty("adminRequest", request);
        return;
    }
    bool contentLengthOk = false;
    const int contentLength = header(request, "Content-Length").toInt(&contentLengthOk);
    if (!contentLengthOk && !header(request, "Content-Length").isEmpty()) {
        socket->write(jsonResponse(400, {{"message", "Content-Length 无效"}})); socket->disconnectFromHost(); return;
    }
    const int expectedSize = bodyOffset + 4 + (contentLengthOk ? contentLength : 0);
    if (contentLength < 0 || expectedSize > 65536 || request.size() > expectedSize) {
        socket->write(jsonResponse(400, {{"message", "HTTP 请求长度无效"}})); socket->disconnectFromHost(); return;
    }
    if (request.size() < expectedSize) {
        socket->setProperty("adminRequest", request);
        return;
    }
    socket->setProperty("adminRequest", {});
    const QByteArray first = request.left(request.indexOf("\r\n"));
    const QList<QByteArray> parts = first.split(' ');
    if (parts.size() < 2 || parts[1] == "/admin" || parts[1] == "/admin/") {
        socket->write(response(200, "text/html; charset=utf-8", page())); socket->disconnectFromHost(); return;
    }
    const QByteArray path = parts[1].split('?').first();
    if (path == "/admin/api/login" && parts[0] == "POST") {
        const QJsonObject body = requestJson(request); QString error;
        const SessionIdentity admin = m_auth->authenticateAdmin(body.value("username").toString(), body.value("password").toString(), &error);
        if (!admin.isValid()) { socket->write(jsonResponse(401, {{"message", error}})); socket->disconnectFromHost(); return; }
        const QByteArray sid = randomHex(32), csrf = randomHex(24);
        m_sessions.insert(sid, {QDateTime::currentSecsSinceEpoch() + 3600, csrf});
        socket->write(jsonResponse(200, {{"csrf", QString::fromLatin1(csrf)}}, "Set-Cookie: wargame_admin=" + sid + "; HttpOnly; SameSite=Strict; Path=/admin\r\n"));
        socket->disconnectFromHost(); return;
    }
    QByteArray csrf;
    if (!authorized(socket, request, &csrf)) { socket->write(jsonResponse(403, {{"message", "管理员会话无效或 CSRF 校验失败"}})); socket->disconnectFromHost(); return; }
    if (path == "/admin/api/accounts" && parts[0] == "GET") {
        QJsonArray result; QString error;
        const auto accounts = m_persistence->passwordAccounts(&error);
        if (!error.isEmpty()) { socket->write(jsonResponse(500, {{"message", error}})); socket->disconnectFromHost(); return; }
        for (const auto& account : accounts) {
            result.append(QJsonObject{{QStringLiteral("username"), account.username},
                                      {QStringLiteral("userId"), account.identity.userId},
                                      {QStringLiteral("role"), account.identity.role},
                                      {QStringLiteral("side"), account.identity.side},
                                      {QStringLiteral("disabled"), account.disabled}});
        }
        socket->write(jsonResponse(200, {{"accounts", result}})); socket->disconnectFromHost(); return;
    }
    if (path == "/admin/api/accounts" && parts[0] == "POST") {
        const QJsonObject body = requestJson(request); const QString username = body.value("username").toString().trimmed();
        const QString password = body.value("password").toString();
        const QString role = body.value("role").toString();
        const QString side = body.value("side").toString();
        const bool sideValid = (role == QLatin1String(SessionRole::Red) && side == QLatin1String("red"))
            || (role == QLatin1String(SessionRole::Blue) && side == QLatin1String("blue"))
            || (role != QLatin1String(SessionRole::Red) && role != QLatin1String(SessionRole::Blue) && side.isEmpty());
        const QString userId = body.value("userId").toString().trimmed();
        const QString roomId = body.value("roomId").toString().trimmed();
        const QDateTime expiresAt = QDateTime::fromString(body.value("expiresAt").toString(), Qt::ISODate);
        if (username.isEmpty() || password.size() < 8 || password.size() > 512 || userId.isEmpty() || userId.size() > 128
            || roomId.isEmpty() || roomId.size() > 128 || !expiresAt.isValid()
            || expiresAt <= QDateTime::currentDateTimeUtc() || !AuthPolicy::isKnownRole(role) || !sideValid) { socket->write(jsonResponse(400, {{"message", "账号、密码、用户ID、房间、过期时间、角色或阵营无效"}})); socket->disconnectFromHost(); return; }
        const SessionIdentity identity{userId, role, side, roomId, expiresAt};
        const PasswordAccount account = AuthPolicy::makePasswordAccount(username, password, identity); QString error;
        AuthPolicy validator;
        if (!validator.setPasswordAccounts({account}, &error)) { socket->write(jsonResponse(400, {{"message", error}})); socket->disconnectFromHost(); return; }
        auto accounts = m_persistence->passwordAccounts(&error);
        if (!error.isEmpty()) { socket->write(jsonResponse(500, {{"message", error}})); socket->disconnectFromHost(); return; }
        if (std::any_of(accounts.cbegin(), accounts.cend(), [&](const auto& current) { return current.username == username; })) { socket->write(jsonResponse(409, {{"message", "账号已存在"}})); socket->disconnectFromHost(); return; }
        accounts.push_back(account);
        if (!m_persistence->syncPasswordAccounts(accounts, &error) || !m_auth->setPasswordAccounts(accounts, &error)) { socket->write(jsonResponse(409, {{"message", error}})); socket->disconnectFromHost(); return; }
        socket->write(jsonResponse(201, {{"message", "账号已创建"}})); socket->disconnectFromHost(); return;
    }
    const QByteArray prefix = "/admin/api/accounts/";
    if (path.startsWith(prefix)) {
        const QString username = QUrl::fromPercentEncoding(path.mid(prefix.size()));
        const QJsonObject body = requestJson(request); QString error; auto accounts = m_persistence->passwordAccounts(&error);
        if (!error.isEmpty()) { socket->write(jsonResponse(500, {{"message", error}})); socket->disconnectFromHost(); return; }
        auto it = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.username == username; });
        if (it == accounts.end()) { socket->write(jsonResponse(404, {{"message", "账号不存在"}})); socket->disconnectFromHost(); return; }
        if (parts[0] == "PATCH") {
            if (!body.contains("disabled") && !body.contains("password")) { socket->write(jsonResponse(400, {{"message", "缺少可更新字段"}})); socket->disconnectFromHost(); return; }
            if (body.contains("disabled") && !body.value("disabled").isBool()) { socket->write(jsonResponse(400, {{"message", "disabled 必须为布尔值"}})); socket->disconnectFromHost(); return; }
            if (body.contains("password") && !body.value("password").isString()) { socket->write(jsonResponse(400, {{"message", "password 必须为字符串"}})); socket->disconnectFromHost(); return; }
            if (body.contains("disabled")) it->disabled = body.value("disabled").toBool();
            if (body.contains("password")) {
                const QString password = body.value("password").toString();
                if (password.size() < 8 || password.size() > 512) { socket->write(jsonResponse(400, {{"message", "密码长度必须在 8 至 512 个字符之间"}})); socket->disconnectFromHost(); return; }
                const PasswordAccount replacement = AuthPolicy::makePasswordAccount(it->username, password, it->identity);
                it->salt = replacement.salt;
                it->passwordHash = replacement.passwordHash;
            }
        } else if (parts[0] == "DELETE") {
            if (!m_persistence->deletePasswordAccount(username, &error)) { socket->write(jsonResponse(409, {{"message", error}})); socket->disconnectFromHost(); return; }
            accounts.erase(it);
        }
        else { socket->write(jsonResponse(405, {{"message", "不支持的操作"}})); socket->disconnectFromHost(); return; }
        if (!m_persistence->syncPasswordAccounts(accounts, &error) || !m_auth->setPasswordAccounts(accounts, &error)) { socket->write(jsonResponse(409, {{"message", error}})); socket->disconnectFromHost(); return; }
        socket->write(jsonResponse(200, {{"message", "操作成功"}})); socket->disconnectFromHost(); return;
    }
    socket->write(response(404, "text/plain", "not found\n")); socket->disconnectFromHost();
}

} // namespace gbr
