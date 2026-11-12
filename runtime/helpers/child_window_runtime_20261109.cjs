'use strict';
// Apply a registered initializer only to child windows after native construction.
// Shared jsdom settings carry the registration across nested frame creation.
const path=require('node:path');
const initializers=new WeakMap();
let installed=false,hook;
function install(){
 if(installed)return hook;
 const windows=require(require.resolve("jsdom/lib/jsdom/browser/Window.js"));
 const original=windows.createWindow;
 if(typeof original!=='function')throw Error('child_window_createWindow_missing');
 hook=function createWindow(options){
  const w=Reflect.apply(original,this,arguments);
  const documentOptions=options&&options.documentOptions;
  const settings=options&&options.settings;
  if(documentOptions&&Object.hasOwn(documentOptions,'parentOrigin')&&settings&&typeof settings==='object'){
   const initializer=initializers.get(settings);
   if(initializer)initializer(w);
  }
  return w;
 };
 windows.createWindow=hook;
 installed=true;
 return hook;
}
function register(mainWindow,initializer){
 if(!mainWindow||(typeof mainWindow!=='object'&&typeof mainWindow!=='function'))throw new TypeError('child_window_mainWindow_required');
 if(typeof initializer!=='function')throw new TypeError('child_window_initializer_required');
 const settings=mainWindow._settings;
 if(!settings||typeof settings!=='object'||!mainWindow.document||mainWindow.document.defaultView!==mainWindow||mainWindow.parent!==mainWindow)throw new TypeError('child_window_mainWindow_invalid');
 const prior=initializers.get(settings);
 if(prior&&prior!==initializer)throw Error('child_window_initializer_already_registered');
 install();
 initializers.set(settings,initializer);
 return mainWindow;
}
module.exports={install,register};
