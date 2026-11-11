'use strict';
// Preserve XPath evaluation while moving AST/document state off public expressions.
// Runtime-only loader transform; installed jsdom files and challenge code stay intact.
const fs=require('node:fs'),Module=require('node:module'),path=require('node:path');
const TARGET=require.resolve('jsdom/lib/jsdom/level3/xpath.js');
let installed=false;
function install(){
 if(installed)return;
 if(require.cache[TARGET])throw Error('xpath_private_ast_loaded_too_late');
 const previous=Module._extensions['.js'];
 Module._extensions['.js']=function(module,filename){
  if(filename!==TARGET)return previous(module,filename);
  const raw=fs.readFileSync(filename,'utf8');
  const write='this._ast = parse(stream, astFactory);';
  const read='evaluator.val(this._ast, {nodes: [contextNode]})';
  const docWrite='this._doc = optDoc;';
  const docRead='null != this._doc && this._doc !== doc';
  if(raw.split(write).length!==2||raw.split(read).length!==2||raw.split(docWrite).length!==2||raw.split(docRead).length!==2)throw Error('xpath_private_ast_source_mismatch');
  const transformed='const __privateXPathAst_20261109 = new WeakMap(), __privateXPathDoc_20261109 = new WeakMap();'+String.fromCharCode(10)+raw.replace(write,'__privateXPathAst_20261109.set(this, parse(stream, astFactory));').replace(read,'evaluator.val(__privateXPathAst_20261109.get(this), {nodes: [contextNode]})').replace(docWrite,'__privateXPathDoc_20261109.set(this, optDoc);').replace(docRead,'null != __privateXPathDoc_20261109.get(this) && __privateXPathDoc_20261109.get(this) !== doc');
  module._compile(transformed,filename);
 };
 installed=true;
}
module.exports={install};
