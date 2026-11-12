#include "native_service.h"
#include "legacy_departments.h"
#include <QCryptographicHash>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QMutex>
#include <QMutexLocker>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QUuid>
#include <cmath>
#include <cctype>
#include <limits>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#endif

#define S(x) QStringLiteral(x)
namespace native {
namespace {
constexpr qint64 CACHE_MAX=1000000, RESPONSE_MAX=2097152, TTL=86400;
QJsonArray validateProjection(const QJsonValue &value);
// Keep content-based invalidation, including same-size/same-mtime rewrites.
// Windows CNG uses the platform SHA-256 implementation; Qt remains the fallback.
QByteArray contentSha256(const QByteArray &bytes,bool nativeEnabled=true){
#ifdef Q_OS_WIN
 struct CngSha256 {
  using Open=NTSTATUS (WINAPI *)(BCRYPT_ALG_HANDLE *,LPCWSTR,LPCWSTR,ULONG);
  using Hash=NTSTATUS (WINAPI *)(BCRYPT_ALG_HANDLE,PUCHAR,ULONG,PUCHAR,ULONG,PUCHAR,ULONG);
  using Close=NTSTATUS (WINAPI *)(BCRYPT_ALG_HANDLE,ULONG);
  HMODULE module=nullptr;BCRYPT_ALG_HANDLE algorithm=nullptr;Hash hash=nullptr;Close close=nullptr;
  CngSha256(){
   module=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)return;
   const auto open=reinterpret_cast<Open>(GetProcAddress(module,"BCryptOpenAlgorithmProvider"));
   hash=reinterpret_cast<Hash>(GetProcAddress(module,"BCryptHash"));close=reinterpret_cast<Close>(GetProcAddress(module,"BCryptCloseAlgorithmProvider"));
   if(!open||!hash||!close||open(&algorithm,L"SHA256",nullptr,0)<0){if(algorithm&&close)close(algorithm,0);algorithm=nullptr;hash=nullptr;}
  }
  ~CngSha256(){if(algorithm&&close)close(algorithm,0);if(module)FreeLibrary(module);}
 };
 static const CngSha256 cng;
 if(nativeEnabled&&cng.algorithm&&cng.hash){
  QByteArray digest(32,Qt::Uninitialized);unsigned char empty=0;
  auto input=bytes.isEmpty()?&empty:reinterpret_cast<unsigned char *>(const_cast<char *>(bytes.constData()));
  if(cng.hash(cng.algorithm,nullptr,0,input,ULONG(bytes.size()),reinterpret_cast<unsigned char *>(digest.data()),ULONG(digest.size()))>=0)return digest;
 }
#else
 Q_UNUSED(nativeEnabled);
#endif
 return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
// Read through EOF, or MAX+1 bytes to let callers reject oversized files.
// Reserve the observed size only; never allocate the 128MB ceiling for a 2MB file.
QByteArray readBoundedContent(QIODevice &device,qint64 maximum){
 if(maximum<0||maximum>=std::numeric_limits<int>::max())throw Failure(S("metadata_content_read_limit_invalid"));
 QByteArray bytes;const qint64 ceiling=maximum+1;bytes.reserve(int(qMin(qMax(qint64(0),device.size()),ceiling)));
 while(bytes.size()<ceiling){const int offset=bytes.size(),amount=int(qMin(qint64(65536),ceiling-offset));bytes.resize(offset+amount);const qint64 count=device.read(bytes.data()+offset,amount);if(count<0)throw Failure(S("metadata_content_read_failed"));bytes.resize(offset+int(count));if(!count)break;}
 return bytes;
}
QString utcNow(){return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
void checkCancel(std::atomic_bool *p){if(p&&p->load(std::memory_order_relaxed))throw Failure(S("cancelled_by_user"));}
// Preserve the ASCII identifier grammar without compiling one regex per hospital.
bool codeValid(const QString &v,int length=160){if(v.isEmpty()||v.size()>length)return false;for(const QChar c:v){const ushort u=c.unicode();if(!((u>=65&&u<=90)||(u>=97&&u<=122)||(u>=48&&u<=57)||u==95||u==45))return false;}return true;}
bool departmentCodeValid(const QString &value){if(value.isEmpty()||value.size()>160)return false;for(const QChar c:value)if(c.unicode()<32)return false;return true;}
QString scalarString(const QJsonValue &v){if(v.isString())return v.toString();if(v.isDouble())return QString::number(v.toDouble(),'g',16);return QString();}
bool publicText(const QJsonValue &v){if(!v.isString()||v.toString().isEmpty()||v.toString().size()>300)return false;for(const QChar c:v.toString())if(c.unicode()<32)return false;return true;}
// Equal counts plus membership prove exact schema equality (object keys are unique).
bool exactKeys(const QJsonObject &o,const QStringList &keys){if(o.size()!=keys.size())return false;for(auto it=o.constBegin();it!=o.constEnd();++it)if(!keys.contains(it.key()))return false;return true;}
bool reparse(const QString &path){
 QFileInfo info(path);if(info.isSymLink())return true;
#ifdef Q_OS_WIN
 DWORD a=GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
 return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT);
#else
 return false;
#endif
}
QString cachePath(const QString &root,const QString &code,bool create=false){
 QString base=QDir(root).absolutePath();const QStringList parts={S("output"),S("hospital_availability_http_fast_gui"),S("department_cache")};QString current=base;
 for(const QString &part:parts){current=QDir(current).filePath(part);if(reparse(current))throw Failure(S("metadata_cache_path_rejected"));QFileInfo info(current);if(info.exists()&&!info.isDir())throw Failure(S("metadata_cache_path_rejected"));}
 if(create&&!QDir().mkpath(current))throw Failure(S("metadata_cache_write_unavailable"));
 const QString path=QDir(current).filePath(QString::fromLatin1(QCryptographicHash::hash(code.toUtf8(),QCryptographicHash::Sha256).toHex())+S(".json"));
 if(reparse(path))throw Failure(S("metadata_cache_path_rejected"));return path;
}
QDateTime parseUtc(const QJsonValue &value){
 static const QRegularExpression utcSuffix(S("(?:Z|\\+00:00)$"));
 if(!value.isString()||value.toString().size()>40||!utcSuffix.match(value.toString()).hasMatch())throw Failure(S("metadata_cache_timestamp_invalid"));
 auto d=QDateTime::fromString(value.toString(),Qt::ISODateWithMs);if(!d.isValid())d=QDateTime::fromString(value.toString(),Qt::ISODate);
 if(!d.isValid()||d.offsetFromUtc()!=0)throw Failure(S("metadata_cache_timestamp_invalid"));return d.toUTC();
}
QJsonObject validateCache(const QJsonObject &v,const QString &code){
 if(!exactKeys(v,{S("schema_version"),S("hospital_code"),S("hospital_name"),S("fetched_at"),S("cached_at"),S("departments")}))throw Failure(S("metadata_cache_schema_invalid"));
 if(!v.value(S("schema_version")).isDouble()||v.value(S("schema_version")).toDouble()!=1||v.value(S("hospital_code"))!=QJsonValue(code))throw Failure(S("metadata_cache_identity_invalid"));
 if(!publicText(v.value(S("hospital_name"))))throw Failure(S("metadata_cache_name_invalid"));
 const auto fetched=parseUtc(v.value(S("fetched_at"))),cached=parseUtc(v.value(S("cached_at"))),now=QDateTime::currentDateTimeUtc();const qint64 age=fetched.msecsTo(now);
 if(age<0||age>=TTL*1000)throw Failure(S("metadata_cache_expired_or_future"));if(cached<fetched||cached>now)throw Failure(S("metadata_cache_timestamp_invalid"));
 validateProjection(v.value(S("departments")));return v;
}
QString decodeUtf8(const QByteArray &bytes){QTextCodec::ConverterState state;QString text=QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(),bytes.size(),&state);if(state.invalidChars)throw Failure(S("metadata_cache_utf8_invalid"));return text;}
void lexicalCacheKeys(const QByteArray &raw){
 // Qt represents JSON numbers as doubles. Preserve the prior cache contract's
 // integer schema token and reject duplicate object keys before Qt normalizes
 // them. This scanner only examines already-valid JSON and never executes it.
 QVector<QSet<QString>> objects;QVector<char> stack;int depth=0;bool schema=false;
 for(int i=0;i<raw.size();++i){char c=raw.at(i);if(c=='"'){int start=i++;bool escaped=false;for(;i<raw.size();++i){char q=raw.at(i);if(escaped){escaped=false;continue;}if(q=='\\'){escaped=true;continue;}if(q=='"')break;}const int end=i;int next=i+1;while(next<raw.size()&&std::isspace(static_cast<unsigned char>(raw.at(next))))++next;
   if(next<raw.size()&&raw.at(next)==':'&&!stack.isEmpty()&&stack.last()=='{'){
    const auto token=raw.mid(start,end-start+1);const QString key=QJsonDocument::fromJson(QByteArray("[")+token+QByteArray("]")).array().at(0).toString();if(objects.last().contains(key))throw Failure(S("metadata_cache_duplicate_key"));objects.last().insert(key);
    if(depth==1&&key==S("schema_version")){int n=next+1;while(n<raw.size()&&std::isspace(static_cast<unsigned char>(raw.at(n))))++n;int endNumber=n;while(endNumber<raw.size()&&raw.at(endNumber)!=','&&raw.at(endNumber)!='}')++endNumber;if(raw.mid(n,endNumber-n).trimmed()!=QByteArray("1"))throw Failure(S("metadata_cache_identity_invalid"));schema=true;}
   }
  }else if(c=='{'||c=='['){++depth;stack.append(c);if(c=='{')objects.append({});}else if(c=='}'||c==']'){--depth;if(c=='}')objects.removeLast();stack.removeLast();}
 }if(!schema)throw Failure(S("metadata_cache_identity_invalid"));
}
QJsonObject readCache(const QString &root,const QString &code){
 const QString path=cachePath(root,code);QFile f(path);if(!f.exists())return {};if(!QFileInfo(path).isFile()||f.size()>CACHE_MAX||!f.open(QIODevice::ReadOnly))throw Failure(S("metadata_cache_ignored"));QByteArray raw=f.read(CACHE_MAX+1);if(raw.size()>CACHE_MAX)throw Failure(S("metadata_cache_oversize"));decodeUtf8(raw);
 QJsonParseError e;auto doc=QJsonDocument::fromJson(raw,&e);if(e.error!=QJsonParseError::NoError||!doc.isObject())throw Failure(S("metadata_cache_schema_invalid"));lexicalCacheKeys(raw);return validateCache(doc.object(),code);
}
QList<QStringList> csvRows(const QString &s){
 QList<QStringList> rows;QStringList row;QString field;bool quoted=false,afterQuote=false;
 for(int i=0;i<s.size();++i){QChar c=s.at(i);if(quoted){if(c==QLatin1Char('"')){if(i+1<s.size()&&s.at(i+1)==QLatin1Char('"')){field+=c;++i;}else{quoted=false;afterQuote=true;}}else field+=c;continue;}
  if(c==QLatin1Char('"')&&field.isEmpty()&&!afterQuote){quoted=true;continue;}if(c==QLatin1Char(',')){row<<field;field.clear();afterQuote=false;}else if(c==QLatin1Char('\n')||c==QLatin1Char('\r')){if(c==QLatin1Char('\r')&&i+1<s.size()&&s.at(i+1)==QLatin1Char('\n'))++i;row<<field;rows<<row;row.clear();field.clear();afterQuote=false;}else{if(afterQuote&&!c.isSpace())throw Failure(S("hospital_csv_invalid"));if(!afterQuote)field+=c;}}
 if(quoted)throw Failure(S("hospital_csv_invalid"));if(!field.isEmpty()||!row.isEmpty()){row<<field;rows<<row;}return rows;
}
struct HospitalRecord{QString code,name;};
struct HospitalCatalog{QByteArray digest;QVector<HospitalRecord> hospitals;};
HospitalCatalog parseHospitalCatalog(const QByteArray &raw){
 QString text=decodeUtf8(raw);if(text.startsWith(QChar(0xfeff)))text.remove(0,1);
 const auto rows=csvRows(text);if(rows.size()<2)throw Failure(S("hospital_csv_invalid"));
 const int codeIndex=rows[0].indexOf(S("hospital_code")),nameIndex=rows[0].indexOf(S("hospital_name"));
 if(codeIndex<0||nameIndex<0)throw Failure(S("hospital_csv_invalid"));
 HospitalCatalog result;result.digest=contentSha256(raw).toHex();QSet<QString> seen;
 for(int i=1;i<rows.size();++i){if(rows[i].size()<=qMax(codeIndex,nameIndex))throw Failure(S("hospital_csv_invalid"));const QString code=rows[i][codeIndex].trimmed(),name=rows[i][nameIndex].trimmed();if(!codeValid(code,80)||!publicText(QJsonValue(name))||seen.contains(code))throw Failure(S("hospital_csv_identity_invalid"));seen.insert(code);result.hospitals.append({code,name});}
 if(result.hospitals.isEmpty()||result.hospitals.size()>10000)throw Failure(S("hospital_csv_count_invalid"));return result;
}
HospitalCatalog readHospitalCatalog(const QString &root){
 QFile file(QDir(root).absoluteFilePath(S("data/hospitals.csv")));if(!file.open(QIODevice::ReadOnly)||file.size()>20000000)throw Failure(S("hospital_csv_unavailable"));const QByteArray raw=file.read(20000001);file.close();if(raw.size()>20000000)throw Failure(S("hospital_csv_unavailable"));return parseHospitalCatalog(raw);
}
QJsonArray validateProjection(const QJsonValue &value){
 if(!value.isArray())throw Failure(S("metadata_cache_departments_invalid"));const auto deps=value.toArray();if(deps.size()>5000)throw Failure(S("metadata_cache_departments_invalid"));QSet<QString> seen;
  static const QStringList keys{S("code"),S("name"),S("first_department_code"),S("first_department_name")};
  for(const auto &x:deps){if(!x.isObject())throw Failure(S("metadata_cache_projection_invalid"));const auto d=x.toObject();if(!exactKeys(d,keys))throw Failure(S("metadata_cache_projection_invalid"));
  if(!d.value(S("code")).isString()||!d.value(S("first_department_code")).isString()||!departmentCodeValid(d.value(S("code")).toString())||!departmentCodeValid(d.value(S("first_department_code")).toString()))throw Failure(S("metadata_cache_department_code_invalid"));
  if(!publicText(d.value(S("name")))||!publicText(d.value(S("first_department_name"))))throw Failure(S("metadata_cache_department_name_invalid"));
  const QString key=d.value(S("first_department_code")).toString()+QChar(0x1f)+d.value(S("code")).toString();if(seen.contains(key))throw Failure(S("metadata_cache_duplicate_department"));seen.insert(key);
 }return deps;
}
QString snapshotPath(const QString &root){return QDir(root).absoluteFilePath(S("data/hospital_departments_snapshot.json"));}
QJsonObject snapshotProjection(const QHash<QString,QJsonObject> &entries,const QString &code){
 if(!entries.contains(code))throw Failure(S("department_snapshot_hospital_missing"));
 const auto entry=entries.value(code);const QString stamp=entry.value(S("fetched_at")).toString();return {{S("schema_version"),1},{S("hospital_code"),code},{S("hospital_name"),entry.value(S("hospital_name"))},{S("fetched_at"),stamp},{S("cached_at"),stamp},{S("departments"),entry.value(S("departments"))}};
}
QJsonObject readSnapshotEntry(const QString &root,const QString &code){
 const QString path=snapshotPath(root);QFileInfo info(path);if(!info.exists())return {};if(!info.isFile()||reparse(path)||info.size()>128*1024*1024)throw Failure(S("department_snapshot_invalid"));
 QFile file(path);if(!file.open(QIODevice::ReadOnly))throw Failure(S("department_snapshot_unavailable"));const QByteArray raw=readBoundedContent(file,128*1024*1024);file.close();if(raw.size()>128*1024*1024)throw Failure(S("department_snapshot_invalid"));
 const QString csvPath=QDir(root).absoluteFilePath(S("data/hospitals.csv"));QFile csv(csvPath);if(!csv.open(QIODevice::ReadOnly)||csv.size()>20000000)throw Failure(S("hospital_csv_unavailable"));const QByteArray csvRaw=readBoundedContent(csv,20000000);csv.close();if(csvRaw.size()>20000000)throw Failure(S("hospital_csv_unavailable"));
 const QByteArray snapshotDigest=contentSha256(raw),csvDigest=contentSha256(csvRaw);
 struct ValidatedSnapshot{QByteArray snapshotDigest,csvDigest;QHash<QString,QJsonObject> entries;};
 static QHash<QString,ValidatedSnapshot> memo;static QMutex memoMutex;
 // Read and hash both files on every lookup: same-size/same-mtime replacements invalidate too.
 {QMutexLocker lock(&memoMutex);const auto hit=memo.constFind(path);if(hit!=memo.constEnd()&&hit->snapshotDigest==snapshotDigest&&hit->csvDigest==csvDigest)return snapshotProjection(hit->entries,code);}
 decodeUtf8(raw);QJsonParseError error;const auto doc=QJsonDocument::fromJson(raw,&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())throw Failure(S("department_snapshot_invalid"));
 const auto snapshot=doc.object();if(!exactKeys(snapshot,{S("schema_version"),S("complete"),S("source_hospital_csv_sha256"),S("fetched_at"),S("hospital_count"),S("hospitals")})||!snapshot.value(S("schema_version")).isDouble()||snapshot.value(S("schema_version")).toInt()!=1||snapshot.value(S("complete"))!=QJsonValue(true)||!snapshot.value(S("hospitals")).isArray())throw Failure(S("department_snapshot_incomplete"));
 const auto catalog=parseHospitalCatalog(csvRaw);const QString csvHash=QString::fromLatin1(catalog.digest);if(snapshot.value(S("source_hospital_csv_sha256")).toString()!=csvHash||snapshot.value(S("hospital_count")).toInt(-1)!=catalog.hospitals.size()||snapshot.value(S("hospitals")).toArray().size()!=catalog.hospitals.size())throw Failure(S("department_snapshot_catalog_mismatch"));
 const auto published=parseUtc(snapshot.value(S("fetched_at")));if(published>QDateTime::currentDateTimeUtc().addSecs(60))throw Failure(S("department_snapshot_timestamp_invalid"));
 QHash<QString,QJsonObject> entries;for(const auto &item:snapshot.value(S("hospitals")).toArray()){
  if(!item.isObject())throw Failure(S("department_snapshot_entry_invalid"));const auto entry=item.toObject();if(!exactKeys(entry,{S("hospital_code"),S("hospital_name"),S("fetched_at"),S("department_count"),S("departments_sha256"),S("departments")}))throw Failure(S("department_snapshot_entry_invalid"));
  const QString hcode=entry.value(S("hospital_code")).toString();if(!codeValid(hcode,80)||entries.contains(hcode)||!publicText(entry.value(S("hospital_name"))))throw Failure(S("department_snapshot_identity_invalid"));
  const auto dept=validateProjection(entry.value(S("departments")));if(entry.value(S("department_count")).toInt(-1)!=dept.size())throw Failure(S("department_snapshot_count_invalid"));
  const auto fetched=parseUtc(entry.value(S("fetched_at")));if(fetched>QDateTime::currentDateTimeUtc().addSecs(60))throw Failure(S("department_snapshot_timestamp_invalid"));
  const QByteArray projection=QJsonDocument(dept).toJson(QJsonDocument::Compact);const QString digest=QString::fromLatin1(contentSha256(projection).toHex());if(entry.value(S("departments_sha256")).toString()!=digest)throw Failure(S("department_snapshot_hash_invalid"));
  entries.insert(hcode,entry);
 }
 for(const auto &hospital:catalog.hospitals){if(!entries.contains(hospital.code)||entries.value(hospital.code).value(S("hospital_name")).toString()!=hospital.name)throw Failure(S("department_snapshot_coverage_invalid"));}
 if(entries.size()!=catalog.hospitals.size())throw Failure(S("department_snapshot_coverage_invalid"));
 // Cache only fully validated public metadata; availability rows are never cached.
 if(raw.size()<=16*1024*1024){QMutexLocker lock(&memoMutex);if(memo.size()>=4&&!memo.contains(path))memo.clear();memo[path]={snapshotDigest,csvDigest,entries};}
 return snapshotProjection(entries,code);
}
bool saveAtomicJson(const QString &path,const QJsonObject &value){
 const QString dir=QFileInfo(path).absolutePath();if(reparse(dir)||!QDir().mkpath(dir)||reparse(path))return false;QSaveFile file(path);file.setDirectWriteFallback(false);const auto raw=QJsonDocument(value).toJson(QJsonDocument::Compact);return file.open(QIODevice::WriteOnly)&&file.write(raw)==raw.size()&&file.commit();
}
QString hospitalName(const QString &root,const QString &code){
 struct Names{QByteArray digest;QHash<QString,QString> values;};static QHash<QString,Names> memo;static QMutex mutex;
 const QString path=QDir(root).absoluteFilePath(S("data/hospitals.csv"));QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>20000000)return code;const QByteArray bytes=readBoundedContent(f,20000000);f.close();if(bytes.size()>20000000)return code;const QByteArray digest=contentSha256(bytes);
 QMutexLocker lock(&mutex);auto hit=memo.constFind(path);if(hit!=memo.constEnd()&&hit->digest==digest)return hit->values.value(code,code);
 try{QString text=decodeUtf8(bytes);if(text.startsWith(QChar(0xfeff)))text.remove(0,1);auto rows=csvRows(text);if(rows.isEmpty())return code;int ci=rows[0].indexOf(S("hospital_code")),ni=rows[0].indexOf(S("hospital_name"));if(ci<0||ni<0)return code;Names names;names.digest=digest;QSet<QString> seen,duplicates;for(int i=1;i<rows.size();++i)if(rows[i].size()>qMax(ci,ni)){const QString c=rows[i][ci].trimmed(),name=rows[i][ni].trimmed();if(seen.contains(c))duplicates.insert(c);seen.insert(c);if(publicText(QJsonValue(name)))names.values[c]=name;}for(const QString &c:duplicates)names.values.remove(c);if(memo.size()>=32)memo.clear();memo[path]=names;return names.values.value(code,code);}catch(const Failure&){}return code;
}
QJsonObject cacheReport(const QString &root,const QString &code,QJsonObject v,const QString &source){
 v[S("hospital_name")]=hospitalName(root,code);const bool persistent=source==S("snapshot");return {{S("status"),S("complete")},{S("exit_status"),0},{S("business_success"),true},{S("result_kind"),S("departments")},{S("hospital_code"),code},{S("hospital_name"),v.value(S("hospital_name"))},{S("departments"),v.value(S("departments"))},{S("rows"),QJsonArray()},{S("queried_at"),utcNow()},{S("metadata_queried_at"),v.value(S("fetched_at"))},{S("metadata_cached_at"),v.value(S("cached_at"))},{S("metadata_expires_at"),persistent?QJsonValue(QJsonValue::Null):QJsonValue(parseUtc(v.value(S("fetched_at"))).addSecs(TTL).toString(Qt::ISODateWithMs))},{S("metadata_ttl_seconds"),persistent?QJsonValue(QJsonValue::Null):QJsonValue(TTL)},{S("metadata_persistent"),persistent},{S("metadata_cache_source"),source},{S("mode"),S("pure_http")},{S("browser_engine_used"),false},{S("cookie_values_saved"),false},{S("performance"),QJsonObject{{S("metadata_cache_hit"),true},{S("metadata_cache_source"),source},{S("metadata_source"),source},{S("session_reused"),false},{S("http_dispatches"),0}}}};
}
QJsonValue remaining(const QJsonValue &v){if(v.isBool()||v.isNull()||v.isUndefined())return QJsonValue(QJsonValue::Null);const QString t=scalarString(v).trimmed();if(!QRegularExpression(S("^[0-9]+$")).match(t).hasMatch())return QJsonValue(QJsonValue::Null);bool ok=false;const double n=t.toDouble(&ok);return ok&&std::isfinite(n)?QJsonValue(n):QJsonValue(QJsonValue::Null);}
QString statusText(const QJsonObject &d){const auto v=d.value(S("productStatus"));int n=v.isDouble()?v.toInt(-1):-1;switch(n){case 1:return S("有号");case 8:return S("候补");case 9:return S("候补已满");case 2:return S("约满");case 4:return S("无号");default:return d.value(S("productStatusView")).toString(S("未知"));}}
QJsonArray calendarRows(const QJsonObject &data,const QJsonObject &dept,const QString &stamp){
 if(!data.value(S("calendars")).isArray())throw Failure(S("calendar_schema_invalid"));QJsonArray rows;
 for(const auto &value:data.value(S("calendars")).toArray()){if(!value.isObject())throw Failure(S("calendar_date_invalid"));auto e=value.toObject();const QString date=scalarString(e.value(S("dutyDate")));if(!QRegularExpression(S("^[0-9]{8}$")).match(date).hasMatch())throw Failure(S("calendar_date_invalid"));rows.append(QJsonObject{{S("department_code"),dept.value(S("code"))},{S("department_name"),dept.value(S("name"))},{S("date"),date},{S("doctor_name"),S("")},{S("period"),S("")},{S("remaining"),QJsonValue(QJsonValue::Null)},{S("status"),e.value(S("statusView")).toString(S("未知"))},{S("raw_status"),e.value(S("status"))},{S("row_kind"),S("department_calendar")},{S("queried_at"),stamp}});}return rows;
}
}

