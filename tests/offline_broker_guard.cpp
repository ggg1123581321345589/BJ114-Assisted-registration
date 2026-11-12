// Strictly offline Broker substitute for Window scheduler fixtures.
// Worker execution is disconnected by every fixture; any accidental business call fails closed.
#include "native_backend.h"
namespace native {
struct Broker::Impl { std::atomic_bool *cancel=nullptr; };
Broker::Broker(QString,std::atomic_bool *cancel):d(new Impl){d->cancel=cancel;}
Broker::~Broker()=default;
void Broker::prefetchPublicDns(){}
void Broker::prewarm(){throw Failure("unexpected_transport_in_offline_fixture");}
void Broker::setCancel(std::atomic_bool *value){d->cancel=value;}
void Broker::setPublicBackground(bool){}
void Broker::selectHospital(const QString&){}
void Broker::selectLegacyHospital(const QString&){}
Response Broker::request(const QString&,const QString&,const QJsonObject&,bool){throw Failure("unexpected_transport_in_offline_fixture");}
Response Broker::requestBooking(const QString&,const QString&,const QJsonObject&,const QJsonObject&){throw Failure("unexpected_booking_in_offline_fixture");}
QJsonArray Broker::sessionCookies()const{return {};}
void Broker::restoreSessionCookies(const QJsonArray&){}
bool Broker::alive()const{return false;}
int Broker::apiUsed()const{return 0;}
int Broker::dispatches()const{return 0;}
QJsonObject Broker::diagnostics()const{return {{"http_dispatches",0},{"external_dns_lookups",0},{"offline_fixture",true}};}
void Broker::close(){}
}
