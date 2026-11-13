#pragma once
#include "native_backend.h"
#include <QElapsedTimer>
#include <QThread>
#include <functional>
namespace native {
class BookingService {
public:
 using Transport=std::function<Response(QString,QString,QJsonObject,QJsonObject)>;
 explicit BookingService(QString root,Transport fake=Transport());
 ~BookingService();
 QJsonObject execute(QString operation,QJsonObject params,std::atomic_bool *cancel);
 void close();
 QJsonObject exportParallelSession()const;
 void importParallelSession(const QJsonObject &snapshot);
 static QJsonObject offlineSelfTest();
 static QJsonObject offlineSessionSelfTest();
 static QJsonObject offlineParallelSelfTest();
private:
 QString sessionStorePathOverride_;QJsonArray fixtureSessionCookies_;bool sessionPersistenceDisabled_=false,brokerSeeded_=false,parallelSession_=false,parallelImported_=false,serviceUsed_=false;
 bool sessionStoreEnabled()const;bool saveSession();bool removeSavedSession();
 QString root_;Transport fake_;QThread *owner_;std::unique_ptr<Broker> broker_;QElapsedTimer born_,captchaClock_,smsClock_;
 std::atomic_bool *cancel_=nullptr;bool busy_=false,closed_=false,loggedIn_=false,authenticated_=false,captchaReady_=false;int smsRetry_=0;int operationRequestCount_=0;
 QJsonArray patients_;QString previewId_;QJsonObject previewRoute_,previewChecks_;QJsonArray previewPatients_;QElapsedTimer previewClock_;
 QString rulesHospital_;QJsonObject hospitalRules_;QElapsedTimer hospitalRulesClock_;
 void clearHospitalRules();void check();void prepare();void invalidateSession();void clearPreview();
 Response request(const QString &method,const QString &path,const QJsonObject &body=QJsonObject(),const QJsonObject &headers=QJsonObject());
 QJsonObject json(const QString &method,const QString &path,const QJsonObject &body=QJsonObject(),const QJsonObject &headers=QJsonObject());
 QString encryptPhone(const QString &phone);QJsonArray projection(const QJsonArray &patients)const;
};
}
