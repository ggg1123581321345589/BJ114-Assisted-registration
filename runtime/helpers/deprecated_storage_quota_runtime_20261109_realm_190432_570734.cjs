'use strict';
// Unsupported storage backend: this layer exposes observed legacy API structure only.
// It never reports successful quota allocation and does not claim HTTPS Chrome equivalence.
const installed=new WeakMap();
module.exports=function installDeprecatedStorageQuota(w){
 if(installed.has(w))return installed.get(w);
 const storageBrands=new WeakSet(),errorStates=new WeakMap();
 const toNumber=w.Function('value','return +value;');
 const failType=()=>{throw new w.TypeError('Illegal invocation');};
 function DOMError(name){errorStates.set(this,{name,message:name==='NotSupportedError'?'Storage backend is unsupported.':'Quota allocation is unsupported.'});}
 const errorPrototype=w.Object.create(w.Object.prototype);
 w.Object.defineProperties(errorPrototype,{constructor:{value:DOMError,writable:true,configurable:true},name:{get:function name(){const s=errorStates.get(this);if(!s)return failType();return s.name;},enumerable:true,configurable:true},message:{get:function message(){const s=errorStates.get(this);if(!s)return failType();return s.message;},enumerable:true,configurable:true},[w.Symbol.toStringTag]:{value:'DOMError',configurable:true}});
 DOMError.prototype=errorPrototype;
 function validateCallback(value,required,label){if(value===null||value===undefined){if(required)throw new w.TypeError(label+' must be a function.');return null;}if(typeof value!=='function')throw new w.TypeError(label+' must be a function.');return value;}
 function reportUnsupported(callback,name){if(callback)w.setTimeout(()=>{Reflect.apply(callback,undefined,[new DOMError(name)]);},0);}
 const methods={
  queryUsageAndQuota(successCallback){
   if(!storageBrands.has(this))return failType();
   if(arguments.length<1)throw new w.TypeError('queryUsageAndQuota requires one argument.');
   validateCallback(successCallback,true,'successCallback');
   const errorCallback=validateCallback(arguments[1],false,'errorCallback');
   reportUnsupported(errorCallback,'NotSupportedError');
  },
  requestQuota(requestedBytes){
   if(!storageBrands.has(this))return failType();
   if(arguments.length<1)throw new w.TypeError('requestQuota requires one argument.');
   // WebIDL unsigned long long conversion accepts nonfinite/negative values;
   // unary plus preserves the observed BigInt and Symbol TypeError semantics.
   const converted=toNumber(requestedBytes);
   let unsignedBytes=!Number.isFinite(converted)||converted===0?0:Math.trunc(converted)%18446744073709551616;
   if(unsignedBytes<0)unsignedBytes+=18446744073709551616;
   void unsignedBytes;
   validateCallback(arguments[1],false,'successCallback');
   const errorCallback=validateCallback(arguments[2],false,'errorCallback');
   reportUnsupported(errorCallback,'AbortError');
  }
 };
 const quotaPrototype=w.Object.create(w.Object.prototype);
 for(const key of ['queryUsageAndQuota','requestQuota'])w.Object.defineProperty(quotaPrototype,key,{value:methods[key],writable:true,enumerable:true,configurable:true});
 w.Object.defineProperty(quotaPrototype,w.Symbol.toStringTag,{value:'DeprecatedStorageQuota',configurable:true});
 const values={};
 for(const key of ['webkitPersistentStorage','webkitTemporaryStorage']){
  const value=w.Object.create(quotaPrototype);storageBrands.add(value);values[key]=value;
  const getter=w.Object.getOwnPropertyDescriptor({get [key](){if(this!==w.navigator)return failType();return value;}},key).get;
  w.Object.defineProperty(w.Navigator.prototype,key,{get:getter,enumerable:true,configurable:true});
 }
 const installation={backend:'unsupported',success_quota_values_fabricated:false,https_chrome_equivalence:false,properties:Object.keys(values)};
 installed.set(w,installation);return installation;
};
