'use strict';
// Synthetic documents and host-supplied responses only. Never opens a socket.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {spawn}=require('node:child_process');
const root=path.resolve(__dirname,'..');
const engine=path.join(__dirname,'query_runtime.cjs');
const checks=[];
function check(name,fn){fn();checks.push(name);}
let child;
async function main(){
 check('runtime_hash_matches_cpp',()=>{
  const hash=crypto.createHash('sha256').update(fs.readFileSync(engine)).digest('hex');
  assert.ok(fs.readFileSync(path.join(root,'src/native_backend.cpp'),'utf8').includes('FAST_HASH("'+hash+'")'));
 });
 check('public_headers_have_no_credentials',()=>{
  const fixtures=JSON.parse(fs.readFileSync(path.join(__dirname,'public_headers.json'),'utf8')).browser_network_trace.requests;
  assert.ok(fixtures.length>=3);
  const permitted=new Set(['accept','accept-language','user-agent','sec-fetch-dest','sec-fetch-mode','sec-fetch-site','sec-ch-ua','sec-ch-ua-mobile','sec-ch-ua-platform']);
  for(const fixture of fixtures)for(const [key,value]of Object.entries(fixture.request_extra_headers)){assert.ok(permitted.has(key.toLowerCase()));assert.equal(typeof value,'string');}
 });
 child=spawn(process.execPath,[engine],{cwd:root,stdio:['pipe','pipe','pipe']});
 let buffered='',stderr='',events=[],waiters=[];
 child.stdout.setEncoding('utf8');child.stderr.setEncoding('utf8');
 child.stderr.on('data',data=>{stderr+=data;});
 child.stdout.on('data',data=>{
  buffered+=data;let end;
  while((end=buffered.indexOf('\n'))>=0){
   const line=buffered.slice(0,end);buffered=buffered.slice(end+1);if(!line)continue;
   try{events.push(JSON.parse(line));}catch(error){for(const w of waiters)w.reject(error);waiters=[];return;}
  }
  for(const w of [...waiters]){const i=events.findIndex(w.match);if(i>=0){waiters.splice(waiters.indexOf(w),1);w.resolve(events.splice(i,1)[0]);}}
 });
 const exited=new Promise((resolve,reject)=>{child.once('error',reject);child.once('exit',(code,signal)=>resolve({code,signal}));});
 const send=value=>child.stdin.write(JSON.stringify(value)+'\n');
 const next=(match,timeout=10000)=>new Promise((resolve,reject)=>{
  const i=events.findIndex(match);if(i>=0){resolve(events.splice(i,1)[0]);return;}
  const waiter={match,resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}};
  const timer=setTimeout(()=>{const i=waiters.indexOf(waiter);if(i>=0)waiters.splice(i,1);reject(Error('runtime_event_timeout: '+JSON.stringify(events)+' stderr='+stderr));},timeout);
  waiters.push(waiter);
 });
 send({op:'init',url:'https://www.114yygh.com/newhlwyl/mobile/appointmentRegisterHome?pathchannel=bjt',html:'<!doctype html><html><head><title>Offline fixture</title></head><body>fixture</body></html>',external:'',cookies:[]});
 const ready=await next(e=>e.event==='ready'||e.event==='error');
 check('offline_dom_ready',()=>{assert.equal(ready.event,'ready',JSON.stringify(ready));assert.equal(ready.generation,1);assert.deepEqual(ready.errors,[]);assert.deepEqual(ready.cookies,[]);});
 send({op:'query',id:'forbidden',method:'GET',url:'https://www.114yygh.com/jtjk/mobile-service/patient/list'});
 const rejected=await next(e=>e.event==='error');
 check('account_endpoint_rejected',()=>assert.equal(rejected.code,'query_target_rejected'));
 send({op:'query',id:'fixture-query',method:'GET',url:'https://www.114yygh.com/jtjk/mobile-service/department/list/H01'});
 const request=await next(e=>e.event==='http_request'||e.event==='error');
 check('query_uses_host_transport',()=>{assert.equal(request.event,'http_request',JSON.stringify(request));assert.equal(request.method,'GET');assert.equal(new URL(request.url).pathname,'/jtjk/mobile-service/department/list/H01');});
 const responseBody=JSON.stringify({code:'0000',data:[]});
 send({op:'http_response',id:request.id,status:200,headers:[['Content-Type','application/json']],body_b64:Buffer.from(responseBody).toString('base64')});
 const result=await next(e=>e.event==='query_result'||e.event==='error');
 check('synthetic_query_result',()=>{assert.equal(result.event,'query_result');assert.equal(result.id,'fixture-query');assert.equal(result.status,200);assert.equal(result.body,responseBody);});
 send({op:'close'});
 await next(e=>e.event==='closed');
 const exit=await exited;
 check('clean_shutdown',()=>{assert.equal(exit.code,0);assert.equal(exit.signal,null);assert.equal(stderr,'');});
 process.stdout.write(JSON.stringify({status:'PASS',website_requests:0,synthetic_http_responses:1,checks,passed:checks.length})+'\n');
}
main().catch(error=>{if(child)child.kill();process.stderr.write((error.stack||String(error))+'\n');process.exitCode=1;});
