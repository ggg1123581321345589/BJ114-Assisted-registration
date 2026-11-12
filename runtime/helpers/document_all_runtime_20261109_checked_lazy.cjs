 'use strict';
// EXPERIMENTAL: uses a private V8 test intrinsic in a synchronous process-global flag window.
// The intrinsic's built-in [[Call]] always returns null; HTMLAll call argument lookup is unsupported.
const v8=require('node:v8'),vm=require('node:vm');
const documents=new WeakMap(),brands=new WeakMap(),installations=new WeakMap();
function makeUndetectable(){try{v8.setFlagsFromString('--allow-natives-syntax');return vm.runInNewContext('%GetUndetectable()');}finally{v8.setFlagsFromString('--no-allow-natives-syntax');}}
module.exports=function installDocumentAll(w){
 if(installations.has(w))return installations.get(w);
 const collection=require('jsdom/lib/generated/idl/HTMLCollection.js');
 const idl=require('jsdom/lib/generated/idl/utils.js');
 const checked=makeUndetectable();if(typeof checked!=='undefined'||Boolean(checked)||!(checked==null)||checked===undefined)throw Error('undetectable_contract_failed');let closed=false;try{w.Function('return %GetUndetectable();');}catch(e){closed=e.name==='SyntaxError';}if(!closed)throw Error('native_syntax_flag_not_closed');
 const proto=Object.create(w.Object.prototype);
 function requireState(receiver){const state=brands.get(receiver);if(!state)throw new w.TypeError('Illegal invocation');return state;}
 function find(state,key){if(key==='')return null;const matches=[];for(const el of state.document.getElementsByTagName('*')){if(el.id===key||el.getAttribute('name')===key)matches.push(el);}if(matches.length===0)return null;if(matches.length===1)return matches[0];if(!state.named.has(key)){state.named.set(key,collection.create(w,[],{element:idl.implForWrapper(state.document),query:()=>Array.from(state.document.getElementsByTagName('*')).filter(el=>el.id===key||el.getAttribute('name')===key).map(el=>idl.implForWrapper(el))}));}return state.named.get(key);}
 Object.defineProperties(proto,{length:{configurable:true,enumerable:true,get(){return requireState(this).document.getElementsByTagName('*').length;}},item:{configurable:true,enumerable:true,writable:true,value:function item(index){const state=requireState(this);if(arguments.length<1)throw new w.TypeError('1 argument required');if(typeof index==='symbol')throw new w.TypeError('Cannot convert a Symbol value');const text=String(index);if(/^(?:0|[1-9][0-9]*)$/.test(text)){const n=Number(text);return state.document.getElementsByTagName('*').item(n);}return find(state,text);}},namedItem:{configurable:true,enumerable:true,writable:true,value:function namedItem(name){const state=requireState(this);if(arguments.length<1)throw new w.TypeError('1 argument required');if(typeof name==='symbol')throw new w.TypeError('Cannot convert a Symbol value');return find(state,String(name));}},[w.Symbol.toStringTag]:{configurable:true,value:'HTMLAllCollection'}});
 const dynamic=new Proxy(proto,{get(target,key,receiver){if(Reflect.has(target,key))return Reflect.get(target,key,receiver);const state=brands.get(receiver);if(!state||typeof key!=='string')return undefined;if(/^(?:0|[1-9][0-9]*)$/.test(key))return state.document.getElementsByTagName('*').item(Number(key))||undefined;return find(state,key)||undefined;},has(target,key){return Reflect.has(target,key);}});
 const getter=function all(){if(!w.Document.prototype.isPrototypeOf(this))throw new w.TypeError('Illegal invocation');let value=documents.get(this);if(!documents.has(this)){value=this===w.document?checked:makeUndetectable();const state={document:this,named:new Map()};brands.set(value,state);Object.setPrototypeOf(value,dynamic);for(const key of ['length','name','arguments','caller','prototype']){const desc=Object.getOwnPropertyDescriptor(value,key);if(desc&&desc.configurable)delete value[key];}documents.set(this,value);}return value;};
 Object.defineProperty(w.Document.prototype,'all',{configurable:true,enumerable:true,get:getter});
 const metadata={tested_node:process.version,tested_v8:process.versions.v8,backend:'experimental_v8_GetUndetectable',callable:true,call_argument_dispatch_supported:false,full_HTMLAllCollection_contract:false,process_global_flag_window:'synchronous allow then finally no-allow',native_toString_modified:false};installations.set(w,metadata);return metadata;
};
module.exports.capabilities={experimental:true,full_HTMLAllCollection_contract:false,unsupported:['call arguments dispatch','exotic ownKeys/own descriptor','complete named-property precedence','realm native builtin identity'],requires_v8_private_test_intrinsic:true};

module.exports.install=module.exports;
