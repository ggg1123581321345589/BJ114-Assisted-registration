'use strict';
// Narrow read-only fallback for non-rendered HTML script elements in jsdom.
// Delegates to the existing Node.textContent getter; it does not change page text.
function applyScriptInnerText(window){
 const prototype=window.HTMLScriptElement.prototype;
 if('innerText' in prototype)throw Error('script_innertext_already_exists');
 const nativeGetter=Object.getOwnPropertyDescriptor(window.Node.prototype,'textContent')?.get;
 if(typeof nativeGetter!=='function')throw Error('node_textcontent_getter_unavailable');
 Object.defineProperty(prototype,'innerText',{configurable:true,enumerable:true,get(){return Reflect.apply(nativeGetter,this,[]);}});
 return {scope:'HTMLScriptElement.prototype only',read_only:true};
}
module.exports={applyScriptInnerText};
