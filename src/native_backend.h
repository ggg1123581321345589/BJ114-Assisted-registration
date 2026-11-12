#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <atomic>
#include <memory>
#include <stdexcept>
namespace native {
struct Failure : std::runtime_error { explicit Failure(const QString &s):std::runtime_error(s.toStdString()){} };
struct Response { int status=0; QByteArray body; QString url; QByteArray contentType; };
class Broker {
public:
 Broker(QString projectRoot, std::atomic_bool *cancel);
 ~Broker();
 Broker(const Broker&)=delete; Broker&operator=(const Broker&)=delete;
 void prewarm();
 void setCancel(std::atomic_bool *cancel);
 void selectHospital(const QString &code);
 void selectLegacyHospital(const QString &code);
 Response request(const QString &method,const QString &path,const QJsonObject &body=QJsonObject(),bool requirePrepared=false);
 Response requestBooking(const QString &method,const QString &path,const QJsonObject &body=QJsonObject(),const QJsonObject &extraHeaders=QJsonObject());
 QJsonArray sessionCookies() const;
 void restoreSessionCookies(const QJsonArray &cookies);
 bool alive() const; int apiUsed() const; int dispatches() const;
 QJsonObject diagnostics() const;
 void close();
private: struct Impl; std::unique_ptr<Impl> d;
};
}
