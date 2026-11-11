'use strict';

// Per-runtime-process factory: the navigation engine replaces documents only
// within its validated www.114yygh.com origin, so one factory preserves state.
const implementation=require('../../node_modules/fake-indexeddb');
const factory=new implementation.IDBFactory();
const constructors={
 IDBFactory:implementation.IDBFactory,
 IDBDatabase:implementation.IDBDatabase,
 IDBObjectStore:implementation.IDBObjectStore,
 IDBIndex:implementation.IDBIndex,
 IDBCursor:implementation.IDBCursor,
 IDBCursorWithValue:implementation.IDBCursorWithValue,
 IDBKeyRange:implementation.IDBKeyRange,
 IDBRequest:implementation.IDBRequest,
 IDBOpenDBRequest:implementation.IDBOpenDBRequest,
 IDBTransaction:implementation.IDBTransaction,
 IDBVersionChangeEvent:implementation.IDBVersionChangeEvent
};

function install(window){
 if(!window||!window.Window)throw new TypeError('window_required');
 Object.defineProperty(window.Window.prototype,'indexedDB',{
  configurable:true,
  enumerable:true,
  get(){return factory;}
 });
 for(const [name,constructor]of Object.entries(constructors)){
  Object.defineProperty(window,name,{configurable:true,enumerable:true,writable:true,value:constructor});
 }
}

module.exports={install,factory};