Service::Service(QString root):root_(QDir(root).absolutePath()),owner_(QThread::currentThread()){}
Service::~Service(){try{if(broker_)broker_->close();}catch(...) {}}
void Service::close(){if(QThread::currentThread()!=owner_)throw Failure(S("native_service_owner_thread_required"));if(busy_)throw Failure(S("native_service_close_during_run"));if(broker_)broker_->close();broker_.reset();metadata_.clear();closed_=true;}
void Service::prepare(int required,bool allowBootstrap){
 checkCancel(cancel_);reused_=false;renewReason_.clear();const double limit=qMin(125.0,180.0-required*25.0-5.0);
 // Manual prepared queries retain their broker when the next request has no local budget.
 if(!allowBootstrap){if(!broker_||!broker_->alive())throw Failure(S("manual_connection_required"));if(born_.elapsed()/1000.0>=limit)throw Failure(S("runtime_age_budget"));if(broker_->apiUsed()+required>8||broker_->dispatches()+required>12)throw Failure(S("public_session_budget_exhausted"));reused_=true;broker_->setCancel(cancel_);broker_->selectHospital(currentCode_);return;}
 if(broker_){if(!broker_->alive())renewReason_=S("runtime_not_alive");else if(born_.elapsed()/1000.0>=limit)renewReason_=S("runtime_age_budget");else if(broker_->apiUsed()+required>8)renewReason_=S("runtime_api_budget");if(!renewReason_.isEmpty()){broker_->close();broker_.reset();}}
 if(!broker_){if(!allowBootstrap)throw Failure(S("manual_connection_required"));broker_=std::make_unique<Broker>(root_,cancel_);born_.start();++session_;}else reused_=true;
 broker_->setCancel(cancel_);broker_->selectHospital(currentCode_);
}
QJsonObject Service::queryJson(const QString &method,const QString &path,const QJsonObject &body,bool requirePrepared){
 checkCancel(cancel_);QElapsedTimer clock;clock.start();QJsonObject summary{{S("method"),method},{S("path"),path},{S("queried_at"),utcNow()}};const int before=broker_->dispatches();
 try{const auto response=broker_->request(method,path,body,requirePrepared);checkCancel(cancel_);summary[S("http_status")]=response.status;summary[S("response_bytes")]=response.body.size();summary[S("response_sha256")]=QString::fromLatin1(QCryptographicHash::hash(response.body,QCryptographicHash::Sha256).toHex());if(response.body.size()>RESPONSE_MAX)throw Failure(S("response_oversize"));if(response.status!=200)throw Failure(S("http_%1").arg(response.status));QJsonParseError e;const auto doc=QJsonDocument::fromJson(response.body,&e);if(e.error!=QJsonParseError::NoError||!doc.isObject())throw Failure(S("response_not_json"));const auto payload=doc.object();const auto businessCode=payload.value(S("code"));summary[S("business_code")]=(businessCode.isString()||businessCode.isDouble()||businessCode.isBool()||businessCode.isNull())?businessCode:QJsonValue(QJsonValue::Null);const auto businessMessage=payload.value(S("msg")).isString()?payload.value(S("msg")):payload.value(S("message"));if(businessMessage.isString())summary[S("business_message")]=businessMessage.toString().left(160);if(path.endsWith(S("/doctor/detail"))){const auto data=payload.value(S("data")).toObject();const auto dutyDate=data.value(S("dutyDate"));summary[S("doctor_response_date")]=(dutyDate.isString()||dutyDate.isDouble())?dutyDate:QJsonValue(QJsonValue::Null);summary[S("doctor_response_date_present")]=!dutyDate.isUndefined();summary[S("doctor_count")]=data.value(S("doctors")).isArray()?QJsonValue(data.value(S("doctors")).toArray().size()):QJsonValue(QJsonValue::Null);}// 1521 is a documented date refusal, not a transport failure. Only the caller
 // for doctor/detail may resolve it using a fresh successful official calendar.
 if(businessCode!=QJsonValue(S("0000"))&&!(path==S("/jtjk/mobile-service/product/doctor/detail")&&businessCode==QJsonValue(S("1521"))))throw Failure(S("business_response_rejected"));summary[S("elapsed_seconds")]=clock.elapsed()/1000.0;summary[S("http_dispatches")]=broker_->dispatches()-before;responses_.append(summary);return payload;}
 catch(...){summary[S("elapsed_seconds")]=clock.elapsed()/1000.0;summary[S("http_dispatches")]=broker_->dispatches()-before;responses_.append(summary);throw;}
}
QJsonObject Service::queryLegacyJson(const QString &method,const QString &path,const QJsonObject &body){
 checkCancel(cancel_);QElapsedTimer clock;clock.start();QJsonObject summary{{S("method"),method},{S("path"),path},{S("source_system"),S("legacy")},{S("queried_at"),utcNow()}};const int before=broker_->dispatches();
 try{
  const auto response=broker_->request(method,path,body,false);checkCancel(cancel_);summary[S("http_status")]=response.status;summary[S("response_bytes")]=response.body.size();summary[S("response_sha256")]=QString::fromLatin1(QCryptographicHash::hash(response.body,QCryptographicHash::Sha256).toHex());
  if(response.body.size()>RESPONSE_MAX)throw Failure(S("legacy_response_oversize"));if(response.status!=200)throw Failure(S("legacy_http_%1").arg(response.status));
  QJsonParseError error;const auto document=QJsonDocument::fromJson(response.body,&error);if(error.error!=QJsonParseError::NoError||!document.isObject())throw Failure(S("legacy_response_not_json"));
  const auto envelope=document.object();summary[S("business_code")]=envelope.value(S("resCode"));QJsonValue data;QJsonObject rejected;if(!legacy::unwrap(envelope,data,rejected))throw Failure(rejected.value(S("failure")).toString());
  summary[S("elapsed_seconds")]=clock.elapsed()/1000.0;summary[S("http_dispatches")]=broker_->dispatches()-before;responses_.append(summary);return envelope;
 }catch(...){summary[S("elapsed_seconds")]=clock.elapsed()/1000.0;summary[S("http_dispatches")]=broker_->dispatches()-before;responses_.append(summary);throw;}
}
QJsonArray Service::legacyAvailability(const QString &code,const QJsonObject &department,const QString &date,QJsonObject &report){
 report[S("hospital_system")]=S("legacy");report[S("booking_supported")]=false;report[S("read_only")]=true;report[S("message")]=S("正在查询114旧版官方排班；未取得成功响应前不会推断无号或约满。");
 try{
  broker_->selectLegacyHospital(code);born_.restart();const auto mapping=legacy::parseDepartments(queryLegacyJson(S("GET"),S("/mobile/department/hos/list")),code);
  if(!mapping.value(S("business_success")).toBool())throw Failure(mapping.value(S("failure")).toString());QJsonObject selected;int matches=0;
  for(const auto &value:mapping.value(S("departments")).toArray()){const auto item=value.toObject();if(item.value(S("code"))==department.value(S("code"))&&item.value(S("first_department_code"))==department.value(S("first_department_code"))){selected=item;++matches;}}
  if(matches!=1)throw Failure(S("legacy_department_code_missing_or_ambiguous"));
  const legacy::Context context{code,selected.value(S("first_department_code")).toString(),selected.value(S("code")).toString(),selected.value(S("name")).toString(),utcNow()};
  const QJsonObject body{{S("hosCode"),code},{S("firstDeptCode"),context.firstDepartmentCode},{S("secondDeptCode"),context.departmentCode}};
  const auto calendar=legacy::parseCalendar(queryLegacyJson(S("POST"),S("/mobile/product/list"),body),context);if(!calendar.value(S("business_success")).toBool())throw Failure(calendar.value(S("failure")).toString());
  const auto all=calendar.value(S("rows")).toArray();QJsonObject day;matches=0;for(const auto &value:all){const auto item=value.toObject();if(item.value(S("date"))==QJsonValue(date)){day=item;++matches;}}if(matches!=1)throw Failure(S("legacy_selected_date_missing_or_ambiguous"));QJsonArray rows;
  if(day.value(S("doctor_list_visible")).toBool()){
   auto request=body;request[S("target")]=day.value(S("duty_date"));const auto detail=legacy::parseDetail(queryLegacyJson(S("POST"),S("/mobile/product/detail"),request),context,day.value(S("duty_date")).toString());
   if(!detail.value(S("business_success")).toBool())throw Failure(detail.value(S("failure")).toString());rows=detail.value(S("rows")).toArray();
  }
  report[S("doctor_count")]=rows.size();if(rows.isEmpty())rows.append(day);report[S("calendar_rows")]=all;report[S("legacy_calendar_dates")]=all.size();report[S("message")]=S("已读取114旧版官方排班。旧版号源仅供查号，余量未返回可验证数字时显示未知。");return rows;
 }catch(const std::exception &e){const QString failure=QString::fromUtf8(e.what());report[S("message")]=failure==S("legacy_http_464")?S("114旧版号源接口返回HTTP 464，本次未取得排班。请使用北京114公众号／小程序核对；不能据此判断无号或约满。"):S("114旧版查号未成功（%1），本次未取得可信排班，请使用北京114公众号／小程序核对。").arg(failure);throw;}
}
QJsonArray Service::flattenDepartments(const QJsonValue &data){
 if(!data.isArray())throw Failure(S("department_schema_invalid"));QJsonArray result;QSet<QString> seen;int nodes=0;
 std::function<void(QJsonValue,QJsonObject,int)> visit=[&](QJsonValue value,QJsonObject first,int depth){if(++nodes>20000||depth>64)throw Failure(S("department_schema_limit"));if(!value.isObject())return;auto item=value.toObject();const QString code=scalarString(item.value(S("code")));if(code.isEmpty())return;if(first.isEmpty())first=item;const auto children=item.value(S("subList"));if(!children.isUndefined()&&!children.isNull()&&!children.isArray())throw Failure(S("department_schema_invalid"));if(children.isArray()&&!children.toArray().isEmpty()){for(const auto &child:children.toArray())visit(child,first,depth+1);return;}
  const QString key=scalarString(first.value(S("code")))+QChar(0x1f)+code;if(seen.contains(key))return;seen.insert(key);const QString name=scalarString(item.value(S("name")));QString fn=scalarString(first.value(S("name")));if(fn.isEmpty())fn=scalarString(first.value(S("code")));result.append(QJsonObject{{S("code"),code},{S("name"),name.isEmpty()?code:name},{S("first_department_code"),scalarString(first.value(S("code")))},{S("first_department_name"),fn}});if(result.size()>5000)throw Failure(S("department_schema_limit"));};for(const auto &v:data.toArray())visit(v,{},0);return result;
}
QJsonArray Service::doctorRows(const QJsonObject &data,const QJsonObject &dept,const QString &date){
 if(!data.value(S("doctors")).isArray())throw Failure(S("doctor_schema_invalid"));const auto doctors=data.value(S("doctors")).toArray();const auto responseDate=data.value(S("dutyDate"));const bool missingDate=responseDate.isUndefined()||responseDate.isNull()||(responseDate.isString()&&responseDate.toString().isEmpty());if(doctors.isEmpty()&&missingDate)return {};if(scalarString(responseDate)!=date)throw Failure(S("doctor_response_mismatch"));QJsonArray rows;const QString stamp=utcNow();
 for(const auto &value:data.value(S("doctors")).toArray()){if(!value.isObject())throw Failure(S("doctor_schema_invalid"));const auto doctor=value.toObject();auto details=doctor.value(S("detail"));if(details.isUndefined()||details.isNull())details=QJsonArray();if(!details.isArray())throw Failure(S("doctor_schema_invalid"));for(const auto &dutyValue:details.toArray()){if(!dutyValue.isObject())throw Failure(S("doctor_schema_invalid"));const auto duty=dutyValue.toObject();auto periods=duty.value(S("period"));if(periods.isUndefined()||periods.isNull()||(periods.isArray()&&periods.toArray().isEmpty()))periods=QJsonArray{QJsonValue(QJsonValue::Null)};if(!periods.isArray())throw Failure(S("doctor_schema_invalid"));for(const auto &periodValue:periods.toArray()){if(!periodValue.isNull()&&!periodValue.isObject())throw Failure(S("doctor_schema_invalid"));const auto period=periodValue.toObject();QString title=period.value(S("dutyTimeView")).toString();if(title.isEmpty())title=duty.value(S("dutyCodeView")).toString();rows.append(QJsonObject{{S("booking_payload"),QJsonObject{{S("firstDeptCode"),doctor.value(S("firstDeptCode"))},{S("secondDeptCode"),doctor.value(S("secondDeptCode"))},{S("dutyDate"),date},{S("doctorCode"),doctor.value(S("doctorCode"))},{S("uniqueProductKey"),duty.value(S("uniqueProductKey"))},{S("uniqueProductTimeKey"),period.value(S("uniqProductKey"))},{S("sourcePage"),S("list")}}},{S("product_status"),duty.value(S("productStatus"))},{S("department_code"),dept.value(S("code"))},{S("department_name"),data.value(S("dept2Name")).toString(dept.value(S("name")).toString())},{S("date"),date},{S("doctor_name"),doctor.value(S("doctorName")).toString()},{S("doctor_code"),scalarString(doctor.value(S("doctorCode")))},{S("period"),title},{S("remaining"),remaining(periodValue.isNull()?duty.value(S("ncode")):period.value(S("ncode")))},{S("status"),statusText(duty)},{S("raw_status"),duty.value(S("productStatus"))},{S("row_kind"),S("doctor_period")},{S("queried_at"),stamp}});if(rows.size()>100000)throw Failure(S("doctor_rows_limit"));}}}return rows;
}
QJsonObject Service::cacheOnly(const QString &root,const QString &code){
 if(!codeValid(code,80))return {};try{auto v=readSnapshotEntry(root,code);if(!v.isEmpty())return cacheReport(root,code,v,S("snapshot"));}catch(const Failure&){return {};}
 try{auto v=readCache(root,code);if(v.isEmpty())return {};return cacheReport(root,code,v,S("disk"));}catch(const Failure&){return {};}
}
QJsonArray Service::departments(const QString &code,bool force,bool allowNetwork,bool freshNetwork,QJsonObject &meta){
 try{meta=readSnapshotEntry(root_,code);if(!meta.isEmpty()){meta[S("_source")]=S("snapshot");return meta.value(S("departments")).toArray();}}catch(const Failure &){}
 if(!allowNetwork){
  try{meta=readCache(root_,code);if(!meta.isEmpty()){meta[S("_source")]=S("disk");return meta.value(S("departments")).toArray();}}catch(const Failure &){}
  throw Failure(S("department_snapshot_unavailable"));
 }
 if(freshNetwork){metadata_.remove(code);invalidated_.insert(code);}
 cacheWarning_.clear();if(force){metadata_.remove(code);invalidated_.insert(code);try{const auto path=cachePath(root_,code);if(QFile::exists(path)&&!QFile::remove(path))cacheWarning_=S("metadata_cache_invalidation_unavailable");}catch(const Failure&){cacheWarning_=S("metadata_cache_invalidation_unavailable");}}
 if(!force&&!invalidated_.contains(code)){if(metadata_.contains(code)){try{meta=validateCache(metadata_.value(code),code);meta[S("_source")]=S("memory");return meta.value(S("departments")).toArray();}catch(const Failure&){metadata_.remove(code);}}
  try{meta=readCache(root_,code);if(!meta.isEmpty()){metadata_[code]=meta;meta[S("_source")]=S("disk");return meta.value(S("departments")).toArray();}}catch(const Failure&){cacheWarning_=S("metadata_cache_ignored");}}
 meta={};return {};
}
QJsonObject Service::execute(const QString &operation,const QJsonObject &params,std::atomic_bool *cancel){
 if(QThread::currentThread()!=owner_)throw Failure(S("native_service_owner_thread_required"));if(closed_)throw Failure(S("native_service_closed"));if(busy_)throw Failure(S("native_service_already_active"));busy_=true;cancel_=cancel;responses_={};reused_=false;renewReason_.clear();cacheWarning_.clear();QElapsedTimer elapsed;elapsed.start();const QString code=params.value(S("hospital_code")).toString();currentCode_=code;
 QJsonObject report{{S("status"),S("failed")},{S("exit_status"),2},{S("business_success"),false},{S("result_kind"),operation},{S("hospital_code"),code},{S("queried_at"),utcNow()},{S("rows"),QJsonArray()},{S("departments"),QJsonArray()},{S("mode"),S("pure_http")},{S("browser_engine_used"),false},{S("browser_processes_started"),0},{S("cookie_values_saved"),false},{S("imported_cookies"),false},{S("native_business"),true}};int before=broker_?broker_->dispatches():0,dispatches=0;bool hit=false;double metadataLookupMs=0;QString source;QJsonObject meta;
 try{
  checkCancel(cancel_);if(!codeValid(code,80))throw Failure(S("hospital_code_invalid"));if(operation!=S("prewarm")&&operation!=S("departments")&&operation!=S("availability"))throw Failure(S("unsupported_public_operation"));if(params.contains(S("force_metadata_refresh"))&&!params.value(S("force_metadata_refresh")).isBool())throw Failure(S("metadata_refresh_flag_invalid"));if(params.contains(S("allow_department_network"))&&!params.value(S("allow_department_network")).isBool())throw Failure(S("department_network_flag_invalid"));if(params.contains(S("fresh_department_network"))&&!params.value(S("fresh_department_network")).isBool())throw Failure(S("department_freshness_flag_invalid"));const bool force=params.value(S("force_metadata_refresh")).toBool(),allowDepartmentNetwork=params.value(S("allow_department_network")).toBool(false),freshDepartmentNetwork=params.value(S("fresh_department_network")).toBool(false);if(freshDepartmentNetwork&&!allowDepartmentNetwork)throw Failure(S("fresh_department_network_not_authorized"));const QString department=params.value(S("department_code")).toString(),date=params.value(S("date")).toString(params.value(S("duty_date")).toString());
  if(params.contains(S("renew_session"))){if(!params.value(S("renew_session")).isBool())throw Failure(S("session_renewal_flag_invalid"));if(operation!=S("prewarm"))throw Failure(S("session_renewal_operation_invalid"));}const bool renewSession=params.value(S("renew_session")).toBool(false);
  if(params.contains(S("require_prepared_session"))&&(!params.value(S("require_prepared_session")).isBool()||operation!=S("availability")))throw Failure(S("prepared_session_flag_invalid"));const bool requirePrepared=params.value(S("require_prepared_session")).toBool(false);
  if(operation==S("availability")&&(!departmentCodeValid(department)||!QRegularExpression(S("^[0-9]{8}$")).match(date).hasMatch()||!QDate::fromString(date,S("yyyyMMdd")).isValid()))throw Failure(S("selected_department_and_date_required"));
  if(operation==S("prewarm")){if(renewSession&&broker_){broker_->close();broker_.reset();}prepare(2);if(renewSession)renewReason_=S("requested_session_renewal");before=broker_->dispatches();broker_->prewarm();checkCancel(cancel_);report[S("prewarm_complete")]=true;report[S("status")]=S("ready");}
  else{
   QElapsedTimer metadataClock;metadataClock.start();auto deps=departments(code,force,allowDepartmentNetwork,freshDepartmentNetwork,meta);metadataLookupMs=double(metadataClock.nsecsElapsed())/1e6;hit=!meta.isEmpty();source=meta.take(S("_source")).toString();
   if(!(hit&&operation==S("departments"))){prepare(requirePrepared?1:(hit?0:1)+(operation==S("availability")?2:0),!requirePrepared);before=broker_->dispatches();}
   if(!hit){auto payload=queryJson(S("GET"),S("/jtjk/mobile-service/department/list/")+code,QJsonObject(),requirePrepared);deps=flattenDepartments(payload.value(S("data")));const QString stamp=utcNow();meta={{S("schema_version"),1},{S("hospital_code"),code},{S("hospital_name"),hospitalName(root_,code)},{S("fetched_at"),stamp},{S("cached_at"),stamp},{S("departments"),deps}};validateCache(meta,code);const auto raw=QJsonDocument(meta).toJson(QJsonDocument::Compact);if(raw.size()>CACHE_MAX)throw Failure(S("metadata_projection_oversize"));checkCancel(cancel_);try{const QString path=cachePath(root_,code,true);QSaveFile file(path);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly)||file.write(raw)!=raw.size())throw Failure(S("metadata_cache_write_unavailable"));checkCancel(cancel_);if(cachePath(root_,code)!=path||!file.commit())throw Failure(S("metadata_cache_write_unavailable"));}catch(const Failure &e){if(QString::fromUtf8(e.what())==S("cancelled_by_user"))throw;cacheWarning_=S("metadata_cache_write_unavailable");}metadata_[code]=meta;invalidated_.remove(code);source=S("network");}
   report[S("hospital_name")]=hospitalName(root_,code);report[S("departments")]=deps;report[S("metadata_queried_at")]=meta.value(S("fetched_at"));report[S("metadata_cached_at")]=meta.value(S("cached_at"));report[S("metadata_expires_at")]=source==S("snapshot")?QJsonValue(QJsonValue::Null):QJsonValue(parseUtc(meta.value(S("fetched_at"))).addSecs(TTL).toString(Qt::ISODateWithMs));report[S("metadata_ttl_seconds")]=source==S("snapshot")?QJsonValue(QJsonValue::Null):QJsonValue(TTL);report[S("metadata_persistent")]=source==S("snapshot");report[S("metadata_cache_source")]=source;
   if(operation==S("availability")){
    const QString firstDepartment=params.value(S("first_department_code")).toString();if(!firstDepartment.isEmpty()&&!departmentCodeValid(firstDepartment))throw Failure(S("department_code_invalid"));
    QJsonObject dept;int matches=0;for(const auto &v:deps){const auto item=v.toObject();if(item.value(S("code")).toString()==department&&(firstDepartment.isEmpty()||item.value(S("first_department_code")).toString()==firstDepartment)){dept=item;++matches;}}if(matches!=1)throw Failure(S("department_code_missing_or_ambiguous"));
    const QJsonObject body{{S("hosCode"),code},{S("firstDeptCode"),dept.value(S("first_department_code"))},{S("secondDeptCode"),department}};
    auto doctorBody=body;doctorBody[S("dutyDate")]=date;doctorBody[S("type")]=S("");doctorBody[S("dutyCode")]=S("");
    // The selected-date detail is already live and date-validated. Avoid a preceding calendar round trip.
    const auto doctor=queryJson(S("POST"),S("/jtjk/mobile-service/product/doctor/detail"),doctorBody,requirePrepared);
    const bool dateRefused=doctor.value(S("code"))==QJsonValue(S("1521"));
    QJsonArray rows;QJsonObject data;bool legacyQuery=false;
    if(!dateRefused){if(!doctor.value(S("data")).isObject())throw Failure(S("doctor_schema_invalid"));data=doctor.value(S("data")).toObject();rows=doctorRows(data,dept,date);}
    report[S("doctor_count")]=dateRefused?QJsonValue(QJsonValue::Null):QJsonValue(data.value(S("doctors")).toArray().size());
    // An empty new-system detail cannot establish availability for a legacy hospital.
    if(rows.isEmpty()&&!dateRefused){
     if(!hospitalNewRoute_.contains(code)){
      const auto route=queryJson(S("GET"),S("/jtjk/mobile-service/user/collect/newHospital/")+code,QJsonObject(),requirePrepared);
      if(!route.value(S("data")).isBool())throw Failure(S("hospital_route_schema_invalid"));
      hospitalNewRoute_.insert(code,route.value(S("data")).toBool());
     }
     report[S("hospital_system")]=hospitalNewRoute_.value(code)?S("new"):S("legacy");
     if(!hospitalNewRoute_.value(code)){
      rows=legacyAvailability(code,dept,date,report);legacyQuery=true;before=0;
     }
    }
    const bool calendarChecked=!legacyQuery&&(dateRefused||rows.isEmpty());QJsonValue calendarDates(QJsonValue::Null);
    if(calendarChecked){
     // One public calendar lookup explains an empty or disallowed date. No
     // detail retry, no invented doctors, counts, identifiers or booking payload.
     const auto cal=queryJson(S("POST"),S("/jtjk/mobile-service/product/calendar"),body,requirePrepared);
     if(!cal.value(S("data")).isObject())throw Failure(S("calendar_schema_invalid"));
     const auto all=calendarRows(cal.value(S("data")).toObject(),dept,utcNow());matches=0;QJsonArray availableDates;
     for(const auto &v:all){const auto entry=v.toObject();availableDates.append(entry.value(S("date")));if(entry.value(S("date")).toString()==date){rows.append(v);++matches;}}
     if(matches>1)throw Failure(S("selected_date_missing_or_ambiguous"));
     if(matches==0&&!dateRefused)throw Failure(S("selected_date_missing_or_ambiguous"));
     if(dateRefused){
      QString official=doctor.value(S("message")).toString(doctor.value(S("msg")).toString()).left(160);
      report[S("official_business_code")]=S("1521");report[S("official_message")]=official;
      report[S("available_dates")]=availableDates;report[S("calendar_rows")]=all;
      report[S("selected_date_in_calendar")]=matches==1;
      if(matches==0){
       rows.append(QJsonObject{{S("department_code"),dept.value(S("code"))},{S("department_name"),dept.value(S("name"))},{S("date"),date},{S("doctor_name"),S("")},{S("period"),S("")},{S("remaining"),QJsonValue(QJsonValue::Null)},{S("status"),S("不在官方可查询日期内")},{S("row_kind"),S("date_unavailable")},{S("queried_at"),utcNow()}});
       QStringList dates;for(const auto &d:availableDates)dates.append(QDate::fromString(d.toString(),S("yyyyMMdd")).toString(S("yyyy-MM-dd")));
       report[S("message")]=S("%1 不在该科室官方可查询日期内。官方提示：%2。官方公布日期：%3。请在月历选择其他日期。").arg(QDate::fromString(date,S("yyyyMMdd")).toString(S("yyyy-MM-dd")),official,dates.isEmpty()?S("暂无"):dates.join(S("、")));
      }else report[S("message")]=S("官方提示：%1；已显示该日期的最新官方日历状态。").arg(official);
     }
     calendarDates=all.size();
    }
    for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();if(row.contains(S("booking_payload"))){auto route=row.value(S("booking_payload")).toObject();route[S("hosCode")]=code;row[S("booking_payload")]=route;}rows[i]=row;}report[S("rows")]=rows;report[S("coverage")]=QJsonObject{{S("mode"),S("department_selected_date")},{S("scope"),S("one_department_one_date")},{S("coverage_scope"),S("one_department_one_date")},{S("selected_duty_date"),date},{S("department_total"),1},{S("departments_completed"),1},{S("doctor_dates_completed"),1},{S("calendar_dates"),legacyQuery?report.value(S("legacy_calendar_dates")):calendarDates},{S("calendar_checked"),legacyQuery||calendarChecked},{S("query_strategy"),legacyQuery?S("legacy_calendar_then_detail"):S("doctor_detail_first")},{S("selected_date_evidence"),legacyQuery?S("legacy_official_calendar_and_detail"):dateRefused?S("calendar_after_date_refusal"):calendarChecked?S("calendar_after_empty_detail"):S("doctor_detail")},{S("requested_date_count"),1},{S("full_department_calendar_coverage"),false},{S("selected_date_completed"),true}};
   }if(report.value(S("hospital_system"))==QJsonValue(S("legacy"))&&broker_){broker_->close();broker_.reset();}report[S("status")]=S("complete");report[S("business_success")]=true;
  }report[S("exit_status")]=0;report[S("failure")]=QJsonValue(QJsonValue::Null);
 }catch(const std::exception &e){const QString raw=QString::fromUtf8(e.what());const QString failure=QRegularExpression(S("^[a-z_0-9]{1,100}$")).match(raw).hasMatch()?raw:S("native_service_failure");dispatches=broker_?qMax(0,broker_->dispatches()-before):0;report[S("failure")]=failure;report[S("status")]=failure==S("cancelled_by_user")?S("cancelled"):S("failed");report[S("rows")]=QJsonArray();const bool keptForBudget=failure==S("public_session_budget_exhausted")&&broker_&&broker_->alive();report[S("session_invalidated")]=!keptForBudget;if(broker_){report[S("runtime")]=broker_->diagnostics();if(!keptForBudget){broker_->close();report[S("runtime")]=broker_->diagnostics();broker_.reset();}}}
 if(broker_)dispatches=qMax(0,broker_->dispatches()-before);if(report.value(S("hospital_system"))==QJsonValue(S("legacy"))){dispatches=0;for(const auto &entry:responses_)dispatches+=entry.toObject().value(S("http_dispatches")).toInt();}int api=0;for(const auto &v:responses_)if(v.toObject().value(S("business_code"))==QJsonValue(S("0000"))||(v.toObject().value(S("source_system"))==QJsonValue(S("legacy"))&&v.toObject().value(S("business_code"))==QJsonValue(0)))++api;report[S("responses")]=responses_;report[S("performance")]=QJsonObject{{S("elapsed_seconds"),elapsed.elapsed()/1000.0},{S("metadata_lookup_ms"),metadataLookupMs},{S("http_dispatches"),dispatches},{S("session_reused"),reused_},{S("metadata_cache_hit"),hit},{S("metadata_cache_source"),source},{S("metadata_source"),source},{S("session_number"),session_},{S("renew_reason"),renewReason_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(renewReason_)},{S("api_requests"),api},{S("session_api_used"),broker_?broker_->apiUsed():0},{S("session_age_seconds"),broker_?QJsonValue(born_.elapsed()/1000.0):QJsonValue(QJsonValue::Null)}};report[S("session_kept_open")]=bool(broker_);report[S("query_allowed")]=broker_&&broker_->alive()&&broker_->apiUsed()<8&&broker_->dispatches()<12;report[S("cleanup_complete")]=!broker_;if(broker_)report[S("runtime")]=broker_->diagnostics();if(!cacheWarning_.isEmpty())report[S("metadata_cache_warning")]=cacheWarning_;report[S("finished_at")]=utcNow();cancel_=nullptr;busy_=false;return report;
}

