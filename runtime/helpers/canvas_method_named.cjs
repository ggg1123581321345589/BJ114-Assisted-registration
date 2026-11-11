'use strict';
// Explicit two-method source-representation adapter. This does not create native Chrome methods.
module.exports=function installNamedCanvasMethodAdapter(window){
 const proto=window.HTMLCanvasElement.prototype;
 for(const name of ['toBlob','toDataURL']){
  const descriptor=Object.getOwnPropertyDescriptor(proto,name),original=descriptor?.value;
  if(typeof original!=='function')throw Error('canvas_method_missing');
  const sourceMethod=original.toString;let proxy;
  const sourceProxy=new Proxy(sourceMethod,{apply(target,receiver,args){
   if(receiver===proxy)return 'function '+name+'() { [native code] }';
   return Reflect.apply(target,receiver,args);
  }});
  proxy=new Proxy(original,{
   apply(target,receiver,args){return Reflect.apply(target,receiver,args);},
   construct(target,args,newTarget){return Reflect.construct(target,args,newTarget);},
   get(target,key,receiver){return key==='toString'?sourceProxy:Reflect.get(target,key,receiver);}
  });
  Object.defineProperty(proto,name,{...descriptor,value:proxy});
 }
};
