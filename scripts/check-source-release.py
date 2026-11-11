"""Check the curated source tree, not installed dependencies/build outputs."""
from pathlib import Path
import hashlib
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP = {'node_modules', '.git', 'build', 'build-cmake', 'bin', '__pycache__', '.wireit'}
BAD_SUFFIX = {'.exe', '.dll', '.obj', '.pdb', '.pyc', '.log', '.har'}
def files():
    return sorted(p for p in ROOT.rglob('*') if p.is_file() and not any(part in SKIP or part.startswith('build-') or part.startswith('build_') for part in p.relative_to(ROOT).parts))
def main():
    issues=[]
    for p in files():
        name=p.relative_to(ROOT).as_posix()
        if p.suffix.lower() in BAD_SUFFIX:issues.append('binary/session output: '+name)
        if any(s in name.lower() for s in ('desktop_result','cookie_jar','session.json','account.json')):issues.append('private-looking file: '+name)
        if p.suffix.lower() in {'.cpp','.h','.cjs','.js','.json','.cmd','.pro','.txt'} and 'vendor/' not in name:
            body=p.read_text(encoding='utf-8-sig',errors='replace')
            if re.search(r'[CD]:[/\\](?:Users|Anaconda3|Visual Studio|Program Files|抢号软件开发)',body,re.I):issues.append('machine-specific path: '+name)
    for name in ('src/main.cpp','src/native_backend.cpp','src/native_backend.h','src/native_service.cpp','src/native_service.h','data/hospitals.csv','runtime/query_runtime.cjs','runtime/public_headers.json','package-lock.json','vendor/jsdom/LICENSE.txt','README.md','CMakeLists.txt','AssistedRegistration.pro'):
        if not (ROOT/name).is_file():issues.append('missing: '+name)
    main_cpp=(ROOT/'src/main.cpp').read_text(encoding='utf-8')
    if '辅助挂号软件v1.0.0' not in main_cpp or '免责说明' not in main_cpp:issues.append('branding missing')
    print(json.dumps({'status':'PASS' if not issues else 'FAIL','files':len(files()),'issues':issues},ensure_ascii=False))
    return bool(issues)
if __name__=='__main__':sys.exit(main())
