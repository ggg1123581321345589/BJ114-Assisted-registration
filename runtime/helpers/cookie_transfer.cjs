'use strict';
// Pure CookieJar helpers. The exported snapshots contain secrets: private IPC only.
const canonicalDomain=value=>String(value||'').toLowerCase().replace(/^\.+/,'').replace(/\.$/,'');

function seedCookieJar(jar,cookies,url){
  for(const cookie of cookies||[]){
    const key=cookie.key??cookie.name,domain=canonicalDomain(cookie.domain),path=cookie.path||'/';
    if(typeof key!=='string'||!key||typeof cookie.value!=='string'||!domain||!path.startsWith('/'))throw Error('cookie_attributes_invalid');
    const hostOnly=cookie.hostOnly??(typeof cookie.domainSpecified==='boolean'?!cookie.domainSpecified:undefined);
    if(typeof hostOnly!=='boolean')throw Error('cookie_host_only_attribute_missing');
    let text=key+'='+cookie.value+'; Path='+path;
    if(!hostOnly)text+='; Domain='+domain;
    if(cookie.secure)text+='; Secure';
    if(cookie.httpOnly===true)text+='; HttpOnly';
    if(cookie.sameSite!=null){
      const sameSite=String(cookie.sameSite).toLowerCase();
      if(!['strict','lax','none'].includes(sameSite))throw Error('cookie_samesite_invalid');
      text+='; SameSite='+sameSite;
    }
    if(cookie.expires!=null){
      if(typeof cookie.expires!=='number'||!Number.isFinite(cookie.expires))throw Error('cookie_expiry_invalid');
      text+='; Expires='+new Date(cookie.expires*1000).toUTCString();
    }
    const target=new URL(url);
    if(hostOnly&&target.hostname.toLowerCase()!==domain)throw Error('cookie_host_only_origin_mismatch');
    jar.setCookieSync(text,target.href,{http:true});
  }
  return jar;
}

function exportCookieJar(jar){
  // serializeSync includes all domain/path tuples; getCookiesSync(entry) drops API-only paths.
  const expiry=cookie=>{
    if(cookie.maxAge!=null&&cookie.maxAge!=='Infinity'){
      const age=Number(cookie.maxAge);
      if(age===-Infinity||age<=0)return 0;
      if(Number.isFinite(age))return Date.parse(cookie.creation)/1000+age;
    }
    return cookie.expires&&cookie.expires!=='Infinity'?Date.parse(cookie.expires)/1000:null;
  };
  return jar.serializeSync().cookies.filter(cookie=>expiry(cookie)===null||expiry(cookie)>Date.now()/1000).map(cookie=>({
    key:cookie.key,value:cookie.value??'',domain:cookie.domain,path:cookie.path||'/',
    secure:cookie.secure===true,httpOnly:cookie.httpOnly===true,
    hostOnly:cookie.hostOnly===true,domainSpecified:cookie.hostOnly!==true,
    domainInitialDot:false,pathSpecified:cookie.pathIsDefault!==true,
    expires:expiry(cookie),
    discard:expiry(cookie)===null,sameSite:cookie.sameSite??null,
    creation:cookie.creation??null,lastAccessed:cookie.lastAccessed??null
  }));
}

module.exports={canonicalDomain,seedCookieJar,exportCookieJar};
