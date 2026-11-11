'use strict';
// Explicit sandbox clock configuration. Does not alter OS time or explicit dates.
function validateClockOffset(offset){
 if(typeof offset!=='number'||!Number.isFinite(offset)||Math.abs(offset)>300000)throw Error('clock_offset_invalid');
 return offset;
}
function applyClockOffset(window,offset){
 validateClockOffset(offset);
 const NativeDate=window.Date;
 const nativeNow=NativeDate.now;
 const now=()=>Reflect.apply(nativeNow,NativeDate,[])+offset;
 // The actual getter/functions and Date.prototype remain unchanged. Proxy behavior
 // is intentionally limited to current-time APIs, with no forged stringification.
 let clock;
 clock=new Proxy(NativeDate,{
  apply(target,thisArg,args){return new target(now()).toString();},
  construct(target,args,newTarget){return Reflect.construct(target,args.length?args:[now()],newTarget===clock?target:newTarget);},
  get(target,key,receiver){return key==='now'?now:Reflect.get(target,key,receiver);}
 });
 window.Date=clock;
 return {offset_ms:offset,nativeDate:NativeDate,date:clock};
}
module.exports={applyClockOffset,validateClockOffset};
