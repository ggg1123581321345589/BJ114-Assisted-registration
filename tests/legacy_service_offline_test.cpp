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
 calls.append(QJsonObject{{"method",method},{"path",path},{"body",body},{"prepared",prepared}});
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
 QCoreApplication app(argc,argv);const QString root=app.arguments().at(1),first="a536bbf2f1d71d2b6af6c7f79e5a2963",dept="200140299",date="20261111";std::atomic_bool cancelled(false);QJsonArray tests,scenarios;int failed=0;
 auto check=[&](const char *name,bool ok){tests.append(QJsonObject{{"name",name},{"pass",ok}});if(!ok)++failed;};
 QJsonObject params{{"hospital_code","162"},{"department_code",dept},{"first_department_code",first},{"date",date}};
 auto execute=[&](const char *name,const QList<Reply> &sequence){replies=sequence;calls={};native::Service service(root);auto result=service.execute("availability",params,&cancelled);service.close();scenarios.append(QJsonObject{{"scenario",name},{"calls",calls},{"result",result}});return result;};
 QJsonObject period{{"dutyTimeView","09:00-09:30"},{"ncode",3},{"uniqProductKey","period-fixture"}},duty{{"productStatus",1},{"uniqueProductKey","duty-fixture"},{"period",QJsonArray{period}}},doctor{{"doctorName","Fixture Doctor"},{"doctorCode","fixture-doctor"},{"firstDeptCode",first},{"secondDeptCode",dept},{"detail",QJsonArray{duty}}};
 auto r=execute("normal_one_request",{json(success({{"dutyDate",date},{"doctors",QJsonArray{doctor}}}))});
 check("normal_complete",r.value("status")==QJsonValue("complete"));check("normal_one_request",calls.size()==1&&replies.isEmpty());
 auto rows=r.value("rows").toArray();check("normal_real_doctor_row",rows.size()==1&&rows[0].toObject().value("doctor_name")==QJsonValue("Fixture Doctor"));auto route=rows[0].toObject().value("booking_payload").toObject();
 check("normal_payload_bound_hospital",route.value("hosCode")==QJsonValue("162"));check("normal_payload_bound_department",route.value("firstDeptCode")==QJsonValue(first)&&route.value("secondDeptCode")==QJsonValue(dept));check("normal_payload_bound_ids",route.value("uniqueProductKey")==QJsonValue("duty-fixture")&&route.value("uniqueProductTimeKey")==QJsonValue("period-fixture"));check("normal_payload_bound_date",route.value("dutyDate")==QJsonValue(date));check("normal_remaining",rows[0].toObject().value("remaining")==QJsonValue(3));check("request_body_exact_date",calls[0].toObject().value("body").toObject().value("dutyDate")==QJsonValue(date));
 QJsonArray future{QJsonObject{{"dutyDate","20261112"},{"status",4},{"statusView",QString::fromUtf8("无号")}},QJsonObject{{"dutyDate","20261119"},{"status",6},{"statusView",QString::fromUtf8("即将放号")}}};
 r=execute("1521_date_not_public",{json(refusal()),json(success({{"calendars",future}}))});rows=r.value("rows").toArray();auto row=rows[0].toObject();
 check("missing_date_complete",r.value("status")==QJsonValue("complete")&&r.value("business_success")==QJsonValue(true));check("missing_date_two_requests",calls.size()==2&&replies.isEmpty());check("missing_date_no_detail_retry",calls[1].toObject().value("path")==QJsonValue("/jtjk/mobile-service/product/calendar"));check("missing_date_unavailable_row",rows.size()==1&&row.value("row_kind")==QJsonValue("date_unavailable"));check("missing_date_retains_requested_date",row.value("date")==QJsonValue(date));check("missing_date_no_fake_doctor",row.value("doctor_name")==QJsonValue(""));check("missing_date_no_fake_remaining",row.value("remaining").isNull());check("missing_date_no_booking_payload",!row.contains("booking_payload")&&!row.contains("product_status"));check("missing_date_doctor_count_unknown",r.value("doctor_count").isNull());check("missing_date_public_dates_exact",r.value("available_dates")==QJsonValue(QJsonArray{"20261112","20261119"}));check("missing_date_in_calendar_false",r.value("selected_date_in_calendar")==QJsonValue(false));check("missing_date_official_message",r.value("official_message")==refusal().value("message"));check("missing_date_user_message_range",r.value("message").toString().contains("2026-11-12")&&r.value("message").toString().contains("2026-11-19"));check("missing_date_fresh_calendar_rows",r.value("calendar_rows").toArray().size()==2);
 QJsonArray present{QJsonObject{{"dutyDate",date},{"status",4},{"statusView",QString::fromUtf8("无号")}}};r=execute("1521_date_present_no_slots",{json(refusal()),json(success({{"calendars",present}}))});rows=r.value("rows").toArray();row=rows[0].toObject();check("present_date_complete",r.value("status")==QJsonValue("complete"));check("present_date_official_row_only",rows.size()==1&&row.value("row_kind")==QJsonValue("department_calendar")&&row.value("status")==QJsonValue(QString::fromUtf8("无号")));check("present_date_unknown_count_no_booking",row.value("remaining").isNull()&&!row.contains("booking_payload"));check("present_date_in_calendar_true",r.value("selected_date_in_calendar")==QJsonValue(true));
 for(const auto &entry:QList<QPair<QString,Reply>>{{"calendar_business_reject",json({{"code","9001"},{"message","Fixture calendar rejected"}})},{"calendar_non_json",{200,"not JSON"}},{"calendar_http429",{429,"too many requests"}}}){
  const QString key=entry.first;r=execute(key.toLatin1().constData(),{json(refusal()),entry.second});check((key+"_fails").toLatin1().constData(),r.value("status")==QJsonValue("failed")&&!r.value("business_success").toBool());check((key+"_no_retry").toLatin1().constData(),calls.size()==2&&replies.isEmpty());check((key+"_no_fabricated_rows").toLatin1().constData(),r.value("rows").toArray().isEmpty());
 }
 r=execute("other_business_code",{json({{"code","9999"},{"message","Fixture explicit official rejection"}})});check("other_code_fails_without_calendar",r.value("status")==QJsonValue("failed")&&calls.size()==1);check("other_code_keeps_message",r.value("responses").toArray()[0].toObject().value("business_message")==QJsonValue("Fixture explicit official rejection"));
 r=execute("calendar_duplicate_selected_date",{json(refusal()),json(success({{"calendars",QJsonArray{present[0],present[0]}}}))});check("duplicate_calendar_date_fails",r.value("failure")==QJsonValue("selected_date_missing_or_ambiguous"));

 const QString detailPath="/jtjk/mobile-service/product/doctor/detail",routePath="/jtjk/mobile-service/user/collect/newHospital/162",calendarPath="/jtjk/mobile-service/product/calendar";
 auto emptyDetail=[&](){return json(success({{"dutyDate",QJsonValue(QJsonValue::Null)},{"doctors",QJsonArray()}}));};
 auto routeReply=[&](QJsonValue value){return json(QJsonObject{{"code","0000"},{"message","成功"},{"data",value}});};
 auto countPath=[&](const QString &path){int n=0;for(const auto &v:calls)if(v.toObject().value("path")==QJsonValue(path))++n;return n;};
 r=execute("legacy_false_stops_before_new_calendar",{emptyDetail(),routeReply(false)});
 check("legacy_false_attempts_legacy_get_and_missing_reply_fails",r.value("status")==QJsonValue("failed")&&r.value("failure")==QJsonValue("unexpected_extra_request")&&!r.value("business_success").toBool());
 check("legacy_false_no_rows_or_fake_no_availability",r.value("rows").toArray().isEmpty()&&!r.contains("coverage")&&!r.contains("calendar_rows"));
 check("legacy_false_exact_three_requests_includes_legacy_get_no_calendar",calls.size()==3&&calls[2].toObject().value("path")==QJsonValue("/mobile/department/hos/list")&&countPath(calendarPath)==0&&replies.isEmpty());
 check("legacy_false_route_get_exact_hospital_empty_body_then_legacy",calls.size()==3&&calls[1].toObject().value("method")==QJsonValue("GET")&&calls[1].toObject().value("path")==QJsonValue(routePath)&&calls[1].toObject().value("body").toObject().isEmpty());
 check("legacy_false_system_and_new_attempt_failure_truthful",r.value("hospital_system")==QJsonValue("legacy")&&r.value("message").toString().contains("unexpected_extra_request")&&r.value("message").toString().contains(QString::fromUtf8("未取得可信排班")));
 check("legacy_false_failure_invalidates_broker",r.value("session_invalidated")==QJsonValue(true)&&r.value("session_kept_open")==QJsonValue(false));
 r=execute("new_true_preserves_calendar_fallback",{emptyDetail(),routeReply(true),json(success({{"calendars",present}}))});rows=r.value("rows").toArray();row=rows.isEmpty()?QJsonObject():rows[0].toObject();
 check("new_true_complete_and_new_system",r.value("status")==QJsonValue("complete")&&r.value("hospital_system")==QJsonValue("new"));
 check("new_true_exact_three_request_order",calls.size()==3&&calls[0].toObject().value("path")==QJsonValue(detailPath)&&calls[1].toObject().value("path")==QJsonValue(routePath)&&calls[2].toObject().value("path")==QJsonValue(calendarPath)&&replies.isEmpty());
 check("new_true_only_official_calendar_row",rows.size()==1&&row.value("row_kind")==QJsonValue("department_calendar")&&row.value("status")==QJsonValue(QString::fromUtf8("无号"))&&row.value("date")==QJsonValue(date));
 check("new_true_unknown_count_no_fake_doctor_or_payload",row.value("remaining").isNull()&&row.value("doctor_name")==QJsonValue("")&&!row.contains("booking_payload"));
 check("new_true_preserves_calendar_coverage",r.value("coverage").toObject().value("calendar_checked")==QJsonValue(true)&&r.value("coverage").toObject().value("selected_date_evidence")==QJsonValue("calendar_after_empty_detail"));
 // The route memo belongs to Service, and survives replacement of an invalidated Broker.
 {native::Service service(root);replies={emptyDetail(),routeReply(false)};calls={};auto firstResult=service.execute("availability",params,&cancelled);const auto firstCalls=calls;
  replies={emptyDetail()};calls={};auto secondResult=service.execute("availability",params,&cancelled);scenarios.append(QJsonObject{{"scenario","legacy_false_cached_across_failed_queries"},{"first_calls",firstCalls},{"calls",calls},{"first_result",firstResult},{"result",secondResult}});
  check("legacy_false_repeated_query_cached_route_but_attempts_legacy_get",calls.size()==2&&calls[1].toObject().value("path")==QJsonValue("/mobile/department/hos/list")&&countPath(routePath)==0&&countPath(calendarPath)==0&&replies.isEmpty());
  check("legacy_false_repeated_query_missing_old_reply_explicit_no_rows",secondResult.value("failure")==QJsonValue("unexpected_extra_request")&&secondResult.value("rows").toArray().isEmpty()&&secondResult.value("hospital_system")==QJsonValue("legacy"));
  replies={json(success({{"dutyDate",date},{"doctors",QJsonArray{doctor}}}))};calls={};auto laterDoctor=service.execute("availability",params,&cancelled);scenarios.append(QJsonObject{{"scenario","real_doctor_after_cached_legacy_route"},{"calls",calls},{"result",laterDoctor}});
  check("real_doctor_is_not_overridden_by_legacy_cache",laterDoctor.value("status")==QJsonValue("complete")&&laterDoctor.value("rows").toArray().size()==1&&calls.size()==1&&countPath(routePath)==0&&countPath(calendarPath)==0);
  service.close();}
 {native::Service service(root);replies={emptyDetail(),routeReply(true),json(success({{"calendars",present}}))};calls={};auto firstResult=service.execute("availability",params,&cancelled);
  replies={emptyDetail(),json(success({{"calendars",present}}))};calls={};auto secondResult=service.execute("availability",params,&cancelled);scenarios.append(QJsonObject{{"scenario","new_true_cached_on_repeated_empty_detail"},{"calls",calls},{"first_result",firstResult},{"result",secondResult}});
  check("new_true_repeated_empty_uses_cached_route",secondResult.value("status")==QJsonValue("complete")&&calls.size()==2&&countPath(routePath)==0&&countPath(calendarPath)==1&&replies.isEmpty());service.close();}
 r=execute("real_doctor_no_additional_route_probe",{json(success({{"dutyDate",date},{"doctors",QJsonArray{doctor}}}))});
 check("real_doctor_single_request_no_route_or_calendar",r.value("status")==QJsonValue("complete")&&calls.size()==1&&countPath(routePath)==0&&countPath(calendarPath)==0);
 r=execute("1521_keeps_old_fresh_calendar_fallback",{json(refusal()),json(success({{"calendars",future}}))});
 check("1521_does_not_probe_hospital_route",calls.size()==2&&countPath(routePath)==0&&countPath(calendarPath)==1&&replies.isEmpty());
 check("1521_still_retains_exact_public_dates",r.value("status")==QJsonValue("complete")&&r.value("available_dates")==QJsonValue(QJsonArray{"20261112","20261119"})&&r.value("rows").toArray()[0].toObject().value("row_kind")==QJsonValue("date_unavailable"));
 const QList<QPair<QString,QJsonValue>> malformed{{"string_false",QJsonValue("false")},{"numeric_zero",QJsonValue(0)},{"null",QJsonValue(QJsonValue::Null)},{"object",QJsonValue(QJsonObject{{"newHospital",false}})},{"array",QJsonValue(QJsonArray{false})},{"missing",QJsonValue(QJsonValue::Undefined)}};
 for(const auto &entry:malformed){auto packet=QJsonObject{{"code","0000"},{"message","fixture route"}};if(!entry.second.isUndefined())packet["data"]=entry.second;r=execute(("malformed_route_"+entry.first).toLatin1().constData(),{emptyDetail(),json(packet)});
  check(("malformed_route_"+entry.first+"_schema_refused").toLatin1().constData(),r.value("status")==QJsonValue("failed")&&r.value("failure")==QJsonValue("hospital_route_schema_invalid"));
  check(("malformed_route_"+entry.first+"_no_calendar_no_rows").toLatin1().constData(),calls.size()==2&&countPath(calendarPath)==0&&r.value("rows").toArray().isEmpty()&&replies.isEmpty());}
 {native::Service service(root);replies={emptyDetail(),routeReply("false")};calls={};auto badResult=service.execute("availability",params,&cancelled);replies={emptyDetail(),routeReply(true),json(success({{"calendars",present}}))};calls={};auto goodResult=service.execute("availability",params,&cancelled);scenarios.append(QJsonObject{{"scenario","invalid_route_not_cached"},{"first_result",badResult},{"calls",calls},{"result",goodResult}});check("malformed_route_not_cached_retry_probes_again",goodResult.value("status")==QJsonValue("complete")&&calls.size()==3&&countPath(routePath)==1);service.close();}
 r=execute("route_http_failure_no_calendar",{emptyDetail(),Reply{429,"fixture throttle"}});check("route_http_error_stops_without_calendar",r.value("failure")==QJsonValue("http_429")&&r.value("rows").toArray().isEmpty()&&calls.size()==2&&countPath(calendarPath)==0);
 r=execute("route_business_failure_no_calendar",{emptyDetail(),json(QJsonObject{{"code","9001"},{"message","fixture route denied"},{"data",true}})});check("route_business_error_stops_without_calendar",r.value("failure")==QJsonValue("business_response_rejected")&&r.value("rows").toArray().isEmpty()&&calls.size()==2&&countPath(calendarPath)==0);
 {native::Service service(root);calls={};replies={};auto warmed=service.execute("prewarm",QJsonObject{{"hospital_code","162"}},&cancelled);auto p=params;p["require_prepared_session"]=true;replies={emptyDetail(),routeReply(true),json(success({{"calendars",present}}))};calls={};auto prepared=service.execute("availability",p,&cancelled);bool flags=true;for(const auto &call:calls)flags&=call.toObject().value("prepared")==QJsonValue(true);scenarios.append(QJsonObject{{"scenario","prepared_query_route_uses_same_prepared_constraint"},{"warm",warmed},{"calls",calls},{"result",prepared}});check("prepared_route_respects_prepared_flag_on_every_call",prepared.value("status")==QJsonValue("complete")&&calls.size()==3&&flags);service.close();}

 const QJsonObject out{{"status",failed?"FAIL":"PASS"},{"failed",failed},{"count",tests.size()},{"tests",tests},{"scenarios",scenarios},{"live_requests",0},{"booking_requests",0}};const auto bytes=QJsonDocument(out).toJson(QJsonDocument::Compact);fwrite(bytes.constData(),1,bytes.size(),stdout);return failed?1:0;
}
