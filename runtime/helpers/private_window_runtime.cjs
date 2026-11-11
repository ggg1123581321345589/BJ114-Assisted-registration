'use strict';
// Migrate four internal Window global-object reads to a private host WeakMap.
// No site source or installed node_modules file is changed.
const fs=require('node:fs'),Module=require('node:module'),path=require('node:path');
const baseline=Module._extensions['.js'];
const globals=new WeakMap();let installed=false;
const targets=new Set([
 'living/helpers/events.js','living/helpers/focusing.js',
 'living/helpers/runtime-script-errors.js','living/events/EventTarget-impl.js'
].map(p=>require.resolve('jsdom/lib/jsdom/'+p)));
const resolve=target=>globals.get(target)||target._globalObject;
function install(){
 if(installed)return;installed=true;
 for(const filename of targets)if(require.cache[filename])throw Error('host_global_isolation_loaded_too_late');
 Module._extensions['.js']=function(module,filename){
  if(!targets.has(filename))return baseline(module,filename);
  const raw=fs.readFileSync(filename,'utf8');
  if(!raw.includes('target._globalObject'))throw Error('host_global_isolation_source_mismatch');
  const token='__hostResolveWindowGlobal_76a8';
  const source='const '+token+' = module.__hostWindowGlobalResolver;\n'+raw.replaceAll('target._globalObject',token+'(target)');
  Object.defineProperty(module,'__hostWindowGlobalResolver',{value:resolve});
  module._compile(source,filename);
 };
}
function isolate(window){
 const context=window._globalObject;
 if(context!==window)throw Error('host_global_isolation_identity_mismatch');
 globals.set(window,context);
 if(!Reflect.deleteProperty(window,'_globalObject'))throw Error('host_global_isolation_delete_failed');
 if(typeof window._globalObject!=='undefined')throw Error('host_global_isolation_still_exposed');
}
module.exports={install,isolate};
