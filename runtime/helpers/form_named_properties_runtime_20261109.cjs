"use strict";
// Scoped current-control named lookup; no HTMLDDA/document.all or past-names emulation.
const fs=require('node:fs'),Module=require('node:module'),path=require('node:path');
const target=require.resolve('jsdom/lib/generated/idl/HTMLFormElement.js');
let installed=false;
function named(impl,key,wrapper,globalObject){
 if(typeof key!=='string'||key===''||/^(0|[1-9][0-9]*)$/.test(key))return null;
 const own=Reflect.getOwnPropertyDescriptor(wrapper,key);
 // A Proxy must preserve nonconfigurable target-property invariants.
 if(own&&!own.configurable)return null;
 const query=()=>impl._getElementNodes().filter(node=>node.getAttributeNS(null,'id')===key||node.getAttributeNS(null,'name')===key);
 const matches=query();
 if(matches.length===0)return null;
 if(matches.length===1)return matches[0];
 const RadioNodeList=require(path.resolve(__dirname,'../../node_modules/jsdom/lib/generated/idl/RadioNodeList.js'));
 return RadioNodeList.createImpl(globalObject,[],{element:impl.getRootNode(),query});
}
function install(){
 if(installed)return;
 if(require.cache[target])throw Error('form_named_properties_loaded_too_late');
 const previous=Module._extensions['.js'];
 Module._extensions['.js']=function(module,filename){
  if(filename!==target)return previous(module,filename);
  let source=fs.readFileSync(filename,'utf8');
  const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('form_named_properties_source_mismatch');source=source.replace(from,to);};
  replace('    return Reflect.get(target, P, receiver);\n  }\n\n  has(target, P)',
          '    const namedValue = __formNamedLookup_20261109(impl, P, target, this._globalObject);\n    if (namedValue !== null) return utils.tryWrapperForImpl(namedValue);\n    return Reflect.get(target, P, receiver);\n  }\n\n  has(target, P)');
  replace('    const desc = this.getOwnPropertyDescriptor(target, P);',
          '    // Named properties override configurable own expandos and inherited builtins.\n    const desc = this.getOwnPropertyDescriptor(target, P);');
  replace('    for (const key of Reflect.ownKeys(target)) {\n      keys.add(key);\n    }\n    return [...keys];\n  }',
          '    // Chrome exposes named descriptors but omits synthetic names from ownKeys.\n    for (const key of Reflect.ownKeys(target)) {\n      keys.add(key);\n    }\n    return [...keys];\n  }');
  replace('    return Reflect.getOwnPropertyDescriptor(target, P);\n  }\n\n  set(target, P, V, receiver)',
          '    const namedValue = __formNamedLookup_20261109(impl, P, target, this._globalObject);\n    if (namedValue !== null) return { configurable: true, enumerable: false, writable: false, value: utils.tryWrapperForImpl(namedValue) };\n    return Reflect.getOwnPropertyDescriptor(target, P);\n  }\n\n  set(target, P, V, receiver)');
  Object.defineProperty(module,'__formNamedLookup_20261109',{value:named});
  module._compile('const __formNamedLookup_20261109 = module.__formNamedLookup_20261109;\n'+source,filename);
 };
 installed=true;
}
module.exports={install};
