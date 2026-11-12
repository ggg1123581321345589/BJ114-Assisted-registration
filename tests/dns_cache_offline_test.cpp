// Extracted DNS helper snapshot; fixed answer/clock-delay fixture. No external lookup.
#define MODERN 1
#define ROLE "MODIFIED"
#define SOURCE_SHA "331863f5d907d3a1351661bd27beb9a5023c47269032dcfabf420c93f8507388"
#include <QCoreApplication>
#include <QHostInfo>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <stdexcept>
#include <vector>
#include <cstdio>
const QString HOST=QStringLiteral("www.114yygh.com");
std::atomic_int calls{0},delayMs{0},activeWorkers{0},maxWorkers{0};std::atomic_bool fail{false};
void fixtureDelay(){++calls;int n=++activeWorkers;int m=maxWorkers.load();while(n>m&&!maxWorkers.compare_exchange_weak(m,n)){}std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));--activeWorkers;}
QHostInfo fixtureResolve(const QString&){fixtureDelay();QHostInfo h;h.setError(fail?QHostInfo::HostNotFound:QHostInfo::NoError);if(!fail)h.setAddresses({QHostAddress(QStringLiteral("192.0.2.1")),QHostAddress(QStringLiteral("2001:db8::1"))});return h;}
// One process-wide DNS lookup serves every connection owner. Refresh never erases
// last-good addresses: they may bridge a transient resolver outage for five minutes.
constexpr int PUBLIC_DNS_WAIT_MS=13000,PUBLIC_DNS_RESOLVER_MS=12000;
constexpr int PUBLIC_DNS_REFRESH_SECONDS=45,PUBLIC_DNS_FRESH_SECONDS=60,PUBLIC_DNS_STALE_SECONDS=300;
struct PublicDnsState {
 std::mutex mutex;std::condition_variable changed;bool resolving=false;unsigned long long generation=0;
 QStringList addresses;QString failure;std::chrono::steady_clock::time_point completed,succeeded;double lookupMs=0;
};
PublicDnsState &publicDnsState(){static auto *state=new PublicDnsState;return *state;}
double publicDnsAgeMs(const PublicDnsState &state){return state.succeeded==std::chrono::steady_clock::time_point{}?-1:std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-state.succeeded).count();}
bool publicDnsUsable(const PublicDnsState &state){const auto age=publicDnsAgeMs(state);return !state.addresses.isEmpty()&&age>=0&&age<PUBLIC_DNS_STALE_SECONDS*1000.0;}
bool publicDnsFresh(const PublicDnsState &state){const auto age=publicDnsAgeMs(state);return publicDnsUsable(state)&&age<PUBLIC_DNS_FRESH_SECONDS*1000.0;}
struct SystemDnsAnswer {QStringList addresses;QString failure;};
SystemDnsAnswer systemPublicDnsLookup(){fixtureDelay();SystemDnsAnswer a;if(fail)a.failure=QStringLiteral("native_dns_resolution_failed");else a.addresses=QStringList({QStringLiteral("192.0.2.1"),QStringLiteral("[2001:db8::1]")});return a;}
void startPublicDnsLookup(){
 auto &state=publicDnsState();unsigned long long generation;
 {std::lock_guard<std::mutex> lock(state.mutex);const auto now=std::chrono::steady_clock::now();
  if(state.resolving||(publicDnsUsable(state)&&publicDnsAgeMs(state)<PUBLIC_DNS_REFRESH_SECONDS*1000.0)||(state.completed!=std::chrono::steady_clock::time_point{}&&now-state.completed<std::chrono::seconds(3)))return;
  state.resolving=true;generation=++state.generation;
 }
 try{std::thread([generation]{
  const auto begin=std::chrono::steady_clock::now();SystemDnsAnswer answer;
  try{answer=systemPublicDnsLookup();}catch(...){answer.failure=QStringLiteral("native_dns_resolution_failed");}
  auto &state=publicDnsState();std::lock_guard<std::mutex> lock(state.mutex);if(generation!=state.generation)return;
  if(!answer.addresses.isEmpty()){state.addresses=answer.addresses;state.succeeded=std::chrono::steady_clock::now();}
  state.failure=answer.failure;state.lookupMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();state.completed=std::chrono::steady_clock::now();state.resolving=false;state.changed.notify_all();
 }).detach();}catch(...){std::lock_guard<std::mutex> lock(state.mutex);state.failure=QStringLiteral("native_dns_worker_unavailable");state.lookupMs=0;state.completed=std::chrono::steady_clock::now();state.resolving=false;state.changed.notify_all();}
}
struct PublicDnsResult {
 QStringList addresses;QString failure;double waitMs=0,lookupMs=0;bool cacheHit=false,resolverInFlight=false;double cacheAgeMs=-1;bool staleFallback=false;QString refreshFailure;
 QJsonObject diagnostics()const{return QJsonObject{{QStringLiteral("source"),QStringLiteral("system_shared_bounded_resolver")},{QStringLiteral("deadline_ms"),PUBLIC_DNS_WAIT_MS},{QStringLiteral("resolver_deadline_ms"),PUBLIC_DNS_RESOLVER_MS},{QStringLiteral("wait_ms"),waitMs},{QStringLiteral("lookup_ms"),lookupMs},{QStringLiteral("cache_hit"),cacheHit},{QStringLiteral("cache_age_ms"),cacheAgeMs},{QStringLiteral("stale_fallback"),staleFallback},{QStringLiteral("stale_limit_seconds"),PUBLIC_DNS_STALE_SECONDS},{QStringLiteral("address_count"),addresses.size()},{QStringLiteral("resolver_inflight"),resolverInFlight},{QStringLiteral("refresh_failure"),refreshFailure},{QStringLiteral("failure"),failure}};}
};
template<typename Check,typename Drain>
PublicDnsResult resolvePublicDns(Check check,Drain drain){
 check();const auto begin=std::chrono::steady_clock::now();auto &state=publicDnsState();bool cached;{std::lock_guard<std::mutex> lock(state.mutex);cached=publicDnsUsable(state);}startPublicDnsLookup();
 for(;;){check();std::unique_lock<std::mutex> lock(state.mutex);const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  if(publicDnsUsable(state))return {state.addresses,QString(),elapsed,state.lookupMs,cached,state.resolving,publicDnsAgeMs(state),!publicDnsFresh(state),state.failure};
  if(!state.resolving&&!state.failure.isEmpty())return {{},state.failure,elapsed,state.lookupMs,cached,false,-1,false,state.failure};
  if(elapsed>=PUBLIC_DNS_WAIT_MS)return {{},QStringLiteral("native_dns_timeout"),elapsed,state.lookupMs,cached,state.resolving};
  state.changed.wait_for(lock,std::chrono::milliseconds(20));lock.unlock();drain();
 }
}

