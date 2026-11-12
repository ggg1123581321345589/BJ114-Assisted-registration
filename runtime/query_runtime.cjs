'use strict';
// Secret-bearing private JSONL IPC. Never log raw protocol traffic.
const readline=require('node:readline');
const hostIsolation=require('./helpers/private_window_runtime.cjs');hostIsolation.install();
require('./helpers/xpath_private_ast_runtime_20261109.cjs').install();
require('./helpers/form_named_properties_runtime_20261109.cjs').install();
const {applyClockOffset,validateClockOffset}=require('./helpers/clock_offset_audit/apply_clock_offset.cjs');
const {JSDOM,VirtualConsole,CookieJar,requestInterceptor}=require('jsdom');
const childWindowRuntime=require('./helpers/child_window_runtime_20261109.cjs');childWindowRuntime.install();
const idl=require('jsdom/lib/generated/idl/utils.js');
const whatwgURL=require('whatwg-url');
const {seedCookieJar,exportCookieJar}=require('./helpers/cookie_transfer.cjs');
const MAX=2_000_000,HOST='www.114yygh.com';
let dom,jar,config,initialized=false,ready=false,closed=false,sequence=0,generation=0;
let activeQuery=null,navigationPending=null,navigationCount=0,apiCount=0;
const pending=new Map(),errors=[],blockedResources=[],storage={local:[],session:[]};
const emit=value=>{if(!closed)process.stdout.write(JSON.stringify(value)+'\n');};
const errorCode=e=>typeof e?.message==='string'&&/^[a-z_]{3,80}$/.test(e.message)?e.message:(e?.name||'Error');
const snapshot=()=>jar?exportCookieJar(jar):[];
const documentURL=value=>{
 const u=new URL(value);
 if(u.protocol!=='https:'||u.hostname!==HOST||u.username||u.password||u.port||!u.pathname.startsWith('/newhlwyl/mobile/'))throw Error('document_target_rejected');
 return u;
};
const allowed=value=>{
 const u=new URL(value);
 return u.protocol==='https:'&&u.hostname===HOST&&!u.username&&!u.password&&(!u.port||u.port==='443')&&
 /^\/jtjk\/mobile-service\/(?:hospital\/(?:detail\/[^/]+|list)|department\/(?:list\/[^/]+|productDepartmentDetail)|product\/(?:calendar|doctor\/detail)|common\/(?:enum\/[^/]+|getProhibitLockConfigs))$/.test(u.pathname);
};
function closeDOM(){
 if(activeQuery){clearTimeout(activeQuery.timer);try{activeQuery.xhr.abort();}catch{}activeQuery=null;}
 for(const item of pending.values()){clearTimeout(item.timer);item.resolve(new Response('',{status:503}));}
 pending.clear();if(dom){dom.window.close();dom=null;}
}
function shutdown(){if(closed)return;closeDOM();closed=true;process.stdin.pause();process.exit(0);}
setTimeout(()=>{emit({event:'error',code:'runtime_watchdog'});shutdown();},180_000);
function saveStorage(){
 if(!dom)return;
 for(const [key,name]of [['local','localStorage'],['session','sessionStorage']]){
  try{const s=dom.window[name];storage[key]=Array.from({length:s.length},(_,i)=>{const k=s.key(i);return[k,s.getItem(k)];});}catch{storage[key]=[];}
 }
}
function build(input){
 const u=documentURL(input.url);
 if(typeof input.html!=='string'||Buffer.byteLength(input.html)>MAX)throw Error('document_payload_invalid');
 const parsed=new JSDOM(input.html,{url:u.href});let scripts;
 try{scripts=[...parsed.window.document.querySelectorAll('script[src]')].map(node=>node.src);}finally{parsed.window.close();}
 const mapped=input.resource_map!==undefined,resources=new Map();
 if(mapped){
  if(input.resource_map===null||typeof input.resource_map!=='object'||Array.isArray(input.resource_map))throw Error('resource_map_invalid');
  const entries=Object.entries(input.resource_map);if(entries.length>2)throw Error('resource_map_limit');
  for(const [value,content]of entries){
   const x=new URL(value);
   if(x.href!==value||x.protocol!=='https:'||x.hostname!==HOST||x.username||x.password||x.port)throw Error('resource_map_target_rejected');
   if(!scripts.includes(value))throw Error('resource_map_not_in_document');
   if(typeof content!=='string'||Buffer.byteLength(content)>MAX)throw Error('resource_map_body_invalid');
   resources.set(value,content);
  }
 }else{
  if(typeof input.external!=='string'||Buffer.byteLength(input.external)>MAX)throw Error('document_payload_invalid');
 if(scripts.some(value=>{const x=new URL(value);return x.protocol!=='https:'||x.hostname!==HOST||x.username||x.password||x.port;}))throw Error('external_target_rejected');
  for(const value of scripts)resources.set(value,input.external);
 }
 ready=false;const ownGeneration=++generation;
 const vc=new VirtualConsole();vc.on('jsdomError',e=>{errors.push({type:e.type,name:errorCode(e.cause)});if(errors.length>50)errors.shift();});
 const intercept=requestInterceptor(async(request,{element=null}={})=>{
  if(ownGeneration!==generation)return new Response('',{status:503});
  if(resources.has(request.url)&&request.method==='GET'&&element?.localName==='script')return new Response(resources.get(request.url),{status:200,headers:{'Content-Type':'text/javascript;charset=UTF-8'}});
  if(element){const x=new URL(request.url);blockedResources.push({generation:ownGeneration,element:element.localName||null,host:x.hostname,path:x.pathname,status:403});if(blockedResources.length>50)blockedResources.shift();return new Response('',{status:403});}
  if(scripts.includes(request.url)||request.destination==='script')return new Response('',{status:403});
  if(navigationPending||!activeQuery||new URL(request.url).pathname!==activeQuery.expectedPath||request.method!==activeQuery.expectedMethod||!allowed(request.url)||!['GET','POST'].includes(request.method))return new Response('',{status:403});
  if(++apiCount>8)return new Response('',{status:429});
  const body=Buffer.from(await request.arrayBuffer());if(body.length>MAX)return new Response('',{status:413});
  const id='http-'+(++sequence);
  return new Promise(resolve=>{
   const timer=setTimeout(()=>{pending.delete(id);resolve(new Response('',{status:504}));},20_000);
   pending.set(id,{resolve,timer,generation:ownGeneration});
   emit({event:'http_request',id,method:request.method,url:request.url,headers:[...request.headers.entries()],body_b64:body.toString('base64'),cookies:snapshot()});
  });
 });
 dom=new JSDOM(input.html,{url:u.href,runScripts:'dangerously',pretendToBeVisual:['mobile','mobile_visual'].includes(config.environment),cookieJar:jar,virtualConsole:vc,
  resources:{userAgent:config.user_agent||'HospitalAvailabilityHTTP/1.0',interceptors:[intercept]},
  beforeParse(window){
   hostIsolation.isolate(window);
   require('./helpers/event_constructor_runtime_20261109.cjs')(window);
   require('./helpers/deprecated_storage_quota_runtime_20261109_realm_190432_570734.cjs')(window);
   require('./helpers/document_all_runtime_20261109_checked_lazy.cjs').install(window);
   childWindowRuntime.register(window,child=>{hostIsolation.isolate(child);require('./helpers/event_constructor_runtime_20261109.cjs')(child);require('./helpers/deprecated_storage_quota_runtime_20261109_realm_190432_570734.cjs')(child);require('./helpers/document_all_runtime_20261109_checked_lazy.cjs').install(child);});
   applyClockOffset(window,config.clock_offset_ms);
   require('./helpers/canvas_method_named.cjs')(window);
   require('./helpers/script_text_audit/apply_script_innertext.cjs').applyScriptInnerText(window);
   require('./helpers/indexeddb_runtime.cjs').install(window);
   window.WebSocket=function(){throw new window.DOMException('WebSocket unavailable','NotSupportedError');};
   for(const [key,name]of [['local','localStorage'],['session','sessionStorage']])for(const [k,v]of storage[key])window[name].setItem(k,v);
   if(['mobile','mobile_visual'].includes(config.environment)){
    for(const [key,value]of Object.entries({innerWidth:412,innerHeight:915,outerWidth:412,outerHeight:915,devicePixelRatio:2}))Object.defineProperty(window,key,{configurable:true,value});
    Object.defineProperty(window.navigator,'maxTouchPoints',{configurable:true,value:5});
    Object.defineProperty(window.navigator,'platform',{configurable:true,value:'Linux armv8l'});
    // Explicit synthetic display values; not a captured native-browser fingerprint.
    Object.defineProperty(window.navigator,'language',{configurable:true,value:'zh-CN'});
    Object.defineProperty(window.navigator,'languages',{configurable:true,value:Object.freeze(['zh-CN','zh'])});
    for(const [key,value]of Object.entries({width:412,height:915,availWidth:412,availHeight:915,colorDepth:24,pixelDepth:24}))Object.defineProperty(window.screen,key,{configurable:true,value});
    if(window.screen.orientation){
     for(const [key,value]of Object.entries({type:'portrait-primary',angle:0}))Object.defineProperty(window.screen.orientation,key,{configurable:true,value});
    }
   }
   require('./helpers/network_information_runtime_20261109.cjs')(window);Object.defineProperty(window,'orientation',{configurable:true,enumerable:true,get(){return 0;}});Object.defineProperty(window,'innerWidth',{configurable:true,value:980});Object.defineProperty(window,'innerHeight',{configurable:true,value:2178});
   const location=idl.implForWrapper(window.location),original=location._locationObjectNavigate;
   location._locationObjectNavigate=function(record,options={}){
    if(ownGeneration!==generation)return;
    const target=whatwgURL.serializeURL(record);
    try{documentURL(target);}catch{errors.push({type:'navigation_rejected'});return;}
    if(navigationPending){errors.push({type:'navigation_already_pending'});return;}
    if(++navigationCount>2){errors.push({type:'navigation_limit'});return;}
    const id='navigation-'+navigationCount;
    navigationPending={id,url:target,replacement:!!options.replacement,generation:ownGeneration};
    emit({event:'navigation',...navigationPending,cookies:snapshot()});
    // Deliberately stop jsdom's no-op navigation; the host supplies the document.
   };
  }});
 const announce=()=>{if(closed||ownGeneration!==generation)return;ready=true;emit({event:'ready',generation:ownGeneration,navigation_pending:!!navigationPending,cookies:snapshot(),errors:[...errors],blocked_resources:[...blockedResources]});};
 if(dom.window.document.readyState==='complete')setTimeout(announce,0);
 else dom.window.addEventListener('load',()=>setTimeout(announce,0),{once:true});
}
function init(input){
 if(initialized)throw Error('already_initialized');initialized=true;documentURL(input.url);
 config={user_agent:input.user_agent,environment:input.environment||'default',clock_offset_ms:validateClockOffset(input.clock_offset_ms===undefined?0:input.clock_offset_ms)};
 jar=new CookieJar();seedCookieJar(jar,input.cookies||[],input.url);build(input);
}
function navigateDocument(input){
 if(!navigationPending)throw Error('no_navigation_pending');
 if(new URL(input.url).href!==navigationPending.url)throw Error('navigation_target_mismatch');
 if(input.id&&input.id!==navigationPending.id)throw Error('navigation_id_mismatch');
 documentURL(input.url);saveStorage();closeDOM();
 seedCookieJar(jar,input.cookies||[],input.url);navigationPending=null;build(input);
}
function response(input){
 const item=pending.get(input.id);if(!item)throw Error('unknown_http_response');pending.delete(input.id);clearTimeout(item.timer);
 try{
  if(!Number.isInteger(input.status)||input.status<200||input.status>599)throw Error('response_status_invalid');
  const headers=new Headers();for(const [k,v]of input.headers||[])headers.append(String(k),String(v));
  const raw=Buffer.from(input.body_b64||'','base64');if(raw.length>MAX)throw Error('response_oversize');
  item.resolve(new Response([204,205,304].includes(input.status)?null:raw,{status:input.status,headers}));
 }catch(e){item.resolve(new Response('',{status:502}));throw e;}
}
function query(input){
 if(navigationPending)throw Error('navigation_pending');
 if(!ready||!dom)throw Error('session_not_ready');if(activeQuery)throw Error('query_already_active');
 if(!allowed(input.url)||!['GET','POST'].includes(input.method))throw Error('query_target_rejected');
 if(typeof input.id!=='string'||input.id.length>100)throw Error('query_id_invalid');
 const xhr=new dom.window.XMLHttpRequest();let completed=false;
 const finish=failure=>{
  if(completed)return;completed=true;clearTimeout(activeQuery?.timer);activeQuery=null;
  emit(failure?{event:'query_result',id:input.id,status:0,error:failure,body:'',headers:'',cookies:snapshot()}:
   {event:'query_result',id:input.id,status:xhr.status,body:xhr.responseText,headers:xhr.getAllResponseHeaders(),cookies:snapshot()});
 };
 const timer=setTimeout(()=>{try{xhr.abort();}catch{}finish('query_timeout');},20_000);activeQuery={xhr,timer,expectedPath:new URL(input.url).pathname,expectedMethod:input.method};
 xhr.onload=()=>finish();xhr.onerror=()=>finish('xhr_error');xhr.onabort=()=>finish('xhr_aborted');
 try{
  xhr.open(input.method,input.url,true);xhr.timeout=20_000;
  for(const [k,v]of Array.isArray(input.headers)?input.headers:Object.entries(input.headers||{})){
   if(/^(?:authorization|proxy-authorization|cookie|host)$/i.test(k))throw Error('query_header_rejected');xhr.setRequestHeader(k,v);
  }
  const body=input.body==null?null:typeof input.body==='string'?input.body:JSON.stringify(input.body);
  if(body!==null&&Buffer.byteLength(body)>MAX)throw Error('query_body_oversize');xhr.send(body);
 }catch(e){finish(errorCode(e));}
}
const rl=readline.createInterface({input:process.stdin,crlfDelay:Infinity});
rl.on('line',line=>{
 try{
  if(line.length>8_000_000)throw Error('ipc_line_oversize');const input=JSON.parse(line);
  if(input.op==='init')init(input);
  else if(input.op==='navigate_document')navigateDocument(input);
  else if(input.op==='navigation_ack'){
   if(!navigationPending||input.id!==navigationPending.id)throw Error('navigation_id_mismatch');navigationPending=null;emit({event:'navigation_acknowledged'});
  }
  else if(input.op==='http_response')response(input);
  else if(input.op==='query')query(input);
  else if(input.op==='close'){emit({event:'closed'});shutdown();}
  else throw Error('unknown_operation');
 }catch(e){emit({event:'error',code:errorCode(e)});}
});
rl.on('close',shutdown);
process.on('uncaughtException',e=>{emit({event:'error',code:errorCode(e)});shutdown();});
process.on('unhandledRejection',e=>{emit({event:'error',code:errorCode(e)});shutdown();});
