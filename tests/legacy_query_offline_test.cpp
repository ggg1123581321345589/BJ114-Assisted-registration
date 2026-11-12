#include "native_service.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <mutex>

// Link the real Service with a deterministic, offline-only Broker substitute.
// No URL transport, browser, login or order implementation is present here.
struct Reply { int status; QByteArray bytes; };
static QList<Reply> replies;static QJsonArray calls;
namespace native {
struct Broker::Impl { bool live=true;int used=0,http=0;QString code;bool legacySelected=false;std::atomic_bool *cancel=nullptr; };
Broker::Broker(QString,std::atomic_bool *cancel):d(new Impl){d->cancel=cancel;}
Broker::~Broker()=default;
void Broker::prewarm(){d->live=true;}
void Broker::setCancel(std::atomic_bool *v){d->cancel=v;}
void Broker::selectHospital(const QString &v){d->code=v;}
void Broker::selectLegacyHospital(const QString &v){d->code=v;d->legacySelected=true;}
Response Broker::request(const QString &method,const QString &path,const QJsonObject &body,bool prepared){
 calls.append(QJsonObject{{"method",method},{"path",path},{"body",body},{"prepared",prepared},{"legacy_selected",d->legacySelected}});
 ++d->used;++d->http;if(replies.isEmpty())throw Failure("unexpected_extra_request");const auto reply=replies.takeFirst();return {reply.status,reply.bytes,path,{}};
}
Response Broker::requestBooking(const QString&,const QString&,const QJsonObject&,const QJsonObject&){throw Failure("booking_forbidden_in_offline_probe");}
QJsonArray Broker::sessionCookies()const{return {};}
void Broker::restoreSessionCookies(const QJsonArray&){}
bool Broker::alive()const{return d->live;}
int Broker::apiUsed()const{return d->used;}
int Broker::dispatches()const{return d->http;}
QJsonObject Broker::diagnostics()const{return {{"http_dispatches",d->http},{"api_requests",d->used},{"alive",d->live}};}
void Broker::close(){d->live=false;}
}
static Reply json(const QJsonObject &v){return {200,QJsonDocument(v).toJson(QJsonDocument::Compact)};}
static QJsonObject success(const QJsonObject &data){return {{"code","0000"},{"message","成功"},{"data",data}};}
static QJsonObject refusal(){return {{"code","1521"},{"message",QString::fromUtf8("当前日期无号，不允许查询")}};}
int main(int argc,char **argv){
 QCoreApplication app(argc,argv);const QString root=app.arguments().at(1),first="a7afe6b0325303f2ca9f6c66e8858a88",dept="200002245",date="20261113",iso="2026-11-13";std::atomic_bool cancelled(false);QJsonArray tests,scenarios;int failed=0;
 auto check=[&](const char *name,bool ok){tests.append(QJsonObject{{"name",name},{"pass",ok}});if(!ok)++failed;};
 const QJsonObject params{{"hospital_code","162"},{"department_code",dept},{"first_department_code",first},{"date",date}};
 auto legacy=[](QJsonValue v){return json({{"resCode",0},{"data",v}});};
 auto execute=[&](const char *name,const QList<Reply> &sequence){replies=sequence;calls={};native::Service service(root);auto result=service.execute("availability",params,&cancelled);service.close();scenarios.append(QJsonObject{{"scenario",name},{"calls",calls},{"result",result},{"unused_replies",replies.size()}});return result;};
 const Reply empty=json(success({{"dutyDate",QJsonValue(QJsonValue::Null)},{"doctors",QJsonArray()}})),routeFalse=json({{"code","0000"},{"data",false}});
 const QJsonObject leaf{{"code",dept},{"name",QString::fromUtf8("内分泌科")},{"dept1Code",first}},group{{"code",first},{"name",QString::fromUtf8("内科")},{"subList",QJsonArray{leaf}}};
 const Reply deps=legacy(QJsonObject{{"count",1},{"list",QJsonArray{group}}});
 auto calendar=[&](QString status="SOLD_OUT"){return legacy(QJsonObject{{"calendars",QJsonArray{QJsonObject{{"dutyDate",iso},{"status",status},{"weekDesc",QString::fromUtf8("周五")}}}}});};
 auto detail=[&](int web=0){return legacy(QJsonArray{QJsonObject{{"dutyCode","MORNING"},{"dutyDate",iso},{"detail",QJsonArray{QJsonObject{{"doctorName",""},{"doctorTitleName",QString::fromUtf8("医师|内分泌科")},{"doctorCode","fixture-doctor"},{"uniqProductKey","fixture-product"},{"wnumber",web},{"znumber",0},{"hnumber",0},{"period",QJsonArray()}}}}}});};
 auto noBooking=[&](QJsonArray rows){for(const auto &v:rows){const auto row=v.toObject();if(row.contains("booking_payload")||row.contains("product_status")||row.value("booking_supported").toBool())return false;}return true;};
 auto noRows=[&](QJsonObject r){return r.value("status")==QJsonValue("failed")&&r.value("rows").toArray().isEmpty()&&!r.value("business_success").toBool();};
 auto countPath=[&](QString path){int n=0;for(const auto &v:calls)if(v.toObject().value("path")==QJsonValue(path))++n;return n;};
 auto r=execute("legacy_http464_list",{empty,routeFalse,deps,Reply{464,{}}});
 check("legacy_464_is_failure_without_rows",noRows(r));check("legacy_464_preserved",r.value("failure").toString().contains("464"));check("legacy_464_no_new_calendar_fallback",countPath("/jtjk/mobile-service/product/calendar")==0);check("legacy_464_exact_four_calls",calls.size()==4&&replies.isEmpty());check("legacy_context_selected_before_old_calls",calls.size()>=4&&calls[2].toObject().value("legacy_selected").toBool()&&calls[3].toObject().value("legacy_selected").toBool());
 r=execute("legacy_calendar_and_doctor_sold_out",{empty,routeFalse,deps,calendar(),detail()});auto rows=r.value("rows").toArray();auto row=rows.isEmpty()?QJsonObject():rows[0].toObject();
 check("legacy_complete",r.value("status")==QJsonValue("complete")&&r.value("business_success").toBool());check("legacy_exact_one_doctor",rows.size()==1&&row.value("row_kind")==QJsonValue("legacy_doctor"));check("legacy_real_title_label",row.value("doctor_name")==QJsonValue(QString::fromUtf8("医师|内分泌科")));check("legacy_sold_out_status",row.value("status")==QJsonValue(QString::fromUtf8("约满")));check("legacy_no_synthetic_numeric_inventory",row.value("remaining").isNull());check("legacy_no_new_booking_identifiers",noBooking(rows));check("legacy_date_binding",row.value("date")==QJsonValue(date));check("legacy_old_endpoint_order",calls.size()==5&&calls[2].toObject().value("path")==QJsonValue("/mobile/department/hos/list")&&calls[3].toObject().value("path")==QJsonValue("/mobile/product/list")&&calls[4].toObject().value("path")==QJsonValue("/mobile/product/detail")&&replies.isEmpty());
 check("legacy_success_http_count_preserved",r.value("performance").toObject().value("http_dispatches")==QJsonValue(5));
 check("legacy_success_calendar_coverage_checked",r.value("coverage").toObject().value("calendar_checked")==QJsonValue(true));
 check("legacy_success_calendar_date_count",r.value("coverage").toObject().value("calendar_dates")==QJsonValue(1));

 if(calls.size()==5){const auto b=calls[3].toObject().value("body").toObject(),target=calls[4].toObject().value("body").toObject();check("legacy_list_exact_department_binding",b.value("hosCode")==QJsonValue("162")&&b.value("firstDeptCode")==QJsonValue(first)&&b.value("secondDeptCode")==QJsonValue(dept));check("legacy_detail_exact_server_target",target.value("target")==QJsonValue(iso));}else{check("legacy_list_exact_department_binding",false);check("legacy_detail_exact_server_target",false);}
 r=execute("legacy_available_remains_readonly",{empty,routeFalse,deps,calendar("AVAILABLE"),detail(1)});rows=r.value("rows").toArray();row=rows.isEmpty()?QJsonObject():rows[0].toObject();check("legacy_available_real_status",r.value("status")==QJsonValue("complete")&&row.value("status")==QJsonValue(QString::fromUtf8("有号")));check("legacy_available_not_new_booking",noBooking(rows));
 r=execute("legacy_no_inventory_calendar",{empty,routeFalse,deps,calendar("NO_INVENTORY")});rows=r.value("rows").toArray();row=rows.isEmpty()?QJsonObject():rows[0].toObject();check("legacy_no_inventory_is_calendar_not_doctor",r.value("status")==QJsonValue("complete")&&rows.size()==1&&row.value("row_kind")==QJsonValue("department_calendar")&&row.value("doctor_name")==QJsonValue(""));check("legacy_no_inventory_skips_detail",calls.size()==4&&countPath("/mobile/product/detail")==0&&replies.isEmpty());
 r=execute("legacy_unknown_status_rejected",{empty,routeFalse,deps,calendar("UNKNOWN_FIXTURE")});check("legacy_unknown_not_no_inventory",noRows(r));check("legacy_unknown_no_detail",countPath("/mobile/product/detail")==0);
 r=execute("legacy_detail_http464",{empty,routeFalse,deps,calendar(),Reply{464,{}}});check("legacy_detail464_atomic_empty_rows",noRows(r)&&r.value("failure").toString().contains("464"));
 r=execute("legacy_list_not_json",{empty,routeFalse,deps,Reply{200,"not JSON"}});check("legacy_nonjson_atomic_empty_rows",noRows(r));
 r=execute("legacy_list_business_rejected",{empty,routeFalse,deps,json({{"resCode",102},{"msg","Fixture denied"},{"data",QJsonObject()}})});check("legacy_business_rejection_atomic_empty_rows",noRows(r));
 r=execute("legacy_list_string_zero_rejected",{empty,routeFalse,deps,json({{"resCode","0"},{"data",QJsonObject{{"calendars",QJsonArray()}}}})});check("legacy_string_success_rejected",noRows(r));
 auto badGroup=group;auto badLeaf=leaf;badLeaf["dept1Code"]="OTHER";badGroup["subList"]=QJsonArray{badLeaf};r=execute("legacy_department_parent_mismatch",{empty,routeFalse,legacy(QJsonObject{{"count",1},{"list",QJsonArray{badGroup}}})});check("legacy_bad_parent_stops_before_list",noRows(r)&&countPath("/mobile/product/list")==0);
 const QJsonObject out{{"status",failed?"FAIL":"PASS"},{"failed",failed},{"count",tests.size()},{"tests",tests},{"scenarios",scenarios},{"live_requests",0},{"booking_requests",0}};const auto bytes=QJsonDocument(out).toJson(QJsonDocument::Compact);fwrite(bytes.constData(),1,bytes.size(),stdout);return failed?1:0;
}
