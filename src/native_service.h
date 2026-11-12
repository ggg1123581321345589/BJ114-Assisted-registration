#pragma once
#include "native_backend.h"
#include <QJsonArray>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QThread>
#include <functional>
namespace native {
class Service {
public:
 explicit Service(QString root);
 ~Service();
 QJsonObject execute(const QString &operation,const QJsonObject &params,std::atomic_bool *cancel);
 static QJsonObject cacheOnly(const QString &root,const QString &hospitalCode);
 static QJsonObject crawlAllDepartments(const QString &root,std::atomic_bool *cancel,int workers=4);
 static QJsonArray flattenDepartments(const QJsonValue &data);
 static QJsonArray doctorRows(const QJsonObject &data,const QJsonObject &department,const QString &date);
 static QJsonObject planCalendarRows(const QJsonObject &data,const QJsonObject &department,const QJsonArray &dates,const QString &stamp);
 static QJsonObject offlineSelfTest();
 void close();
private:
 QString root_; std::unique_ptr<Broker> broker_; QElapsedTimer born_; int session_=0;
 std::atomic_bool *cancel_=nullptr;
 QThread *owner_=nullptr; bool busy_=false,closed_=false; QString currentCode_,renewReason_,cacheWarning_;
 bool publicBackground_=false;
 bool reused_=false; QHash<QString,QJsonObject> metadata_; QSet<QString> invalidated_; QJsonArray responses_; QHash<QString,bool> hospitalNewRoute_;
 void prepare(int required,bool allowBootstrap=true);
 QJsonObject queryJson(const QString &method,const QString &path,const QJsonObject &body=QJsonObject(),bool requirePrepared=false);
 QJsonObject queryLegacyJson(const QString &method,const QString &path,const QJsonObject &body=QJsonObject());
 QJsonArray legacyAvailability(const QString &code,const QJsonObject &department,const QString &date,QJsonObject &report);
 QJsonArray legacyBatchCalendar(const QString &code,const QJsonObject &department,QJsonObject &selected);
 QJsonArray legacyBatchAvailability(const QString &code,const QJsonObject &department,const QString &date,const QJsonArray &calendar,QJsonObject &report);
 QJsonArray departments(const QString &code,bool force,bool allowNetwork,bool freshNetwork,QJsonObject &metadata);
};
}
