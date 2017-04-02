#include "websocketconnection.h"

WebSocketConnection::WebSocketConnection(QObject *parent) :
    QObject(parent), m_connectionStatus(1) {

    m_enc = new Encryptor(this);
    m_msgs = new MessageManager(this);

    m_timer = new QTimer(this);
    connect(m_timer, SIGNAL(timeout()),
            &m_ws, SLOT(ping()));

    m_checker = new QTimer(this);
    m_checker->setSingleShot(true);
    connect(&m_ws, SIGNAL(pong(quint64,QByteArray)),
            m_checker, SLOT(stop()));
    connect(m_checker, SIGNAL(timeout()),
            this, SIGNAL(networkError()));

    connect(&m_ws, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
            this, SLOT(onStateChanged(QAbstractSocket::SocketState)));

    connect(&m_ws, SIGNAL(textMessageReceived(QString)),
            this, SLOT(onReceived(QString)));

    connect(&m_ws, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onError(QAbstractSocket::SocketError)));

    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    connect(&m_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors),
            this, &WebSocketConnection::onSslErrors);
}

QJsonObject *WebSocketConnection::sendRequestMessage(const QString &cmd, const QJsonValue &data) {
    auto msg = m_msgs->makeRequestMessage(cmd, data);
    if (msg == nullptr) {
        return msg;
    }
    send(*msg);
    return msg;
}

QJsonObject *WebSocketConnection::sendResponseMessage(const QJsonObject &reqMsg, const QJsonValue &data) {
    auto msg = m_msgs->makeResponseMessage(reqMsg, data);
    if (msg == nullptr) {
        return msg;
    }
    send(*msg);
    return msg;
}

void WebSocketConnection::send(QJsonObject &obj) {
    qInfo() << "[WebSocketConnection::send]" << obj;
    // Make random Signature.
    auto signature = m_enc->makeKey();

    // Make Data into Package with Signature.
    auto package = Package::MakePackage(obj, signature);

    // Encrypt Data and Signature.
    auto encSignature = m_enc->encrypt(signature);
    auto encPackage = m_enc->encrypt(package);

    // Join with dot.
    QByteArray out;
    out.append(encSignature).append('.').append(encPackage);

    m_ws.sendTextMessage(QString::fromLatin1(out));
}

void WebSocketConnection::onStateChanged(QAbstractSocket::SocketState state) {
    switch (state) {
    case QAbstractSocket::ConnectedState:
        m_timer->start(TIMER_INTERVAL_MS);

    case QAbstractSocket::UnconnectedState:
        m_enc->resetKey();
        m_msgs->resetIds();
        m_timer->stop();

    default:
    {}
    }
    m_connectionStatus = state;
    emit connectionStatusChanged();
    qDebug() << "[WebSocketConnection::onStateChanged] State:" << state;
}

void WebSocketConnection::onReceived(QString data) {

    // Split msg into Signature and Data.
    QStringList split = data.split('.', QString::SkipEmptyParts);
    if (split.length() != 2) {
        qDebug("Expected %d parts, got %d", 2, split.length());
        return;
    }

    auto encSignature = split.at(0).toLatin1();
    auto encPackage = split.at(1).toLatin1();

    // Decrypt Signature and Data.
    auto signature = m_enc->decrypt(encSignature);
    auto package = m_enc->decrypt(encPackage);

    // Vertify Data with Signature.
    auto msg = Package::ReadPackage(package, signature);
    qInfo() << "[WebSocketConnection::onReceived]" << msg;
    if (m_msgs->checkIncomingMessage(msg) == false) {
        return;
    }

    process(msg);
}

void WebSocketConnection::process(const QJsonObject &obj) {
    MSG::Message msg = MSG::obj_to_struct(obj);

    if (msg.cmd == "handshake")
        ps_handshake(msg);

    if (msg.cmd == "new_chef")
        ps_new_chef(msg);

}

bool WebSocketConnection::ps_handshake(const MSG::Message &msg) {
    if (msg.typ != TYPE_REQUEST) {
        m_ws.close(QWebSocketProtocol::CloseCodeWrongDatatype,
                   "handshake is not request type");
        return false;
    }
    if (msg.data.isString() == false) {
        m_ws.close(QWebSocketProtocol::CloseCodeWrongDatatype,
                   "data does not contain string key");
        return false;
    }
    auto obj = sendResponseMessage(MSG::struct_to_obj(msg), true);
    if (obj == nullptr) {
        qDebug() << "[WebSocketConnection::ps_handshake]"
                 << "Failed to create response; got nullptr.";
        return false;
    }
    m_enc->setKey(msg.data.toString().toLatin1());
    return true;
}

bool WebSocketConnection::ps_new_chef(const MSG::Message &msg) {
    if (msg.typ != TYPE_RESPONSE)
        qDebug() << "[ WebSocketConnection::ps_new_chef]"
                 << "Got a request to create new chef from server? haha";

    if (msg.data.isString() == false)
        qDebug() << "[ WebSocketConnection::ps_new_chef]"
                 << "Invalid response from server.";

    emit responseTextMessage(msg.req->id, msg.data.toString());
    return true;
}
