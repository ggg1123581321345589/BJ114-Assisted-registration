'use strict';
// Offline Network Information API compatibility. Type is unspecified on desktop.
module.exports=function installNetworkInformation(w){
 if(w.navigator.connection!==undefined)return;
 const state=new WeakMap();
 class NetworkInformation extends w.EventTarget{constructor(){super();state.set(this,{onchange:null,effectiveType:'4g',rtt:0,downlink:10,saveData:false});}}
 for(const key of ['onchange','effectiveType','rtt','downlink','saveData']){
  const descriptor={enumerable:true,configurable:true,get(){if(!state.has(this))throw new w.TypeError('Illegal invocation');return state.get(this)[key];}};
  if(key==='onchange')descriptor.set=function(value){if(!state.has(this))throw new w.TypeError('Illegal invocation');state.get(this).onchange=typeof value==='function'?value:null;};
  Object.defineProperty(NetworkInformation.prototype,key,descriptor);
 }
 Object.defineProperty(NetworkInformation.prototype,Symbol.toStringTag,{value:'NetworkInformation',configurable:true});
 const connection=new NetworkInformation();
 Object.defineProperty(w.Navigator.prototype,'connection',{enumerable:true,configurable:true,get(){if(this!==w.navigator)throw new w.TypeError('Illegal invocation');return connection;}});
 Object.defineProperty(w,'NetworkInformation',{value:NetworkInformation,writable:true,configurable:true});
};
