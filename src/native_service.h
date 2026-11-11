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
 static QJsonObject offlineSelfTest();
 void close();
private:
 QString root_; std::unique_ptr<Broker> broker_; QElapsedTimer born_; int session_=0;
 std::atomic_bool *cancel_=nullptr;
 QThread *owner_=nullptr; bool busy_=false,closed_=false; QString currentCode_,renewReason_,cacheWarning_;
 bool reused_=false; QHash<QString,QJsonObject> metadata_; QSet<QString> invalidated_; QJsonArray responses_;
 void prepare(int required);
 QJsonObject queryJson(const QString &method,const QString &path,const QJsonObject &body=QJsonObject());
 QJsonArray departments(const QString &code,bool force,bool allowNetwork,bool freshNetwork,QJsonObject &metadata);
};
}
