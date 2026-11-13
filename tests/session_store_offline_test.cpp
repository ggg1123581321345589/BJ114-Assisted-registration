// Only synthetic cookies and explicit QTemporaryDir paths are used by this fixture.
#include "booking_session_store.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
using native::BookingSessionStore;
namespace {
QJsonArray checks; int failed=0;
void check(const char *name,bool pass){checks.append(QJsonObject{{"name",name},{"pass",pass}});if(!pass)++failed;}
QJsonObject cookie(){return {{"key","fixture_session"},{"value","synthetic-offline-cookie"},{"domain","www.114yygh.com"},{"path","/"},{"hostOnly",true},{"secure",true},{"httpOnly",true},{"expires",QJsonValue::Null},{"sameSite","lax"}};}
bool rejected(const QJsonArray&a){try{BookingSessionStore::validatedCookies(a);return false;}catch(...){return true;}}
QByteArray bytes(const QString&p){QFile f(p);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
bool write(const QString&p,const QByteArray&a){QFile f(p);return f.open(QIODevice::WriteOnly)&&f.write(a)==a.size();}
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QTemporaryDir temp;
 check("temporary_directory_created",temp.isValid());if(!temp.isValid())return 1;
 const QString path=temp.filePath("session.dpapi");const QJsonArray input{cookie()};
 check("synthetic_valid_cookie_normalized",BookingSessionStore::validatedCookies(input).size()==1);
 auto mutate=[&](const char*name,const char*field,QJsonValue value){auto c=cookie();c[field]=value;check(name,rejected({c}));};
 check("empty_array_rejected",rejected({}));check("nonobject_rejected",rejected({1}));
 mutate("foreign_domain_rejected","domain","example.invalid");mutate("parent_domain_rejected","domain","evil114yygh.com");
 mutate("extra_phone_field_rejected","phone","synthetic");mutate("extra_patient_field_rejected","patient_id","synthetic");
 mutate("empty_key_rejected","key","");mutate("invalid_key_rejected","key","bad;key");
 mutate("control_value_rejected","value",QString("bad\nvalue"));mutate("delimiter_value_rejected","value","bad;value");
 mutate("nonstring_value_rejected","value",1);mutate("oversized_value_rejected","value",QString(16385,'x'));
 mutate("relative_path_rejected","path","relative");mutate("control_path_rejected","path",QString("/bad\n"));
 mutate("invalid_host_boolean_rejected","hostOnly","true");mutate("invalid_secure_boolean_rejected","secure",1);
 mutate("invalid_httponly_boolean_rejected","httpOnly","false");mutate("negative_expiry_rejected","expires",-1);
 mutate("invalid_expiry_type_rejected","expires","tomorrow");mutate("invalid_samesite_rejected","sameSite","invalid");
 auto conflict=cookie();conflict["domainSpecified"]=true;check("inconsistent_domain_specified_rejected",rejected({conflict}));
 check("duplicate_identity_rejected",rejected({cookie(),cookie()}));
 auto expired=cookie();expired["expires"]=1;check("expired_only_rejected",rejected({expired}));
 expired["key"]="expired_fixture";check("expired_cookie_filtered",BookingSessionStore::validatedCookies({expired,cookie()}).size()==1);
 auto alias=cookie();alias.remove("key");alias["name"]="fixture_alias";check("name_alias_normalized",BookingSessionStore::validatedCookies({alias})[0].toObject().value("key")==QJsonValue("fixture_alias"));
 auto upper=cookie();upper["domain"]="WWW.114YYGH.COM";check("domain_case_normalized",BookingSessionStore::validatedCookies({upper})[0].toObject().value("domain")==QJsonValue("www.114yygh.com"));
 QJsonArray many;for(int i=0;i<129;++i){auto c=cookie();c["key"]=QString("fixture_%1").arg(i);many.append(c);}check("cookie_count_limit_rejected",rejected(many));
 check("initial_missing_state",BookingSessionStore::load(path).state=="missing");check("initial_saved_probe_false",!BookingSessionStore::hasSavedSessionAt(path));
#ifdef Q_OS_WIN
 const bool saved=BookingSessionStore::save(input,path);check("current_user_dpapi_save",saved);const auto encrypted=bytes(path);
 check("ciphertext_magic",encrypted.startsWith("HBSESS1\n"));check("ciphertext_hides_cookie_value",!encrypted.contains("synthetic-offline-cookie"));
 check("ciphertext_hides_cookie_key",!encrypted.contains("fixture_session"));check("ciphertext_hides_cookie_domain",!encrypted.contains("114yygh.com"));
 check("saved_probe_true",BookingSessionStore::hasSavedSessionAt(path));const auto loaded=BookingSessionStore::load(path);
 check("current_user_dpapi_roundtrip",loaded.state=="loaded"&&loaded.cookies==BookingSessionStore::validatedCookies(input));
 auto changed=cookie();changed["value"]="synthetic-replacement";check("atomic_replace_save",BookingSessionStore::save({changed},path));
 check("atomic_replace_roundtrip",BookingSessionStore::load(path).cookies[0].toObject().value("value")==QJsonValue("synthetic-replacement"));
 check("invalid_save_rejected",!BookingSessionStore::save({},path));check("invalid_save_preserves_existing_ciphertext",BookingSessionStore::load(path).state=="loaded");
 const auto good=bytes(path);QByteArray tampered=good;if(tampered.size()>16)tampered[tampered.size()-1]=char(tampered.back()^0x40);
 const QString corrupt=temp.filePath("tampered.dpapi");check("tampered_fixture_written",write(corrupt,tampered));
 check("tampered_ciphertext_rejected",BookingSessionStore::load(corrupt).state=="invalid");check("tampered_file_removed",!QFileInfo::exists(corrupt));
 const QString bad=temp.filePath("bad-header.dpapi");check("bad_header_fixture_written",write(bad,"NOT_A_SESSION"));
 check("bad_header_rejected",BookingSessionStore::load(bad).state=="invalid");check("bad_header_removed",!QFileInfo::exists(bad));
 const QString shortFile=temp.filePath("short.dpapi");check("short_fixture_written",write(shortFile,"x"));check("short_file_not_saved_session",!BookingSessionStore::hasSavedSessionAt(shortFile));
 check("short_file_rejected",BookingSessionStore::load(shortFile).state=="invalid");check("short_file_removed",!QFileInfo::exists(shortFile));
 check("unrelated_valid_file_preserved",bytes(path)==good&&BookingSessionStore::load(path).state=="loaded");
 check("explicit_session_removal",BookingSessionStore::remove(path)&&!QFileInfo::exists(path));check("removed_session_missing",BookingSessionStore::load(path).state=="missing");
 check("remove_missing_is_idempotent",BookingSessionStore::remove(path));
#else
 check("non_windows_save_fails_closed",!BookingSessionStore::save(input,path));
#endif
 check("temporary_paths_only",path.startsWith(temp.path()+"/")||path.startsWith(temp.path()+"\\"));
 const QJsonObject result{{"checks",checks},{"passed",checks.size()-failed},{"failed",failed},{"website_requests",0},{"real_account_cache_accesses",0},{"scope","Public session-store API, synthetic cookie values, explicit temporary paths only. Windows CurrentUser DPAPI roundtrip and invalid-input rejection. No application default cache is inspected."}};
 const auto json=QJsonDocument(result).toJson(QJsonDocument::Indented);fwrite(json.constData(),1,size_t(json.size()),stdout);return failed?1:0;
}
