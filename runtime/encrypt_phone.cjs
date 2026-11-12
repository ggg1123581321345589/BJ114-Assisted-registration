#!/usr/bin/env node
'use strict';
/**
 * 手机号加密子进程：通过 stdin 接收单个 JSON，通过 stdout 返回密文。
 * 使用上游 MIT 开源 sm-crypto，避免重新分发许可不明确的站点压缩 JS。
 * PUBLIC_KEY 是服务端公开加密公钥，不是私钥、Token 或某个用户的账号凭据。
 * cipherMode=1 使用 C1C3C2；原协议在算法返回值前再加 04 点标识。
 * 不记录手机号或密文；所有异常只输出固定错误名。
 */
const { sm2 } = require('sm-crypto');
const PUBLIC_KEY = '04c7af84e126c3b07fdc20232b314d41ed4fd9b1fbe1a683a59c8cedc3dc0394060d89bbe5b7229761849ca20b615a408c1e9604d315ab0a01ea6d357e98d8fbc6';
const PHONE = /^(?:1[3-9]\d{9}|0(?:[12]\d|[3-9]\d{2})\d{7,8})$/;
function encryptPhone(phone) {
  if (typeof phone !== 'string' || !PHONE.test(phone)) throw Error('invalid_input');
  const ciphertext = '04' + sm2.doEncrypt(phone, PUBLIC_KEY, 1);
  const expected = 2 + 128 + 64 + Buffer.byteLength(phone, 'utf8') * 2;
  if (!/^04[0-9a-f]+$/i.test(ciphertext) || ciphertext.length !== expected) throw Error('invalid_ciphertext');
  return ciphertext;
}
function fail(code) { process.stderr.write('phone_encryption_failed\n'); process.exitCode = code; }
function selfTest() {
  // 仅用合成输入；随机密文每次不同，不能用固定密文进行等值测试。
  const synthetic = '13800000000', first = encryptPhone(synthetic), second = encryptPhone(synthetic);
  const pair = sm2.generateKeyPairHex();
  const roundTrip = sm2.doDecrypt(sm2.doEncrypt(synthetic, pair.publicKey, 1), pair.privateKey, 1);
  const checks = { format: /^04[0-9a-f]+$/i.test(first), randomness: first !== second, length: first.length === 216, roundTrip: roundTrip === synthetic };
  const ok = Object.values(checks).every(Boolean);
  process.stdout.write(JSON.stringify({ selfTest: ok ? 'PASS' : 'FAIL', ...checks }) + '\n');
  if (!ok) process.exitCode = 1;
}
if (require.main === module) {
  if (process.argv.length === 3 && process.argv[2] === '--self-test') { try { selfTest(); } catch { fail(1); } }
  else if (process.argv.length !== 2) fail(2);
  else {
    // 1KB 输入上限。额外字段、数组、畸形 JSON 一律拒绝，stdout 不回显输入。
    let raw = '', bytes = 0, rejected = false;
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', chunk => {
      if (rejected) return;
      bytes += Buffer.byteLength(chunk, 'utf8');
      if (bytes > 1024) { rejected = true; raw = ''; fail(2); return; }
      raw += chunk;
    });
    process.stdin.on('end', () => {
      if (rejected) return;
      try {
        const input = JSON.parse(raw); raw = '';
        if (!input || typeof input !== 'object' || Array.isArray(input) || Object.keys(input).length !== 1 || !Object.hasOwn(input, 'phone')) throw Error('invalid_input');
        const encryptedPhone = encryptPhone(input.phone); input.phone = '';
        process.stdout.write(JSON.stringify({ encryptedPhone }) + '\n');
      } catch { raw = ''; fail(2); }
    });
    process.stdin.on('error', () => { raw = ''; fail(2); });
  }
}
module.exports = { encryptPhone };