QJsonObject Service::crawlAllDepartments(const QString &root,std::atomic_bool *cancel,int workers){
 if(workers<1||workers>4)return {{S("status"),S("failed")},{S("exit_status"),2},{S("failure"),S("department_snapshot_workers_invalid")},{S("network_requests"),0}};
 QElapsedTimer elapsed;elapsed.start();const QString absolute=QDir(root).absolutePath();const auto catalog=readHospitalCatalog(absolute);const QString csvHash=QString::fromLatin1(catalog.digest);
 const QString stageRoot=QDir(absolute).filePath(S("output/hospital_availability_http_fast_gui/department_snapshot_staging/")+csvHash);
 if(reparse(stageRoot)||!QDir().mkpath(stageRoot))return {{S("status"),S("failed")},{S("failure"),S("department_snapshot_stage_unavailable")},{S("exit_status"),2}};
 QLockFile batchLock(QDir(stageRoot).filePath(S("crawl.lock")));if(!batchLock.tryLock(0))return {{S("status"),S("failed")},{S("exit_status"),2},{S("failure"),S("department_snapshot_crawl_already_running")},{S("stage_path"),stageRoot}};
 try{if(!catalog.hospitals.isEmpty()){const auto installed=readSnapshotEntry(absolute,catalog.hospitals.first().code);if(!installed.isEmpty())return {{S("status"),S("complete")},{S("already_complete"),true},{S("worker_count"),workers},{S("max_active_workers"),0},{S("hospital_count"),catalog.hospitals.size()},{S("source_hospital_csv_sha256"),csvHash},{S("snapshot_path"),snapshotPath(absolute)},{S("network_requests"),0},{S("elapsed_seconds"),elapsed.elapsed()/1000.0}};}}catch(const Failure&){}
 const QString progressPath=QDir(stageRoot).filePath(S("progress.json"));
 QString crawlId,startedAt;QFile oldProgress(progressPath);if(oldProgress.open(QIODevice::ReadOnly)){const QByteArray oldBytes=oldProgress.readAll();oldProgress.close();QJsonParseError error;const auto doc=QJsonDocument::fromJson(oldBytes,&error);if(error.error==QJsonParseError::NoError&&doc.isObject()){const auto old=doc.object();if(old.value(S("source_hospital_csv_sha256")).toString()==csvHash&&old.value(S("hospital_count")).toInt(-1)==catalog.hospitals.size()){crawlId=old.value(S("crawl_id")).toString();startedAt=old.value(S("started_at")).toString();}}}
 if(crawlId.isEmpty()){crawlId=QUuid::createUuid().toString(QUuid::WithoutBraces);startedAt=utcNow();}
 QHash<QString,QJsonObject> complete;int requests=0,networkFetched=0;
 auto stageFile=[&](const QString &code){return QDir(stageRoot).filePath(QString::fromLatin1(QCryptographicHash::hash(code.toUtf8(),QCryptographicHash::Sha256).toHex())+S(".json"));};
 auto readStage=[&](const HospitalRecord &hospital)->QJsonObject{
  const QString path=stageFile(hospital.code);QFileInfo info(path);if(!info.exists())return {};if(!info.isFile()||reparse(path)||info.size()>CACHE_MAX)throw Failure(S("department_snapshot_stage_invalid"));QFile f(path);if(!f.open(QIODevice::ReadOnly))throw Failure(S("department_snapshot_stage_invalid"));const auto raw=f.readAll();decodeUtf8(raw);QJsonParseError error;const auto doc=QJsonDocument::fromJson(raw,&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())throw Failure(S("department_snapshot_stage_invalid"));const auto entry=doc.object();
  if(!exactKeys(entry,{S("crawl_id"),S("hospital_code"),S("hospital_name"),S("fetched_at"),S("department_count"),S("departments_sha256"),S("departments")})||entry.value(S("crawl_id")).toString()!=crawlId||entry.value(S("hospital_code")).toString()!=hospital.code||entry.value(S("hospital_name")).toString()!=hospital.name)throw Failure(S("department_snapshot_stage_invalid"));
  const auto deps=validateProjection(entry.value(S("departments")));if(entry.value(S("department_count")).toInt(-1)!=deps.size())throw Failure(S("department_snapshot_stage_invalid"));parseUtc(entry.value(S("fetched_at")));const QString digest=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(deps).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());if(entry.value(S("departments_sha256")).toString()!=digest)throw Failure(S("department_snapshot_stage_invalid"));return entry;
 };
 auto progress=[&](int finished,const QString &current,const QString &failure){QJsonArray done;QStringList keys=complete.keys();keys.sort();for(const auto &code:keys)done.append(code);QJsonObject p{{S("schema_version"),1},{S("crawl_id"),crawlId},{S("started_at"),startedAt},{S("source_hospital_csv_sha256"),csvHash},{S("hospital_count"),catalog.hospitals.size()},{S("worker_count"),workers},{S("completed_count"),finished},{S("completed_hospital_codes"),done},{S("current_hospital_code"),current},{S("failed_hospital_code"),failure.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(failure)},{S("checkpoint_updated_at"),utcNow()}};bool written=false;for(int attempt=0;attempt<26;++attempt){if(saveAtomicJson(progressPath,p)){written=true;break;}if(attempt<25)QThread::msleep(20);}if(!written)throw Failure(S("department_snapshot_progress_write_failed"));};
 QList<HospitalRecord> pending;
 for(const auto &hospital:catalog.hospitals){
  if(cancel&&cancel->load())return {{S("status"),S("cancelled")},{S("exit_status"),130},{S("failure"),S("cancelled_by_user")},{S("completed_hospitals"),complete.size()},{S("hospital_count"),catalog.hospitals.size()},{S("network_requests"),0},{S("worker_count"),workers},{S("max_active_workers"),0},{S("stage_path"),stageRoot},{S("progress_path"),progressPath}};
  try{const auto entry=readStage(hospital);if(!entry.isEmpty()){complete.insert(hospital.code,entry);continue;}}catch(const Failure&){}pending.append(hospital);
 }
 progress(complete.size(),pending.isEmpty()?QString():pending.first().code,QString());
 struct Completed { HospitalRecord hospital; QJsonObject result; };
 std::mutex queueMutex;std::condition_variable ready;std::deque<Completed> results;
 std::atomic_bool stopped(false);std::atomic_int next(0),active(0),peak(0),running(0);
 const int actualWorkers=qMin(workers,pending.size());std::vector<QThread*> threads;
 // Each worker owns its Service/Broker on one Qt thread; only this coordinator writes batch state.
 auto push=[&](const HospitalRecord &hospital,const QJsonObject &result){std::lock_guard<std::mutex> lock(queueMutex);results.push_back({hospital,result});ready.notify_one();};
 auto join=[&](){for(auto *thread:threads){thread->wait();delete thread;}threads.clear();};
 QString failure,failedCode;
 try{
  for(int worker=0;worker<actualWorkers;++worker){
   ++running;auto *thread=QThread::create([&](){
    HospitalRecord current;
    try{
     Service service(absolute);
     while(!stopped.load()){
      if(cancel&&cancel->load()){stopped.store(true);break;}
      const int index=next.fetch_add(1);if(index>=pending.size())break;current=pending.at(index);
      if(stopped.load())break;
      const int now=++active;int high=peak.load();while(now>high&&!peak.compare_exchange_weak(high,now)){}
      const QJsonObject params{{S("hospital_code"),current.code},{S("allow_department_network"),true},{S("fresh_department_network"),true}};
      QJsonObject result;
      try{result=service.execute(S("departments"),params,&stopped);}catch(...){--active;throw;}
      --active;push(current,result);
      if(result.value(S("status")).toString()!=S("complete")||!result.value(S("business_success")).toBool()){stopped.store(true);break;}
     }
     service.close();
    }catch(const std::exception &e){stopped.store(true);push(current,{{S("status"),S("failed")},{S("failure"),QString::fromUtf8(e.what())}});}
    catch(...){stopped.store(true);push(current,{{S("status"),S("failed")},{S("failure"),S("department_snapshot_worker_failed")}});}
    --running;ready.notify_one();
   });threads.push_back(thread);thread->start();
  }
  while(true){
   if(cancel&&cancel->load())stopped.store(true);
   Completed item;
   {std::unique_lock<std::mutex> lock(queueMutex);ready.wait_for(lock,std::chrono::milliseconds(20),[&](){return !results.empty()||running.load()==0;});if(results.empty()){if(running.load()==0)break;continue;}item=std::move(results.front());results.pop_front();}
   const auto &hospital=item.hospital;const auto &result=item.result;requests+=result.value(S("performance")).toObject().value(S("http_dispatches")).toInt();
   if(result.value(S("status")).toString()!=S("complete")||!result.value(S("business_success")).toBool()){
    const QString why=result.value(S("failure")).toString(S("department_fetch_failed"));
    if(failure.isEmpty()||(failure==S("cancelled_by_user")&&why!=S("cancelled_by_user"))){failure=why;failedCode=hospital.code;}stopped.store(true);continue;
   }
   const auto deps=validateProjection(result.value(S("departments")));const QString stamp=result.value(S("metadata_queried_at")).toString();parseUtc(QJsonValue(stamp));const QString digest=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(deps).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
   const QJsonObject entry{{S("crawl_id"),crawlId},{S("hospital_code"),hospital.code},{S("hospital_name"),hospital.name},{S("fetched_at"),stamp},{S("department_count"),deps.size()},{S("departments_sha256"),digest},{S("departments"),deps}};
   if(!saveAtomicJson(stageFile(hospital.code),entry)){failure=S("department_snapshot_stage_write_failed");failedCode=hospital.code;stopped.store(true);continue;}
   ++networkFetched;complete.insert(hospital.code,entry);progress(complete.size(),hospital.code,QString());
  }
  join();
 }catch(...){stopped.store(true);join();throw;}
 if(cancel&&cancel->load()&&failure.isEmpty())failure=S("cancelled_by_user");
 if(!failure.isEmpty()){
  progress(complete.size(),failedCode,failure);return {{S("status"),failure==S("cancelled_by_user")?S("cancelled"):S("failed")},{S("exit_status"),failure==S("cancelled_by_user")?130:1},{S("failure"),failure},{S("failed_hospital_code"),failedCode},{S("completed_hospitals"),complete.size()},{S("hospital_count"),catalog.hospitals.size()},{S("network_requests"),requests},{S("worker_count"),workers},{S("max_active_workers"),peak.load()},{S("stage_path"),stageRoot},{S("progress_path"),progressPath},{S("elapsed_seconds"),elapsed.elapsed()/1000.0}};
 }
 if(complete.size()!=catalog.hospitals.size())return {{S("status"),S("failed")},{S("exit_status"),2},{S("failure"),S("department_snapshot_coverage_incomplete")},{S("completed_hospitals"),complete.size()},{S("hospital_count"),catalog.hospitals.size()},{S("stage_path"),stageRoot}};
 QJsonArray hospitals;for(const auto &hospital:catalog.hospitals){const auto e=complete.value(hospital.code);hospitals.append(QJsonObject{{S("hospital_code"),e.value(S("hospital_code"))},{S("hospital_name"),e.value(S("hospital_name"))},{S("fetched_at"),e.value(S("fetched_at"))},{S("department_count"),e.value(S("department_count"))},{S("departments_sha256"),e.value(S("departments_sha256"))},{S("departments"),e.value(S("departments"))}});}
 QJsonObject snapshot{{S("schema_version"),1},{S("complete"),true},{S("source_hospital_csv_sha256"),csvHash},{S("fetched_at"),utcNow()},{S("hospital_count"),catalog.hospitals.size()},{S("hospitals"),hospitals}};
 const QByteArray encoded=QJsonDocument(snapshot).toJson(QJsonDocument::Compact);if(encoded.size()>128*1024*1024||!saveAtomicJson(snapshotPath(absolute),snapshot))return {{S("status"),S("failed")},{S("exit_status"),2},{S("failure"),encoded.size()>128*1024*1024?S("department_snapshot_oversize"):S("department_snapshot_publish_failed")},{S("completed_hospitals"),complete.size()},{S("hospital_count"),catalog.hospitals.size()},{S("stage_path"),stageRoot}};
 try{readSnapshotEntry(absolute,catalog.hospitals.first().code);}catch(const Failure &){return {{S("status"),S("failed")},{S("exit_status"),2},{S("failure"),S("department_snapshot_postpublish_validation_failed")},{S("snapshot_path"),snapshotPath(absolute)},{S("stage_path"),stageRoot}};}
 progress(complete.size(),QString(),QString());return {{S("status"),S("complete")},{S("exit_status"),0},{S("already_complete"),false},{S("hospital_count"),complete.size()},{S("network_fetched_hospitals"),networkFetched},{S("network_requests"),requests},{S("worker_count"),workers},{S("max_active_workers"),peak.load()},{S("source_hospital_csv_sha256"),csvHash},{S("snapshot_path"),snapshotPath(absolute)},{S("stage_path"),stageRoot},{S("progress_path"),progressPath},{S("elapsed_seconds"),elapsed.elapsed()/1000.0}};
}

