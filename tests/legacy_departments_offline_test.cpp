#include "legacy_departments.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
using namespace native::legacy;
static QJsonObject ok(QJsonValue data) { return {{"resCode",0},{"data",data}}; }
static QJsonObject leaf(QString code="SECOND",QString parent="FIRST") { return {{"code",code},{"name","Leaf"},{"dept1Code",parent}}; }
static QJsonObject group(QJsonArray leaves=QJsonArray{leaf()},QString code="FIRST") { return {{"code",code},{"name","Parent"},{"subList",leaves}}; }
static QJsonObject envelope(QJsonArray groups=QJsonArray{group()}) { return ok(QJsonObject{{"count",groups.size()},{"list",groups}}); }
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);QJsonArray checks;int errors=0;
 auto check=[&](QString name,bool pass){checks.append(QJsonObject{{"name",name},{"passed",pass}});if(!pass)++errors;};
 auto reject=[&](QString name,QJsonObject e,QString failure){auto r=parseDepartments(e,"162");check(name,r.value("status")==QJsonValue("failed")&&r.value("failure")==QJsonValue(failure)&&r.value("departments").toArray().isEmpty()&&r.value("rows").toArray().isEmpty());};
 auto r=parseDepartments(envelope(),"162");auto deps=r.value("departments").toArray();check("valid_mapping",r.value("status")==QJsonValue("complete")&&deps.size()==1&&deps[0].toObject().value("code")==QJsonValue("SECOND")&&deps[0].toObject().value("first_department_code")==QJsonValue("FIRST"));
 check("existing_cache_four_keys_exact",deps.size()==1&&deps[0].toObject().keys()==QStringList({"code","first_department_code","first_department_name","name"}));
 check("read_only_and_empty_availability_rows",r.value("read_only").toBool()&&!r.value("booking_supported").toBool()&&r.value("rows").toArray().isEmpty());
 r=parseDepartments(envelope(QJsonArray()),"162");check("empty_list_success_not_no_inventory",r.value("status")==QJsonValue("complete")&&r.value("departments").toArray().isEmpty());
 r=parseDepartments(envelope(QJsonArray{group(QJsonArray())}),"162");check("empty_parent_does_not_invent_leaf",r.value("status")==QJsonValue("complete")&&r.value("departments").toArray().isEmpty());
 auto count=envelope();auto data=count.value("data").toObject();data["count"]=999;count["data"]=data;r=parseDepartments(count,"162");check("count_not_used_as_leaf_quantity",r.value("department_count")==QJsonValue(1));
 auto numeric=envelope();numeric["resCode"]="0";reject("string_success_code_rejected",numeric,"legacy_envelope_invalid");
 auto business=envelope();business["resCode"]=102;business["msg"]="Login required";r=parseDepartments(business,"162");check("business_failure_preserves_message",r.value("status")==QJsonValue("failed")&&r.value("official_business_code")==QJsonValue(102)&&r.value("official_message")==QJsonValue("Login required"));
 reject("wrong_data_shape",ok(QJsonArray()),"legacy_department_schema_invalid");
 reject("missing_list",ok(QJsonObject()),"legacy_department_schema_invalid");
 reject("invalid_group",envelope(QJsonArray{false}),"legacy_department_group_invalid");
 auto g=group();g["code"]=17;reject("numeric_parent_id_rejected",envelope(QJsonArray{g}),"legacy_department_group_invalid");
 g=group();g.remove("subList");reject("missing_children_rejected",envelope(QJsonArray{g}),"legacy_department_group_invalid");
 g=group();g["name"]="";reject("missing_parent_name_rejected",envelope(QJsonArray{g}),"legacy_department_group_invalid");
 reject("duplicate_parent_rejected",envelope(QJsonArray{group(),group()}),"legacy_department_parent_ambiguous");
 reject("invalid_leaf_type",envelope(QJsonArray{group(QJsonArray{false})}),"legacy_department_leaf_invalid");
 auto l=leaf();l["code"]=17;reject("numeric_leaf_id_rejected",envelope(QJsonArray{group(QJsonArray{l})}),"legacy_department_leaf_invalid");
 l=leaf();l["name"]=QString("bad\nname");reject("leaf_control_text_rejected",envelope(QJsonArray{group(QJsonArray{l})}),"legacy_department_leaf_invalid");
 l=leaf();l.remove("dept1Code");reject("missing_parent_reference_rejected",envelope(QJsonArray{group(QJsonArray{l})}),"legacy_department_parent_mismatch");
 reject("mismatched_parent_reference_rejected",envelope(QJsonArray{group(QJsonArray{leaf("SECOND","OTHER")})}),"legacy_department_parent_mismatch");
 reject("duplicate_leaf_pair_rejected",envelope(QJsonArray{group(QJsonArray{leaf(),leaf()})}),"legacy_department_leaf_ambiguous");
 l=leaf();l["subList"]=QJsonArray{leaf()};reject("unexpected_depth_rejected",envelope(QJsonArray{group(QJsonArray{l})}),"legacy_department_depth_unsupported");
 r=parseDepartments(envelope(QJsonArray{group(),group(QJsonArray{leaf("SECOND","OTHER")},"OTHER")}),"162");check("same_second_with_different_first_preserved",r.value("departments").toArray().size()==2);
 reject("atomic_rejection_no_partial_departments",envelope(QJsonArray{group(),group(QJsonArray{leaf("SECOND","WRONG")},"OTHER")}),"legacy_department_parent_mismatch");
 r=parseDepartments(envelope(),"");check("hospital_identity_required",r.value("failure")==QJsonValue("legacy_hospital_code_invalid"));
 QFile fixture(app.arguments().value(1));bool fixtureRead=fixture.open(QIODevice::ReadOnly);auto bytes=fixtureRead?fixture.readAll():QByteArray();QJsonParseError e;auto doc=QJsonDocument::fromJson(bytes,&e);check("real_fixture_loads",fixtureRead&&e.error==QJsonParseError::NoError&&doc.isObject());
 r=parseDepartments(doc.object(),"162");deps=r.value("departments").toArray();check("real_fixture_has_18_groups_132_leaves",r.value("status")==QJsonValue("complete")&&r.value("first_department_count")==QJsonValue(18)&&deps.size()==132);
 int selected=0;for(const auto &v:deps){auto d=v.toObject();if(d.value("code")==QJsonValue("200002245")){++selected;check("real_endocrinology_parent_id_exact",d.value("first_department_code")==QJsonValue("a7afe6b0325303f2ca9f6c66e8858a88"));check("real_endocrinology_name_exact",d.value("name")==QJsonValue(QString::fromUtf8("内分泌科")));}}check("real_endocrinology_unique",selected==1);
 QJsonObject out{{"status",errors?"FAIL":"PASS"},{"count",checks.size()},{"failed",errors},{"tests",checks},{"network_requests",0},{"booking_requests",0},{"production_edits",false}};auto b=QJsonDocument(out).toJson(QJsonDocument::Indented);fwrite(b.constData(),1,b.size(),stdout);return errors?1:0;
}