void settle(){for(int n=0;n<1000;++n){bool done;{auto&s=publicDnsState();std::lock_guard<std::mutex>l(s.mutex);done=!s.resolving;}if(done&&activeWorkers.load()==0)return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}throw std::runtime_error("fixture_worker_still_running");}
void reset(){settle();auto&s=publicDnsState();std::lock_guard<std::mutex>l(s.mutex);s.addresses.clear();s.failure.clear();s.completed={};s.lookupMs=0;
#if MODERN
s.succeeded={};
#endif
calls=0;maxWorkers=0;}
void seed(int age){auto&s=publicDnsState();std::lock_guard<std::mutex>l(s.mutex);s.addresses=QStringList({QStringLiteral("192.0.2.9")});s.completed=std::chrono::steady_clock::now()-std::chrono::seconds(age);
#if MODERN
s.succeeded=s.completed;
#endif
}
double elapsedMs(std::chrono::steady_clock::time_point start){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonArray checks,observed;int failed=0;auto test=[&](bool ok,const char*n){checks.append(QJsonObject{{"name",n},{"pass",ok}});if(!ok)++failed;};auto nothing=[]{};
reset();delayMs=80;fail=false;std::vector<std::thread>threads;std::atomic_int ok{0};for(int i=0;i<16;++i)threads.emplace_back([&]{auto r=resolvePublicDns(nothing,nothing);if(r.failure.isEmpty()&&r.addresses==QStringList({QStringLiteral("192.0.2.1"),QStringLiteral("[2001:db8::1]")}))++ok;});for(auto&t:threads)t.join();test(calls==1&&ok==16,"sixteen_waiters_share_one_lookup_ipv6_preserved");test(maxWorkers==1,"process_lookup_workers_bounded_to_one");auto fresh=resolvePublicDns(nothing,nothing);test(fresh.cacheHit&&calls==1&&fresh.waitMs<30,"fresh_cache_immediate_without_new_lookup");
reset();delayMs=1800;fail=false;auto start=std::chrono::steady_clock::now();auto cold=resolvePublicDns(nothing,nothing);double coldMs=elapsedMs(start);test(MODERN?(cold.failure.isEmpty()&&coldMs>=1750&&coldMs<2300):(cold.failure==QStringLiteral("native_dns_timeout")&&coldMs>=1490&&coldMs<1700),"same_1800ms_cold_input_variant_behavior");observed.append(QJsonObject{{"scenario","cold_1800ms"},{"failure",cold.failure},{"elapsed_ms",coldMs},{"lookup_count",int(calls)},{"inflight_at_result",cold.resolverInFlight}});settle();auto later=resolvePublicDns(nothing,nothing);test(later.failure.isEmpty()&&calls==1,"late_or_completed_lookup_success_reused");
reset();delayMs=11000;fail=false;start=std::chrono::steady_clock::now();auto slowNative=resolvePublicDns(nothing,nothing);double slowNativeMs=elapsedMs(start);test(slowNative.failure.isEmpty()&&slowNative.addresses.size()==2&&calls==1&&slowNativeMs>=10950&&slowNativeMs<11500,"eleven_second_cold_lookup_succeeds_with_13s_budget");observed.append(QJsonObject{{"scenario","cold_11000ms"},{"failure",slowNative.failure},{"elapsed_ms",slowNativeMs},{"lookup_count",int(calls)}});
reset();delayMs=300;fail=true;seed(61);start=std::chrono::steady_clock::now();auto old=resolvePublicDns(nothing,nothing);double oldMs=elapsedMs(start);test(MODERN?(old.failure.isEmpty()&&old.addresses==QStringList({QStringLiteral("192.0.2.9")})&&oldMs<60):(old.failure==QStringLiteral("native_dns_resolution_failed")&&old.addresses.isEmpty()&&oldMs>=250),"same_61s_cache_refresh_failure_variant_behavior");
// Allow the single refresh to finish before testing the retained result.
settle();auto again=resolvePublicDns(nothing,nothing);test(calls==1,"refresh_failure_negative_cooldown_prevents_duplicate_lookup");test(MODERN?(again.failure.isEmpty()&&again.addresses==QStringList({QStringLiteral("192.0.2.9")})):(again.failure==QStringLiteral("native_dns_resolution_failed")&&again.addresses.isEmpty()),"refresh_failure_retains_last_good_only_in_modified");
#if MODERN
test(old.staleFallback&&old.cacheHit&&old.resolverInFlight,"stale_use_diagnosed_during_background_refresh");test(again.refreshFailure==QStringLiteral("native_dns_resolution_failed"),"stale_refresh_failure_exposed_in_diagnostics");
#endif
observed.append(QJsonObject{{"scenario","61s_cache_refresh_failure"},{"first_failure",old.failure},{"first_elapsed_ms",oldMs},{"first_address_count",old.addresses.size()},{"after_failure",again.failure},{"after_address_count",again.addresses.size()},{"lookup_count",int(calls)}});
reset();delayMs=120;fail=true;seed(61);threads.clear();std::atomic_int staleOk{0},staleFail{0};start=std::chrono::steady_clock::now();for(int i=0;i<16;++i)threads.emplace_back([&]{auto r=resolvePublicDns(nothing,nothing);if(r.failure.isEmpty()&&r.addresses==QStringList({QStringLiteral("192.0.2.9")}))++staleOk;else if(!r.failure.isEmpty())++staleFail;});for(auto&t:threads)t.join();double stale16Ms=elapsedMs(start);test(MODERN?(staleOk==16&&stale16Ms<100):(staleFail==16&&stale16Ms>=100),"sixteen_stale_waiters_variant_behavior");settle();test(calls==1&&maxWorkers==1,"sixteen_stale_waiters_single_background_refresh");observed.append(QJsonObject{{"scenario","sixteen_61s_cache_refresh_failure"},{"success_count",int(staleOk)},{"failure_count",int(staleFail)},{"elapsed_ms",stale16Ms},{"lookup_count",int(calls)}});
reset();delayMs=100;fail=false;seed(50);auto refreshEarly=resolvePublicDns(nothing,nothing);settle();test(refreshEarly.failure.isEmpty()&&refreshEarly.waitMs<50&&(MODERN?calls==1:calls==0),"fresh50s_background_refresh_starts_at45s_only_in_modified");
reset();delayMs=30;fail=true;seed(301);auto expired=resolvePublicDns(nothing,nothing);test(!expired.failure.isEmpty()&&expired.addresses.isEmpty(),"hard_age_301s_cache_not_accepted");
reset();delayMs=180;fail=false;start=std::chrono::steady_clock::now();bool cancelled=false;try{resolvePublicDns([&]{if(elapsedMs(start)>30)throw std::runtime_error("cancelled_by_user");},nothing);}catch(...){cancelled=true;}double cancelMs=elapsedMs(start);test(cancelled&&cancelMs<100,"cancel_observed_under100ms");settle();auto afterCancel=resolvePublicDns(nothing,nothing);test(afterCancel.failure.isEmpty()&&calls==1,"cancel_does_not_duplicate_or_destroy_shared_lookup");observed.append(QJsonObject{{"scenario","cancel_at30ms"},{"cancelled",cancelled},{"elapsed_ms",cancelMs},{"lookup_count",int(calls)}});
reset();delayMs=0;fail=true;auto neg=resolvePublicDns(nothing,nothing);auto neg2=resolvePublicDns(nothing,nothing);test(neg.failure==QStringLiteral("native_dns_resolution_failed")&&neg2.failure==neg.failure&&calls==1,"cold_failure_negative_cache_without_hidden_retry");
reset();delayMs=120;fail=false;startPublicDnsLookup();std::this_thread::sleep_for(std::chrono::milliseconds(20));{auto&s=publicDnsState();std::lock_guard<std::mutex>l(s.mutex);++s.generation;s.resolving=false;s.addresses=QStringList({QStringLiteral("192.0.2.77")});s.completed=std::chrono::steady_clock::now();
#if MODERN
s.succeeded=s.completed;
#endif
}std::this_thread::sleep_for(std::chrono::milliseconds(180));auto generation=resolvePublicDns(nothing,nothing);test(generation.addresses==QStringList({QStringLiteral("192.0.2.77")}),"late_generation_cannot_override_newer_result");
reset();delayMs=13500;fail=false;start=std::chrono::steady_clock::now();auto deadline=resolvePublicDns(nothing,nothing);double deadlineMs=elapsedMs(start);test(deadline.failure==QStringLiteral("native_dns_timeout")&&deadlineMs>=(MODERN?12990:1490)&&deadlineMs<(MODERN?13350:1700),"outer_wait_deadline_bounded_for_stuck_fixture_resolver");observed.append(QJsonObject{{"scenario","13500ms_outer_deadline"},{"failure",deadline.failure},{"elapsed_ms",deadlineMs},{"lookup_count",int(calls)}});settle();
QJsonObject out{{"status",failed?"FAIL":"PASS"},{"role",QStringLiteral(ROLE)},{"checks",checks},{"count",checks.size()},{"failed",failed},{"observed",observed},{"source_sha256",QStringLiteral(SOURCE_SHA)},{"external_dns_lookups",0},{"http_requests",0},{"scope","Exact copied DNS sharing/cache helper; only system lookup body substituted by deterministic fixture. Production Windows cancellation API is not exercised by this helper fixture."}};auto raw=QJsonDocument(out).toJson(QJsonDocument::Indented);fwrite(raw.constData(),1,raw.size(),stdout);return failed?1:0;
}