QJsonObject Service::offlineSelfTest(){
 int passed=0;QJsonArray names;auto check=[&](bool ok,const QString &name){if(!ok)throw Failure(S("native_selftest_failed"));++passed;names.append(name);};QTemporaryDir tmp;if(!tmp.isValid())throw Failure(S("native_selftest_temp_failed"));
 auto boundedFixture=[&](int size,int maximum,const QString &label){QByteArray input(size,Qt::Uninitialized);for(int n=0;n<size;++n)input[n]=char(n*17);QBuffer buffer(&input);check(buffer.open(QIODevice::ReadOnly),label+S("_open"));const auto actual=readBoundedContent(buffer,maximum);const int expected=qMin(size,maximum+1);check(actual==input.left(expected)&&buffer.pos()==expected,label+S("_bytes_and_consumption"));};
 boundedFixture(0,0,S("bounded_read_empty"));boundedFixture(1,0,S("bounded_read_zero_limit_oversize"));boundedFixture(65535,65536,S("bounded_read_before_chunk"));boundedFixture(65536,65536,S("bounded_read_exact_chunk_and_limit"));boundedFixture(65537,65536,S("bounded_read_limit_plus_one"));boundedFixture(196608,65536,S("bounded_read_stops_at_limit_plus_one"));boundedFixture(2570245,128*1024*1024,S("bounded_read_large_binary_content"));
 class ChangingReadDevice final:public QIODevice {QByteArray bytes_,replacement_;int offset_=0,calls_=0;bool error_;protected:qint64 readData(char *data,qint64 maximum)override{if(calls_++==1){if(error_)return -1;bytes_=replacement_;}const qint64 count=qMin(maximum,qMax(qint64(0),qint64(bytes_.size())-offset_));if(count>0)std::copy_n(bytes_.constData()+offset_,int(count),data);offset_+=int(count);return count;}qint64 writeData(const char *,qint64)override{return -1;}public:ChangingReadDevice(const QByteArray &first,const QByteArray &next,bool error=false):bytes_(first),replacement_(next),error_(error){open(QIODevice::ReadOnly|QIODevice::Unbuffered);}qint64 size()const override{return bytes_.size();}bool isSequential()const override{return true;}};
 {const QByteArray initial(65536,'a'),grown=initial+QByteArray(65536,'b');ChangingReadDevice device(initial,grown);check(readBoundedContent(device,200000)==grown,S("bounded_read_growth_after_first_chunk_complete"));}
 {const QByteArray initial(65536,'a'),grown=initial+QByteArray(65536,'b');ChangingReadDevice device(initial,grown);check(readBoundedContent(device,65536)==grown.left(65537),S("bounded_read_growth_oversize_sentinel"));}
 {const QByteArray initial(131072,'c');ChangingReadDevice device(initial,initial.left(65536));check(readBoundedContent(device,200000)==initial.left(65536),S("bounded_read_truncation_returns_actual_bytes"));}
 {ChangingReadDevice device(QByteArray(131072,'d'),QByteArray(),true);bool failed=false;try{readBoundedContent(device,200000);}catch(const Failure &e){failed=QString::fromUtf8(e.what())==S("metadata_content_read_failed");}check(failed,S("bounded_read_io_error_never_returns_partial_success"));}
 const QByteArray emptyHash=QByteArray::fromHex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 check(contentSha256(QByteArray())==emptyHash,S("content_sha256_empty_standard_vector"));
 check(contentSha256(QByteArray("abc")).toHex()==QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),S("content_sha256_abc_standard_vector"));
 QByteArray binary(256,Qt::Uninitialized);for(int n=0;n<binary.size();++n)binary[n]=char(n);
 check(contentSha256(binary)==QCryptographicHash::hash(binary,QCryptographicHash::Sha256),S("content_sha256_all_binary_bytes_match_qt"));
 for(int size:{55,56,63,64,65,127,128,129}){QByteArray block(size,Qt::Uninitialized);for(int n=0;n<size;++n)block[n]=char(n);check(contentSha256(block)==QCryptographicHash::hash(block,QCryptographicHash::Sha256),S("content_sha256_padding_boundary_")+QString::number(size));}
 QByteArray large(2570245,Qt::Uninitialized);for(int n=0;n<large.size();++n)large[n]=char(n*31);const auto largeExpected=QCryptographicHash::hash(large,QCryptographicHash::Sha256);
 check(contentSha256(large)==largeExpected,S("content_sha256_large_binary_matches_qt"));
 check(contentSha256(large,false)==largeExpected&&contentSha256(QByteArray(),false)==emptyHash,S("content_sha256_qt_fallback_matches"));
 for(int offset:{0,128,2570244}){auto changed=large;changed[offset]=char(static_cast<unsigned char>(changed.at(offset))^1);check(contentSha256(changed)!=largeExpected&&contentSha256(changed)==QCryptographicHash::hash(changed,QCryptographicHash::Sha256),S("content_sha256_detects_mutation_")+QString::number(offset));}
 std::atomic_int hashFailures{0};std::vector<std::thread> hashReaders;for(int n=0;n<4;++n)hashReaders.emplace_back([&](){for(int i=0;i<20;++i)if(contentSha256(large)!=largeExpected)++hashFailures;});for(auto &reader:hashReaders)reader.join();check(hashFailures==0,S("content_sha256_four_thread_consistency"));
 QJsonObject leaf{{S("code"),S("leaf")},{S("name"),S("Leaf")}};QJsonObject parent{{S("code"),S("root")},{S("name"),S("Root")},{S("subList"),QJsonArray{leaf}}};QJsonArray data{parent};auto deps=flattenDepartments(data);check(deps.size()==1&&deps[0].toObject().value(S("first_department_code")).toString()==S("root"),S("flatten_nested"));
 QJsonObject value{{S("schema_version"),1},{S("hospital_code"),S("H01")},{S("hospital_name"),S("Fixture")},{S("fetched_at"),utcNow()},{S("cached_at"),utcNow()},{S("departments"),deps}};check(!validateCache(value,S("H01")).isEmpty(),S("strict_valid_cache"));auto reject=[&](QJsonObject v,const QString &name){bool failed=false;try{validateCache(v,S("H01"));}catch(const Failure&){failed=true;}check(failed,name);};auto bad=value;bad[S("cookie")]=S("forbidden");reject(bad,S("extra_sensitive_rejected"));bad=value;bad[S("schema_version")]=true;reject(bad,S("boolean_schema_rejected"));bad=value;bad[S("hospital_code")]=S("H02");reject(bad,S("identity_rejected"));bad=value;bad[S("fetched_at")]=QDateTime::currentDateTimeUtc().addSecs(-TTL).toString(Qt::ISODateWithMs);reject(bad,S("expired_rejected"));bad=value;bad[S("cached_at")]=QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs);reject(bad,S("future_rejected"));bad=value;bad[S("departments")]=QJsonArray{deps[0],deps[0]};reject(bad,S("duplicate_leaf_rejected"));bad=value;auto d=deps[0].toObject();d[S("token")]=S("forbidden");bad[S("departments")]=QJsonArray{d};reject(bad,S("department_extra_rejected"));
 bad=value;bad[S("fetched_at")]=S("2026-11-09T12:00:00");reject(bad,S("naive_timestamp_rejected"));bad=value;bad[S("fetched_at")]=QDateTime::currentDateTimeUtc().toOffsetFromUtc(3600).toString(Qt::ISODateWithMs);reject(bad,S("non_utc_timestamp_rejected"));bad=value;bad[S("hospital_name")]=S("bad\nname");reject(bad,S("control_character_name_rejected"));check(cacheOnly(tmp.path(),S("../unsafe")).isEmpty(),S("invalid_cache_code_safe_miss"));
 const auto path=cachePath(tmp.path(),S("H01"),true);QSaveFile file(path);check(file.open(QIODevice::WriteOnly),S("atomic_open"));file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));check(file.commit(),S("atomic_commit"));check(!cacheOnly(tmp.path(),S("H01")).isEmpty(),S("disk_cache_no_broker"));QFile oversized(cachePath(tmp.path(),S("H02")));check(oversized.open(QIODevice::WriteOnly),S("oversize_open"));oversized.write(QByteArray(CACHE_MAX+1,' '));oversized.close();check(cacheOnly(tmp.path(),S("H02")).isEmpty(),S("oversize_rejected"));
 auto csv=csvRows(S("hospital_code,hospital_name\nH01,\"Quoted, name\"\nH02,\"two \"\"quotes\"\"\"\n"));check(csv.size()==3&&csv[1][1]==S("Quoted, name")&&csv[2][1]==S("two \"quotes\""),S("csv_quotes"));
 QJsonObject period{{S("dutyTimeView"),S("Morning")},{S("ncode"),S("0")}},duty{{S("productStatus"),2},{S("period"),QJsonArray{period}}},doctor{{S("doctorName"),S("Public doctor")},{S("detail"),QJsonArray{duty}}},doc{{S("dutyDate"),S("20261109")},{S("doctors"),QJsonArray{doctor}}};auto rows=doctorRows(doc,deps[0].toObject(),S("20261109"));check(rows.size()==1&&rows[0].toObject().value(S("remaining")).toInt(-1)==0&&rows[0].toObject().value(S("status")).toString()==S("约满"),S("doctor_status_remaining_zero"));check(remaining(QJsonValue(true)).isNull()&&remaining(QJsonValue(S("-1"))).isNull(),S("remaining_unknown_preserved"));
 return {{S("status"),S("PASS")},{S("tests_passed"),passed},{S("tests"),names},{S("network_attempts"),0},{S("python_used"),false}};
}
}
