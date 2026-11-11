'use strict';
// Ordinary realm function preserves construction semantics without replacing toString.
const installed=new WeakMap();
module.exports=function installEventConstructor(w){
 if(installed.has(w))return installed.get(w);
 const Original=w.Event;
 if(typeof Original!=='function')throw Error('event_constructor_missing');
 const Event=w.Function('Original', 'return function Event(type){if(!new.target)throw new TypeError("Class constructor Event cannot be invoked without new");return Reflect.construct(Original,arguments,new.target);};')(Original);
 Object.setPrototypeOf(Event,Object.getPrototypeOf(Original));
 const descriptors=Object.getOwnPropertyDescriptors(Original);
 for(const key of Reflect.ownKeys(descriptors))if(!['length','name','arguments','caller','prototype'].includes(key))Object.defineProperty(Event,key,descriptors[key]);
 Object.defineProperty(Event,'prototype',{...Object.getOwnPropertyDescriptor(Original,'prototype'),value:Original.prototype});
 Object.defineProperty(Original.prototype,'constructor',{...Object.getOwnPropertyDescriptor(Original.prototype,'constructor'),value:Event});
 for(const key of Object.getOwnPropertyNames(w)){
  const desc=Object.getOwnPropertyDescriptor(w,key),value=desc&&'value'in desc?desc.value:null;
  if(typeof value==='function'&&value!==Original&&Object.getPrototypeOf(value)===Original)Object.setPrototypeOf(value,Event);
 }
 Object.defineProperty(w,'Event',{...Object.getOwnPropertyDescriptor(w,'Event'),value:Event});
 installed.set(w,Event);
 return Event;
};
